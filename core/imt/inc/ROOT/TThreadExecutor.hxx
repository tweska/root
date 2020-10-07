// @(#)root/thread:$Id$
// Author: Xavier Valls March 2016

/*************************************************************************
 * Copyright (C) 1995-2006, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TThreadExecutor
#define ROOT_TThreadExecutor

#include "RConfigure.h"

// exclude in case ROOT does not have IMT support
#ifndef R__USE_IMT
// No need to error out for dictionaries.
# if !defined(__ROOTCLING__) && !defined(G__DICTIONARY)
#  error "Cannot use ROOT::TThreadExecutor without defining R__USE_IMT."
# endif
#else

#include "ROOT/TExecutorCRTP.hxx"
#include "RTaskArena.hxx"
#include "TError.h"
#include <functional>
#include <memory>
#include <numeric>


namespace ROOT {

   class TThreadExecutor: public TExecutorCRTP<TThreadExecutor> {
   public:

      explicit TThreadExecutor(UInt_t nThreads = 0u);

      TThreadExecutor(const TThreadExecutor &) = delete;
      TThreadExecutor &operator=(const TThreadExecutor &) = delete;

      template<class F>
      void Foreach(F func, unsigned nTimes, unsigned nChunks = 0);
      template<class F, class INTEGER>
      void Foreach(F func, ROOT::TSeq<INTEGER> args, unsigned nChunks = 0);
      template<class F, class T>
      void Foreach(F func, std::initializer_list<T> args, unsigned nChunks = 0);
      template<class F, class T>
      void Foreach(F func, std::vector<T> &args, unsigned nChunks = 0);
      template<class F, class T>
      void Foreach(F func, const std::vector<T> &args, unsigned nChunks = 0);

      using TExecutorCRTP<TThreadExecutor>::Map;
      template<class F, class Cond = noReferenceCond<F>>
      auto Map(F func, unsigned nTimes) -> std::vector<typename std::result_of<F()>::type>;
      template<class F, class INTEGER, class Cond = noReferenceCond<F, INTEGER>>
      auto Map(F func, ROOT::TSeq<INTEGER> args) -> std::vector<typename std::result_of<F(INTEGER)>::type>;
      template<class F, class T, class Cond = noReferenceCond<F, T>>
      auto Map(F func, std::vector<T> &args) -> std::vector<typename std::result_of<F(T)>::type>;

      // // MapReduce
      // // the late return types also check at compile-time whether redfunc is compatible with func,
      // // other than checking that func is compatible with the type of arguments.
      // // a static_assert check in TThreadExecutor::Reduce is used to check that redfunc is compatible with the type returned by func
      using TExecutorCRTP<TThreadExecutor>::MapReduce;
      template<class F, class R, class Cond = noReferenceCond<F>>
      auto MapReduce(F func, unsigned nTimes, R redfunc) -> typename std::result_of<F()>::type;
      template<class F, class R, class Cond = noReferenceCond<F>>
      auto MapReduce(F func, unsigned nTimes, R redfunc, unsigned nChunks) -> typename std::result_of<F()>::type;
      template<class F, class INTEGER, class R, class Cond = noReferenceCond<F, INTEGER>>
      auto MapReduce(F func, ROOT::TSeq<INTEGER> args, R redfunc, unsigned nChunks) -> typename std::result_of<F(INTEGER)>::type;
      template<class F, class T, class R, class Cond = noReferenceCond<F, T>>
      auto MapReduce(F func, std::initializer_list<T> args, R redfunc, unsigned nChunks) -> typename std::result_of<F(T)>::type;
      template<class F, class T, class R, class Cond = noReferenceCond<F, T>>
      auto MapReduce(F func, std::vector<T> &args, R redfunc) -> typename std::result_of<F(T)>::type;
      template<class F, class T, class R, class Cond = noReferenceCond<F, T>>
      auto MapReduce(F func, std::vector<T> &args, R redfunc, unsigned nChunks) -> typename std::result_of<F(T)>::type;

      using TExecutorCRTP<TThreadExecutor>::Reduce;
      template<class T, class BINARYOP> auto Reduce(const std::vector<T> &objs, BINARYOP redfunc) -> decltype(redfunc(objs.front(), objs.front()));

      unsigned GetPoolSize();

   protected:
      template<class F, class R, class Cond = noReferenceCond<F>>
      auto Map(F func, unsigned nTimes, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F()>::type>;
      template<class F, class INTEGER, class R, class Cond = noReferenceCond<F, INTEGER>>
      auto Map(F func, ROOT::TSeq<INTEGER> args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(INTEGER)>::type>;
      template<class F, class T, class R, class Cond = noReferenceCond<F, T>>
      auto Map(F func, std::vector<T> &args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(T)>::type>;
      template<class F, class T, class R, class Cond = noReferenceCond<F, T>>
      auto Map(F func, std::initializer_list<T> args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(T)>::type>;

   private:
      void   ParallelFor(unsigned start, unsigned end, unsigned step, const std::function<void(unsigned int i)> &f);
      double ParallelReduce(const std::vector<double> &objs, const std::function<double(double a, double b)> &redfunc);
      float  ParallelReduce(const std::vector<float> &objs, const std::function<float(float a, float b)> &redfunc);
      template<class T, class R>
      auto SeqReduce(const std::vector<T> &objs, R redfunc) -> decltype(redfunc(objs));

      /// Pointer to the TBB task arena wrapper
      std::shared_ptr<ROOT::Internal::RTaskArenaWrapper> fTaskArenaW = nullptr;
   };

   /************ TEMPLATE METHODS IMPLEMENTATION ******************/

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function without arguments several times in parallel, dividing the execution in nChunks.
   ///
   /// \param func Function to be executed.
   /// \param nTimes Number of times function should be called.
   /// \param nChunks Number of chunks to split the input data for processing.
   template<class F>
   void TThreadExecutor::Foreach(F func, unsigned nTimes, unsigned nChunks) {
      if (nChunks == 0) {
         ParallelFor(0U, nTimes, 1, [&](unsigned int){func();});
         return;
      }

      unsigned step = (nTimes + nChunks - 1) / nChunks;
      auto lambda = [&](unsigned int i)
      {
         for (unsigned j = 0; j < step && (i + j) < nTimes; j++) {
            func();
         }
      };
      ParallelFor(0U, nTimes, step, lambda);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over a sequence of indexes, dividing the execution in nChunks.
   ///
   /// \param func Function to be executed. Must take an element of the sequence passed assecond argument as a parameter.
   /// \param args Sequence of indexes to execute `func` on.
   /// \param nChunks Number of chunks to split the input data for processing.
   template<class F, class INTEGER>
   void TThreadExecutor::Foreach(F func, ROOT::TSeq<INTEGER> args, unsigned nChunks) {
      if (nChunks == 0) {
         ParallelFor(*args.begin(), *args.end(), args.step(), [&](unsigned int i){func(i);});
         return;
      }
      unsigned start = *args.begin();
      unsigned end = *args.end();
      unsigned seqStep = args.step();
      unsigned step = (end - start + nChunks - 1) / nChunks; //ceiling the division

      auto lambda = [&](unsigned int i)
      {
         for (unsigned j = 0; j < step && (i + j) < end; j+=seqStep) {
            func(i + j);
         }
      };
      ParallelFor(start, end, step, lambda);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of an initializer_list, dividing the execution in nChunks.
   ///
   /// \param func Function to be executed on the elements of the initializer_list passed as second parameter.
   /// \param args initializer_list for a vector to apply `func` on.
   /// \param nChunks Number of chunks to split the input data for processing.
   template<class F, class T>
   void TThreadExecutor::Foreach(F func, std::initializer_list<T> args, unsigned nChunks) {
      std::vector<T> vargs(std::move(args));
      Foreach(func, vargs, nChunks);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a vector, dividing the execution in nChunks.
   ///
   /// \param func Function to be executed on the elements of the vector passed as second parameter.
   /// \param args Vector of elements passed as an argument to `func`.
   /// \param nChunks Number of chunks to split the input data for processing.
   template<class F, class T>
   void TThreadExecutor::Foreach(F func, std::vector<T> &args, unsigned nChunks) {
      unsigned int nToProcess = args.size();
      if (nChunks == 0) {
         ParallelFor(0U, nToProcess, 1, [&](unsigned int i){func(args[i]);});
         return;
      }

      unsigned step = (nToProcess + nChunks - 1) / nChunks; //ceiling the division
      auto lambda = [&](unsigned int i)
      {
         for (unsigned j = 0; j < step && (i + j) < nToProcess; j++) {
            func(args[i + j]);
         }
      };
      ParallelFor(0U, nToProcess, step, lambda);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a immutable vector, dividing the execution in nChunks.
   ///
   /// \param func Function to be executed on the elements of the vector passed as second parameter.
   /// \param args Vector of elements passed as an argument to `func`.
   /// \param nChunks Number of chunks to split the input data for processing.
   template<class F, class T>
   void TThreadExecutor::Foreach(F func, const std::vector<T> &args, unsigned nChunks) {
      unsigned int nToProcess = args.size();
      if (nChunks == 0) {
         ParallelFor(0U, nToProcess, 1, [&](unsigned int i){func(args[i]);});
         return;
      }

      unsigned step = (nToProcess + nChunks - 1) / nChunks; //ceiling the division
      auto lambda = [&](unsigned int i)
      {
         for (unsigned j = 0; j < step && (i + j) < nToProcess; j++) {
            func(args[i + j]);
         }
      };
      ParallelFor(0U, nToProcess, step, lambda);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function without arguments several times in parallel.
   ///
   /// \copydetails TExecutorCRTP::Map(F func,unsigned nTimes)
   template<class F, class Cond>
   auto TThreadExecutor::Map(F func, unsigned nTimes) -> std::vector<typename std::result_of<F()>::type> {
      using retType = decltype(func());
      std::vector<retType> reslist(nTimes);
      auto lambda = [&](unsigned int i)
      {
         reslist[i] = func();
      };
      ParallelFor(0U, nTimes, 1, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function over a sequence of indexes in parallel.
   ///
   /// \copydetails TExecutorCRTP::Map(F func,ROOT::TSeq<INTEGER> args)
   template<class F, class INTEGER, class Cond>
   auto TThreadExecutor::Map(F func, ROOT::TSeq<INTEGER> args) -> std::vector<typename std::result_of<F(INTEGER)>::type> {
      unsigned start = *args.begin();
      unsigned end = *args.end();
      unsigned seqStep = args.step();

      using retType = decltype(func(start));
      std::vector<retType> reslist(args.size());
      auto lambda = [&](unsigned int i)
      {
         reslist[i] = func(i);
      };
      ParallelFor(start, end, seqStep, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function `nTimes` in parallel, dividing the execution in nChunks and
   /// providing a result per chunk.
   ///
   /// \copydetails ROOT::Internal::TExecutor::Map(F func,unsigned nTimes,R redfunc,unsigned nChunks)
   template<class F, class R, class Cond>
   auto TThreadExecutor::Map(F func, unsigned nTimes, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F()>::type> {
      if (nChunks == 0)
      {
         return Map(func, nTimes);
      }

      unsigned step = (nTimes + nChunks - 1) / nChunks;
      // Avoid empty chunks
      unsigned actualChunks = (nTimes + step - 1) / step;
      using retType = decltype(func());
      std::vector<retType> reslist(actualChunks);
      auto lambda = [&](unsigned int i)
      {
         std::vector<retType> partialResults(std::min(nTimes-i, step));
         for (unsigned j = 0; j < step && (i + j) < nTimes; j++) {
            partialResults[j] = func();
         }
         reslist[i / step] = Reduce(partialResults, redfunc);
      };
      ParallelFor(0U, nTimes, step, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function over the elements of a vector in parallel
   ///
   /// \copydetails TExecutorCRTP::Map(F func,unsigned nTimes,std::vector<T> &args)
   template<class F, class T, class Cond>
   auto TThreadExecutor::Map(F func, std::vector<T> &args) -> std::vector<typename std::result_of<F(T)>::type> {
      // //check whether func is callable
      using retType = decltype(func(args.front()));

      unsigned int nToProcess = args.size();
      std::vector<retType> reslist(nToProcess);

      auto lambda = [&](unsigned int i)
      {
         reslist[i] = func(args[i]);
      };

      ParallelFor(0U, nToProcess, 1, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a sequence, dividing the execution in nChunks and
   /// providing a result per chunk.
   ///
   /// \copydetails ROOT::Internal::TExecutor::Map(F func,ROOT::TSeq<INTEGER> args,R redfunc,unsigned nChunks)
   template<class F, class INTEGER, class R, class Cond>
   auto TThreadExecutor::Map(F func, ROOT::TSeq<INTEGER> args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(INTEGER)>::type> {
      if (nChunks == 0)
      {
         return Map(func, args);
      }

      unsigned start = *args.begin();
      unsigned end = *args.end();
      unsigned seqStep = args.step();
      unsigned step = (end - start + nChunks - 1) / nChunks; //ceiling the division
      // Avoid empty chunks
      unsigned actualChunks = (end - start + step - 1) / step;

      using retType = decltype(func(start));
      std::vector<retType> reslist(actualChunks);
      auto lambda = [&](unsigned int i)
      {
         std::vector<retType> partialResults(std::min(end-i, step));
         for (unsigned j = 0; j < step && (i + j) < end; j+=seqStep) {
            partialResults[j] = func(i + j);
         }
         reslist[i / step] = Reduce(partialResults, redfunc);
      };
      ParallelFor(start, end, step, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a vector, dividing the execution in nChunks and
   /// providing a result per chunk.
   ///
   /// \copydetails ROOT::Internal::TExecutor::Map(F func,std::vector<T> &args,R redfunc,unsigned nChunks)
   template<class F, class T, class R, class Cond>
   auto TThreadExecutor::Map(F func, std::vector<T> &args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(T)>::type> {
      if (nChunks == 0)
      {
         return Map(func, args);
      }

      unsigned int nToProcess = args.size();
      unsigned step = (nToProcess + nChunks - 1) / nChunks; //ceiling the division
      // Avoid empty chunks
      unsigned actualChunks = (nToProcess + step - 1) / step;

      using retType = decltype(func(args.front()));
      std::vector<retType> reslist(actualChunks);
      auto lambda = [&](unsigned int i)
      {
         std::vector<T> partialResults(step);
         for (unsigned j = 0; j < step && (i + j) < nToProcess; j++) {
            partialResults[j] = func(args[i + j]);
         }
         reslist[i / step] = Reduce(partialResults, redfunc);
      };

      ParallelFor(0U, nToProcess, step, lambda);

      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of an initializer_list, dividing the execution in nChunks and
   /// providing a result per chunk.
   ///
   /// \copydetails ROOT::Internal::TExecutor::Map(F func,std::initializer_list<T> args,R redfunc,unsigned nChunks)
   template<class F, class T, class R, class Cond>
   auto TThreadExecutor::Map(F func, std::initializer_list<T> args, R redfunc, unsigned nChunks) -> std::vector<typename std::result_of<F(T)>::type> {
      std::vector<T> vargs(std::move(args));
      const auto &reslist = Map(func, vargs, redfunc, nChunks);
      return reslist;
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function `nTimes` in parallel (Map) and accumulate the results into a single value (Reduce).
   /// \copydetails  ROOT::Internal::TExecutor::MapReduce(F func,unsigned nTimes,R redfunc)
   template<class F, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, unsigned nTimes, R redfunc) -> typename std::result_of<F()>::type {
      return Reduce(Map(func, nTimes), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a vector (Map) and accumulate the results into a single value (Reduce).
   /// Benefits from partial reduction into `nChunks` intermediate results.
   ///
   /// \copydetails ROOT::Internal::TExecutor::MapReduce(F func,unsigned nTimes,R redfunc,unsigned nChunks)
   template<class F, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, unsigned nTimes, R redfunc, unsigned nChunks) -> typename std::result_of<F()>::type {
      return Reduce(Map(func, nTimes, redfunc, nChunks), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a vector (Map) and accumulate the results into a single value (Reduce).
   /// Benefits from partial reduction into `nChunks` intermediate results.
   ///
   /// \copydetails ROOT::Internal::TExecutor::MapReduce(F func,ROOT::TSeq<INTEGER> args,R redfunc,unsigned nChunks)
   template<class F, class INTEGER, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, ROOT::TSeq<INTEGER> args, R redfunc, unsigned nChunks) -> typename std::result_of<F(INTEGER)>::type {
      return Reduce(Map(func, args, redfunc, nChunks), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of an initializer_list (Map) and accumulate the results into a single value (Reduce).
   /// Benefits from partial reduction into `nChunks` intermediate results.
   ///
   /// \copydetails ROOT::Internal::TExecutor::MapReduce(F func,std::initializer_list<T> args,R redfunc,unsigned nChunks)
   template<class F, class T, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, std::initializer_list<T> args, R redfunc, unsigned nChunks) -> typename std::result_of<F(T)>::type {
      return Reduce(Map(func, args, redfunc, nChunks), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function over the elements of a vector in parallel (Map) and accumulate the results into a single value (Reduce).
   /// \copydetails  ROOT::Internal::TExecutor::MapReduce(F func,std::vector<T> &args,R redfunc)
   template<class F, class T, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, std::vector<T> &args, R redfunc) -> typename std::result_of<F(T)>::type {
      return Reduce(Map(func, args), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief Execute a function in parallel over the elements of a vector (Map) and accumulate the results into a single value (Reduce).
   /// Benefits from partial reduction into `nChunks` intermediate results.
   ///
   /// \copydetails ROOT::Internal::TExecutor::MapReduce(F func,std::vector<T> &args,R redfunc,unsigned nChunks)
   template<class F, class T, class R, class Cond>
   auto TThreadExecutor::MapReduce(F func, std::vector<T> &args, R redfunc, unsigned nChunks) -> typename std::result_of<F(T)>::type {
      return Reduce(Map(func, args, redfunc, nChunks), redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief "Reduce" an std::vector into a single object in parallel by passing a
   /// binary function as the second argument defining the reduction operation.
   ///
   /// \param objs A vector of elements to combine.
   /// \param redfunc Binary reduction function to combine the elements of the vector `objs`.
   /// \return A value result of combining the vector elements into a single object of the same type.
   template<class T, class BINARYOP>
   auto TThreadExecutor::Reduce(const std::vector<T> &objs, BINARYOP redfunc) -> decltype(redfunc(objs.front(), objs.front()))
   {
      // check we can apply reduce to objs
      static_assert(std::is_same<decltype(redfunc(objs.front(), objs.front())), T>::value, "redfunc does not have the correct signature");
      return ParallelReduce(objs, redfunc);
   }

   //////////////////////////////////////////////////////////////////////////
   /// \brief "Reduce", sequentially, an std::vector into a single object 
   ///
   /// \param objs A vector of elements to combine.
   /// \param redfunc Binary reduction function to combine the elements of the vector `objs`.
   /// \return A value result of combining the vector elements into a single object of the same type.
   template<class T, class R>
   auto TThreadExecutor::SeqReduce(const std::vector<T> &objs, R redfunc) -> decltype(redfunc(objs))
   {
      return redfunc(objs);
   }

} // namespace ROOT

#endif   // R__USE_IMT
#endif
