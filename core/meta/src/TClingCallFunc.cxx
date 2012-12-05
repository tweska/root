// @(#)root/core/meta:$Id$
// Author: Paul Russo   30/07/2012

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TClingCallFunc                                                       //
//                                                                      //
// Emulation of the CINT CallFunc class.                                //
//                                                                      //
// The CINT C++ interpreter provides an interface for calling           //
// functions through the generated wrappers in dictionaries with        //
// the CallFunc class. This class provides the same functionality,      //
// using an interface as close as possible to CallFunc but the          //
// function metadata and calling service comes from the Cling           //
// C++ interpreter and the Clang C++ compiler, not CINT.                //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include "TClingCallFunc.h"

#include "TClingClassInfo.h"
#include "TClingMethodInfo.h"

#include "TError.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/StoredValueRef.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/CodeGen/CodeGenModule.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/LLVMContext.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Type.h"

#include <string>
#include <vector>

// This ought to be declared by the implementer .. oh well...
extern void unresolvedSymbol();

namespace {
   static llvm::GenericValue convertIntegralToArg(const llvm::GenericValue& GV,
                                                  const llvm::Type* targetType) {
      // Do "extended" integral conversion, at least as CINT's dictionaries
      // would have done it: everything is a long, then gets cast to whatever
      // type is expected. "Whatever type" is an llvm type here, thus we do
      // integral conversion, integral to ptr, integral to floating point.

      // SetArg() takes a long (i.e. signed) or a longlong or a ulonglong:
      const llvm::APInt& GVI = GV.IntVal;
      const unsigned nSourceBits = GVI.getBitWidth();
      bool sourceIsSigned = true;
      if (nSourceBits > sizeof(long) * CHAR_BIT) {
         // SetArg() does not have an interface for setting a ulong,
         // so only check for [u]longlong
         sourceIsSigned = GVI.isSignedIntN(nSourceBits);
      }
      switch (targetType->getTypeID()) {
      case llvm::Type::IntegerTyID:
         {
            llvm::GenericValue ret;
            const unsigned nTargetBits = targetType->getIntegerBitWidth();
            ret.IntVal = (sourceIsSigned) ?
               GVI.sextOrTrunc(nTargetBits) : GVI.zextOrTrunc(nTargetBits);
            return ret;
         }
         break;

      case llvm::Type::FloatTyID:
         {
            llvm::GenericValue ret;
            ret.FloatVal = GV.FloatVal;
            return ret;
         }
         break;

      case llvm::Type::DoubleTyID:
         {
            llvm::GenericValue ret;
            ret.DoubleVal = GV.DoubleVal;
            return ret;
         }
         break;

      case llvm::Type::PointerTyID:
         {
            void* Ptr = (sourceIsSigned) ?
               (void*)GVI.getSExtValue() : (void*)GVI.getZExtValue();
            return llvm::PTOGV(Ptr);
         }
         break;

      default:
         Error("integralXConvertGV()",
               "Cannot convert to parameter with TypeID %d",
               targetType->getTypeID());
      }
      return GV;
   }
} // unnamed namespace


std::string TClingCallFunc::ExprToString(const clang::Expr* expr) const
{
   // Get a string representation of an expression

   clang::PrintingPolicy policy(fInterp->getCI()->
                                getASTContext().getPrintingPolicy());
   policy.SuppressTagKeyword = true;
   policy.SuppressUnwrittenScope = false;
   policy.SuppressInitializers = false;
   policy.AnonymousTagLocations = false;

   std::string buf;
   llvm::raw_string_ostream out(buf);
   expr->printPretty(out, /*Helper=*/0, policy, /*Indentation=*/0);
   out << ';'; // no value printing
   out.flush();
   return buf;
}

cling::StoredValueRef
TClingCallFunc::EvaluateExpression(const clang::Expr* expr) const
{
   // Evaluate an Expr* and return its cling::StoredValueRef
   cling::StoredValueRef valref;
   cling::Interpreter::CompilationResult cr 
      = fInterp->evaluate(ExprToString(expr), valref);
   if (cr == cling::Interpreter::kSuccess)
      return valref;
   return cling::StoredValueRef();
}


void TClingCallFunc::Exec(void *address) const
{
   if (!IsValid()) {
      Error("TClingCallFunc::Exec", "Attempt to execute while invalid.");
      return;
   }
   const clang::Decl *D = fMethod->GetMethodDecl();
   const clang::CXXMethodDecl *MD = llvm::dyn_cast<clang::CXXMethodDecl>(D);
   const clang::DeclContext *DC = D->getDeclContext();
   if (DC->isTranslationUnit() || DC->isNamespace() || (MD && MD->isStatic())) {
      // Free function or static member function.
      Invoke(fArgs);
      return;
   }
   // Member function.
   if (llvm::dyn_cast<clang::CXXConstructorDecl>(D)) {
      // Constructor.
      Error("TClingCallFunc::Exec",
            "Constructor must be called with ExecInt!");
      return;
   }
   if (!address) {
      Error("TClingCallFunc::Exec",
            "calling member function with no object pointer!");
      return;
   }
   std::vector<llvm::GenericValue> args;
   llvm::GenericValue this_ptr;
   this_ptr.IntVal = llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                                 reinterpret_cast<unsigned long>(address));
   args.push_back(this_ptr);
   args.insert(args.end(), fArgs.begin(), fArgs.end());
   Invoke(args);
}

long TClingCallFunc::ExecInt(void *address) const
{
   // Yes, the function name has Int in it, but it
   // returns a long.  This is a matter of CINT history.
   if (!IsValid()) {
      Error("TClingCallFunc::ExecInt", "Attempt to execute while invalid.");
      return 0L;
   }
   const clang::Decl *D = fMethod->GetMethodDecl();
   const clang::CXXMethodDecl *MD = llvm::dyn_cast<clang::CXXMethodDecl>(D);
   const clang::DeclContext *DC = D->getDeclContext();
   if (DC->isTranslationUnit() || DC->isNamespace() || (MD && MD->isStatic())) {
      // Free function or static member function.
      cling::Value val;
      Invoke(fArgs, &val);
      return val.simplisticCastAs<long>();
   }
   // Member function.
   if (const clang::CXXConstructorDecl *CD =
            llvm::dyn_cast<clang::CXXConstructorDecl>(D)) {
      //
      // We are simulating evaluating the expression:
      //
      //      new MyClass(args...)
      //
      // and we return the allocated address.
      //
      clang::ASTContext &Ctx = CD->getASTContext();
      const clang::RecordDecl *RD = llvm::cast<clang::RecordDecl>(DC);
      if (!RD->getDefinition()) {
         // Forward-declared class, we do not know what the size is.
         return 0L;
      }
      //
      //  If we are not doing a placement new, then
      //  find and call an operator new to allocate
      //  the memory for the object.
      //
      if (!address) {
        const clang::ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
        int64_t size = Layout.getSize().getQuantity();
        const cling::LookupHelper& LH = fInterp->getLookupHelper();
        TClingCallFunc cf(fInterp);
        const clang::FunctionDecl *newFunc = 0;
        if ((newFunc = LH.findFunctionProto(RD, "operator new",
              "std::size_t"))) {
           // We have a member function operator new.
           // Note: An operator new that is a class member does not take
           //       a this pointer as the first argument, unlike normal
           //       member functions.
        }
        else if ((newFunc = LH.findFunctionProto(Ctx.getTranslationUnitDecl(),
              "operator new", "std::size_t"))) {
           // We have a global operator new.
        }
        else {
           Error("TClingCallFunc::ExecInt",
                 "in constructor call and could not find an operator new");
           return 0L;
        }
        cf.fMethod = new TClingMethodInfo(fInterp, newFunc);
        cf.Init(newFunc);
        cf.SetArg(static_cast<long>(size));
        // Note: This may throw!
        cling::Value val;
        cf.Invoke(cf.fArgs, &val);
        address =
           reinterpret_cast<void*>(val.simplisticCastAs<unsigned long>());
        // Note: address is guaranteed to be non-zero here, otherwise
        //       the operator new would have thrown a bad_alloc exception.
      }
      //
      //  Call the constructor, either passing the address we were given,
      //  or the address we got from the operator new as the this pointer.
      //
      std::vector<llvm::GenericValue> args;
      llvm::GenericValue this_ptr;
      this_ptr.IntVal = llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                                    reinterpret_cast<unsigned long>(address));
      args.push_back(this_ptr);
      args.insert(args.end(), fArgs.begin(), fArgs.end());
      cling::Value val;
      Invoke(args, &val);
      // And return the address of the object.
      return reinterpret_cast<long>(address);
   }
   // FIXME: Need to treat member operator new special, it takes no this ptr.
   if (!address) {
      Error("TClingCallFunc::ExecInt",
            "Calling member function with no object pointer!");
      return 0L;
   }
   std::vector<llvm::GenericValue> args;
   llvm::GenericValue this_ptr;
   this_ptr.IntVal = llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                                 reinterpret_cast<unsigned long>(address));
   args.push_back(this_ptr);
   args.insert(args.end(), fArgs.begin(), fArgs.end());
   cling::Value val;
   Invoke(args, &val);
   return val.simplisticCastAs<long>();
}

long long TClingCallFunc::ExecInt64(void *address) const
{
   if (!IsValid()) {
      Error("TClingCallFunc::ExecInt64", "Attempt to execute while invalid.");
      return 0LL;
   }
   const clang::Decl *D = fMethod->GetMethodDecl();
   const clang::CXXMethodDecl *MD = llvm::dyn_cast<clang::CXXMethodDecl>(D);
   const clang::DeclContext *DC = D->getDeclContext();
   if (DC->isTranslationUnit() || DC->isNamespace() || (MD && MD->isStatic())) {
      // Free function or static member function.
      cling::Value val;
      Invoke(fArgs, &val);
      return val.simplisticCastAs<long long>();
   }
   // Member function.
   if (llvm::dyn_cast<clang::CXXConstructorDecl>(D)) {
      // Constructor.
      Error("TClingCallFunc::Exec",
            "Constructor must be called with ExecInt!");
      return 0LL;
   }
   if (!address) {
      Error("TClingCallFunc::Exec",
            "Calling member function with no object pointer!");
      return 0LL;
   }
   std::vector<llvm::GenericValue> args;
   llvm::GenericValue this_ptr;
   this_ptr.IntVal = llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                                 reinterpret_cast<unsigned long>(address));
   args.push_back(this_ptr);
   args.insert(args.end(), fArgs.begin(), fArgs.end());
   cling::Value val;
   Invoke(args, &val);
   return val.simplisticCastAs<long long>();
}

double TClingCallFunc::ExecDouble(void *address) const
{
   if (!IsValid()) {
      Error("TClingCallFunc::ExecDouble", "Attempt to execute while invalid.");
      return 0.0;
   }
   const clang::Decl *D = fMethod->GetMethodDecl();
   const clang::CXXMethodDecl *MD = llvm::dyn_cast<clang::CXXMethodDecl>(D);
   const clang::DeclContext *DC = D->getDeclContext();
   if (DC->isTranslationUnit() || DC->isNamespace() || (MD && MD->isStatic())) {
      // Free function or static member function.
      cling::Value val;
      Invoke(fArgs, &val);
      return val.simplisticCastAs<double>();
   }
   // Member function.
   if (llvm::dyn_cast<clang::CXXConstructorDecl>(D)) {
      // Constructor.
      Error("TClingCallFunc::Exec",
            "Constructor must be called with ExecInt!");
      return 0.0;
   }
   if (!address) {
      Error("TClingCallFunc::Exec",
            "Calling member function with no object pointer!");
      return 0.0;
   }
   std::vector<llvm::GenericValue> args;
   llvm::GenericValue this_ptr;
   this_ptr.IntVal = llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                                 reinterpret_cast<unsigned long>(address));
   args.push_back(this_ptr);
   args.insert(args.end(), fArgs.begin(), fArgs.end());
   cling::Value val;
   Invoke(args, &val);
   return val.simplisticCastAs<double>();
}

TClingMethodInfo *TClingCallFunc::FactoryMethod() const
{
   return new TClingMethodInfo(*fMethod);
}

void TClingCallFunc::Init()
{
   delete fMethod;
   fMethod = 0;
   fEEFunc = 0;
   fEEAddr = 0;
   ResetArg();
}

void *TClingCallFunc::InterfaceMethod() const
{
   if (!IsValid()) {
      return 0;
   }
   return fEEAddr;
}

bool TClingCallFunc::IsValid() const
{
   return fEEAddr;
}

void TClingCallFunc::ResetArg()
{
   fArgVals.clear();
   fArgs.clear();
}

void TClingCallFunc::SetArg(long param)
{
   llvm::GenericValue gv;
   gv.IntVal = llvm::APInt(sizeof(long) * CHAR_BIT, param);
   fArgs.push_back(gv);
}

void TClingCallFunc::SetArg(double param)
{
   llvm::GenericValue gv;
   gv.DoubleVal = param;
   fArgs.push_back(gv);
}

void TClingCallFunc::SetArg(long long param)
{
   llvm::GenericValue gv;
   gv.IntVal = llvm::APInt(sizeof(long long) * CHAR_BIT, param);
   fArgs.push_back(gv);
}

void TClingCallFunc::SetArg(unsigned long long param)
{
   llvm::GenericValue gv;
   gv.IntVal = llvm::APInt(sizeof(unsigned long long) * CHAR_BIT, param);
   fArgs.push_back(gv);
}

void TClingCallFunc::SetArgArray(long *paramArr, int nparam)
{
   ResetArg();
   for (int i = 0; i < nparam; ++i) {
      llvm::GenericValue gv;
      gv.IntVal = llvm::APInt(sizeof(long) * CHAR_BIT, paramArr[i]);
      fArgs.push_back(gv);
   }
}

void TClingCallFunc::EvaluateArgList(const std::string &ArgList)
{
   ResetArg();
   llvm::SmallVector<clang::Expr*, 4> exprs;
   fInterp->getLookupHelper().findArgList(ArgList, exprs);
   for (llvm::SmallVector<clang::Expr*, 4>::const_iterator I = exprs.begin(),
         E = exprs.end(); I != E; ++I) {
      cling::StoredValueRef val = EvaluateExpression(*I);
      if (!val.isValid()) {
         // Bad expression, all done.
         break;
      }
      fArgVals.push_back(val);
   }
}

void TClingCallFunc::SetArgs(const char *params)
{
   ResetArg();
   EvaluateArgList(params);
   clang::ASTContext &Context = fInterp->getCI()->getASTContext();
   for (unsigned I = 0U, E = fArgVals.size(); I < E; ++I) {
      const cling::Value& val = fArgVals[I].get();
      if (!val.type->isIntegralType(Context) &&
            !val.type->isRealFloatingType() && !val.type->isPointerType()) {
         // Invalid argument type.
         Error("TClingCallFunc::SetArgs", "Given arguments: %s", params);
         Error("TClingCallFunc::SetArgs", "Argument number %u is not of "
               "integral, floating, or pointer type!", I);
         break;
      }
      fArgs.push_back(val.value);
   }
}

void TClingCallFunc::SetFunc(const TClingClassInfo* info, const char* method, const char* arglist, long* poffset)
{
   delete fMethod;
   fMethod = new TClingMethodInfo(fInterp);
   fEEFunc = 0;
   fEEAddr = 0;
   if (poffset) {
      *poffset = 0L;
   }
   if (!info->IsValid()) {
      Error("TClingCallFunc::SetFunc", "Class info is invalid!");
      return;
   }
   if (!strcmp(arglist, ")")) {
      // CINT accepted a single right paren as meaning no arguments.
      arglist = "";
   }
   const cling::LookupHelper& lh = fInterp->getLookupHelper();
   const clang::FunctionDecl* decl =
      lh.findFunctionArgs(info->GetDecl(), method, arglist);
   if (!decl) {
      //Error("TClingCallFunc::SetFunc", "Could not find method %s(%s)", method,
      //      arglist);
      return;
   }
   fMethod->Init(decl);
   Init(decl);
   if (!IsValid()) {
      //Error("TClingCallFunc::SetFunc", "Method %s(%s) has no body.", method,
      //      arglist);
   }
   if (poffset) {
      // We have been asked to return a this pointer adjustment.
      if (const clang::CXXMethodDecl* md =
               llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
         // This is a class member function.
         info->GetOffset(md);
      }
   }
   // FIXME: We should eliminate the double parse here!
   ResetArg();
   EvaluateArgList(arglist);
   clang::ASTContext& Context = fInterp->getCI()->getASTContext();
   for (unsigned I = 0U, E = fArgVals.size(); I < E; ++I) {
      const cling::Value& val = fArgVals[I].get();
      if (!val.type->isIntegralType(Context) &&
            !val.type->isRealFloatingType() && !val.type->isPointerType()) {
         // Invalid argument type, cint skips it, strange.
         Info("TClingCallFunc::SetFunc", "Invalid value for arg %u of "
              "function %s(%s)", I, method, arglist);
         // FIXME: This really should be an error.
         continue;
      }
      fArgs.push_back(val.value);
   }
}

void TClingCallFunc::SetFunc(const TClingMethodInfo *info)
{
   delete fMethod;
   fMethod = 0;
   fEEFunc = 0;
   fEEAddr = 0;
   fMethod = new TClingMethodInfo(*info);
   if (!fMethod->IsValid()) {
      return;
   }
   Init(fMethod->GetMethodDecl());
   //if (!IsValid()) {
   //   Error("TClingCallFunc::SetFunc", "Method has no body.");
   //}
}

void TClingCallFunc::SetFuncProto(const TClingClassInfo *info, const char *method,
                                  const char *proto, long *poffset)
{
   delete fMethod;
   fMethod = new TClingMethodInfo(fInterp);
   fEEFunc = 0;
   fEEAddr = 0;
   if (poffset) {
      *poffset = 0L;
   }
   ResetArg();
   if (!info->IsValid()) {
      Error("TClingCallFunc::SetFuncProto", "Class info is invalid!");
      return;
   }
   *fMethod = info->GetMethod(method, proto, poffset);
   if (!fMethod->IsValid()) {
      //Error("TClingCallFunc::SetFuncProto", "Could not find method %s(%s)",
      //      method, proto);
   }
   const clang::FunctionDecl *FD = fMethod->GetMethodDecl();
   if (FD) {
      Init(FD);
      //if (!IsValid()) {
      //   Error("TClingCallFunc::SetFuncProto", "Method %s(%s) has no body.",
      //         method, proto);
      //}
   }
}

static llvm::Type *getLLVMType(cling::Interpreter *interp, clang::QualType QT)
{
   clang::CodeGenerator* CG = interp->getCodeGenerator();
   clang::CodeGen::CodeGenModule* CGM = CG->GetBuilder();
   clang::CodeGen::CodeGenTypes& CGT = CGM->getTypes();
   // Note: The first thing this routine does is getCanonicalType(), so we
   //       do not need to do that first.
   llvm::Type* Ty = CGT.ConvertType(QT);
   //llvm::Type* Ty = CGT.ConvertTypeForMem(QT);
   return Ty;
}

void TClingCallFunc::Init(const clang::FunctionDecl *FD)
{
   fEEFunc = 0;
   fEEAddr = 0;
   bool isMemberFunc = true;
   const clang::CXXMethodDecl *MD = llvm::dyn_cast<clang::CXXMethodDecl>(FD);
   const clang::DeclContext *DC = FD->getDeclContext();
   if (DC->isTranslationUnit() || DC->isNamespace() || (MD && MD->isStatic())) {
      // Free function or static member function.
      isMemberFunc = false;
   }
   //
   //  Mangle the function name, if necessary.
   //
   const char *FuncName = 0;
   std::string MangledName;
   llvm::raw_string_ostream OS(MangledName);
   clang::ASTContext& ASTCtx = fInterp->getCI()->getASTContext();
   llvm::OwningPtr<clang::MangleContext> Mangle(ASTCtx.createMangleContext());
   if (!Mangle->shouldMangleDeclName(FD)) {
      clang::IdentifierInfo *II = FD->getIdentifier();
      FuncName = II->getNameStart();
   }
   else {
      if (const clang::CXXConstructorDecl *D =
               llvm::dyn_cast<clang::CXXConstructorDecl>(FD)) {
         //Ctor_Complete,          // Complete object ctor
         //Ctor_Base,              // Base object ctor
         //Ctor_CompleteAllocating // Complete object allocating ctor (unused)
         Mangle->mangleCXXCtor(D, clang::Ctor_Complete, OS);
      }
      else if (const clang::CXXDestructorDecl *D =
                  llvm::dyn_cast<clang::CXXDestructorDecl>(FD)) {
         //Dtor_Deleting, // Deleting dtor
         //Dtor_Complete, // Complete object dtor
         //Dtor_Base      // Base object dtor
         Mangle->mangleCXXDtor(D, clang::Dtor_Deleting, OS);
      }
      else {
         Mangle->mangleName(FD, OS);
      }
      OS.flush();
      FuncName = MangledName.c_str();
   }
   //
   //  Check the execution engine for the function.
   //
   llvm::ExecutionEngine *EE = fInterp->getExecutionEngine();
   fEEFunc = EE->FindFunctionNamed(FuncName);
   if (fEEFunc) {
      // Execution engine had it, get the mapping.
      fEEAddr = EE->getPointerToFunction(fEEFunc);
   }
   else {
      // Execution engine does not have it, check
      // the loaded shareable libraries.

      // Avoid spurious error message if we look for an
      // unimplemented (but declared) function.
      fInterp->suppressLazyFunctionCreatorDiags(true);
      void *FP = EE->getPointerToNamedFunction(FuncName,
                 /*AbortOnFailure=*/false);
      fInterp->suppressLazyFunctionCreatorDiags(false);
      if (FP == unresolvedSymbol) {
         // We failed to find an implementation for the function, the 
         // interface requires the 'address' to be zero.
         fEEAddr = 0;
      } else if (FP) {
         fEEAddr = FP;

         // Create a llvm function we can use to call it with later.
         llvm::LLVMContext &Context = *fInterp->getLLVMContext();
         unsigned NumParams = FD->getNumParams();
         llvm::SmallVector<llvm::Type *, 8> Params;
         if (isMemberFunc) {
            // Force the invisible this pointer arg to pointer to char.
            Params.push_back(llvm::PointerType::getUnqual(
                                llvm::IntegerType::get(Context, CHAR_BIT)));
         }
         for (unsigned I = 0U; I < NumParams; ++I) {
            const clang::ParmVarDecl *PVD = FD->getParamDecl(I);
            clang::QualType QT = PVD->getType();
            llvm::Type *argtype = getLLVMType(fInterp, QT);
            if (argtype == 0) {
               // We are not in good shape, quit while we are still alive.
               return;
            }
            Params.push_back(argtype);
         }
         llvm::Type *ReturnType = 0;
         if (llvm::isa<clang::CXXConstructorDecl>(FD)) {
            // Force the return type of a constructor to be long.
            ReturnType = llvm::IntegerType::get(Context, sizeof(long) *
                                                CHAR_BIT);
         }
         else {
            ReturnType = getLLVMType(fInterp, FD->getResultType());
         }
         if (ReturnType) {
            
            // Create the llvm function type.
            llvm::FunctionType *FT = llvm::FunctionType::get(ReturnType, Params,
                                     /*isVarArg=*/false);
            // Create the ExecutionEngine function.
            // Note: We use weak linkage here so lookup failure does not abort.
            llvm::Function *F = llvm::Function::Create(FT,
                                llvm::GlobalValue::ExternalWeakLinkage,
                                FuncName, fInterp->getModule());
            // FIXME: This probably does not work for Windows!
            // See ASTContext::getFunctionType() for proper way to set it.
            // Actually this probably is not needed.
            F->setCallingConv(llvm::CallingConv::C);
            // Map the created ExecutionEngine function to the
            // address found in the shareable library, so the next
            // time we do a lookup it will be found.
            EE->addGlobalMapping(F, FP);
            // Set our state.
            fEEFunc = F;
         }
      }
   }
}

void
TClingCallFunc::Invoke(const std::vector<llvm::GenericValue> &ArgValues,
                       cling::Value* result /*= 0*/) const
{
   // FIXME: We need to think about thunks for the this pointer adjustment,
   //        and the return pointer adjustment for covariant return types.
   //if (!IsValid()) {
   //   return;
   //}
   if (result) *result = cling::Value();
   unsigned long num_given_args = static_cast<unsigned long>(ArgValues.size());
   const clang::FunctionDecl *fd = fMethod->GetMethodDecl();
   const clang::CXXMethodDecl *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
   const clang::DeclContext *dc = fd->getDeclContext();
   bool isMemberFunction = true;
   if (dc->isTranslationUnit() || dc->isNamespace() || (md && md->isStatic())) {
      isMemberFunction = false;
   }
   unsigned num_params = fd->getNumParams();
   unsigned min_args = fd->getMinRequiredArguments();
   if (isMemberFunction) {
      // Adjust for the hidden this pointer first argument.
      ++num_params;
      ++min_args;
   }
   if (num_given_args < min_args) {
      // Not all required arguments given.
      Error("TClingCallFunc::Invoke",
            "Not enough function arguments given (min: %u max:%u, given: %lu)",
            min_args, num_params, num_given_args);
      return;
   }
   else if (num_given_args > num_params) {
      Error("TClingCallFunc::Invoke",
            "Too many function arguments given (min: %u max: %u, given: %lu)",
            min_args, num_params, num_given_args);
      return;
   }

   // This will be the arguments actually passed to the JIT function.
   std::vector<llvm::GenericValue> Args;
   std::vector<cling::StoredValueRef> ArgsStorage;
   // We are going to loop over the JIT function args.
   llvm::FunctionType *ft = fEEFunc->getFunctionType();
   clang::ASTContext& context = fd->getASTContext();
   for (unsigned i = 0U, e = ft->getNumParams(); i < e; ++i) {
      if (i < num_given_args) {
         // We have a user-provided argument value.
         const llvm::Type *ty = ft->getParamType(i);
         Args.push_back(convertIntegralToArg(ArgValues[i], ty));
      }
      else {
         // Use the default value from the decl.
         const clang::ParmVarDecl* pvd = 0;
         if (!isMemberFunction) {
            pvd = fd->getParamDecl(i);
         }
         else {
            // Compensate for the undeclared added this pointer value.
            pvd = fd->getParamDecl(i-1);
         }
         //assert(pvd->hasDefaultArg() && "No default for argument!");
         const clang::Expr* expr = pvd->getDefaultArg();
         cling::StoredValueRef valref = EvaluateExpression(expr);
         if (valref.isValid()) {
            ArgsStorage.push_back(valref);
            const cling::Value& val = valref.get();
            if (!val.type->isIntegralType(context) &&
                  !val.type->isRealFloatingType() &&
                  !val.type->canDecayToPointerType()) {
               // Invalid argument type.
               Error("TClingCallFunc::Invoke",
                     "Default for argument %u: %s", i, ExprToString(expr).c_str());
               Error("TClingCallFunc::Invoke",
                     "is not of integral, floating, or pointer type!");
               return;
            }
            const llvm::Type *ty = ft->getParamType(i);
            Args.push_back(convertIntegralToArg(val.value, ty));
         }
         else {
            Error("TClingCallFunc::Invoke",
                  "Could not evaluate default for argument %u: %s",
                  i, ExprToString(expr).c_str());
            return;
         }
      }
   }

   llvm::GenericValue return_val = fInterp->getExecutionEngine()->runFunction(fEEFunc, Args);
   if (result) {
      if (ft->getReturnType()->getTypeID() == llvm::Type::PointerTyID) {
         // Note: The cint interface requires pointers to be
         //       returned as unsigned long.
         llvm::GenericValue converted_return_val;
         converted_return_val.IntVal =
            llvm::APInt(sizeof(unsigned long) * CHAR_BIT,
                        reinterpret_cast<unsigned long>(GVTOP(return_val)));
         *result = cling::Value(converted_return_val, context.LongTy);
      } else {
         *result = cling::Value(return_val, fd->getResultType());
      }
   }
}

