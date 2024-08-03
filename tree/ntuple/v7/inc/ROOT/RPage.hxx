/// \file ROOT/RPage.hxx
/// \ingroup NTuple ROOT7
/// \author Jakob Blomer <jblomer@cern.ch>
/// \date 2018-10-09
/// \warning This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback
/// is welcome!

/*************************************************************************
 * Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT7_RPage
#define ROOT7_RPage

#include <ROOT/RNTupleUtil.hxx>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ROOT {
namespace Experimental {
namespace Internal {

class RPageAllocator;
class RPageRef;

// clang-format off
/**
\class ROOT::Experimental::Internal::RPage
\ingroup NTuple
\brief A page is a slice of a column that is mapped into memory

The page provides an opaque memory buffer for uncompressed, unpacked data. It does not interpret
the contents but it does now about the size (and thus the number) of the elements inside as well as the element
number range within the backing column/cluster.
For reading, pages are allocated and filled by the page source and then registered with the page pool.
For writing, the page sink allocates uninitialized pages of a given size.
The page has a pointer to its memory allocator so that it can release itself.
*/
// clang-format on
class RPage {
   friend class RPageRef;

public:
   static constexpr size_t kPageZeroSize = 64 * 1024;

   /**
    * Stores information about the cluster in which this page resides.
    */
   class RClusterInfo {
   private:
      /// The cluster number
      DescriptorId_t fId = 0;
      /// The first element index of the column in this cluster
      NTupleSize_t fIndexOffset = 0;
   public:
      RClusterInfo() = default;
      RClusterInfo(NTupleSize_t id, NTupleSize_t indexOffset) : fId(id), fIndexOffset(indexOffset) {}
      NTupleSize_t GetId() const { return fId; }
      NTupleSize_t GetIndexOffset() const { return fIndexOffset; }
   };

private:
   ColumnId_t fColumnId = kInvalidColumnId;
   void *fBuffer = nullptr;
   /// The allocator used to allocate fBuffer. Can be null if the buffer doesn't need to be freed.
   RPageAllocator *fPageAllocator = nullptr;
   std::uint32_t fElementSize = 0;
   std::uint32_t fNElements = 0;
   /// The capacity of the page in number of elements
   std::uint32_t fMaxElements = 0;
   NTupleSize_t fRangeFirst = 0;
   RClusterInfo fClusterInfo;

public:
   RPage() = default;
   RPage(ColumnId_t columnId, void *buffer, RPageAllocator *pageAllocator, ClusterSize_t::ValueType elementSize,
         ClusterSize_t::ValueType maxElements)
      : fColumnId(columnId),
        fBuffer(buffer),
        fPageAllocator(pageAllocator),
        fElementSize(elementSize),
        fMaxElements(maxElements)
   {}
   RPage(const RPage &) = delete;
   RPage &operator=(const RPage &) = delete;
   RPage(RPage &&other)
   {
      fColumnId = other.fColumnId;
      fBuffer = other.fBuffer;
      fPageAllocator = other.fPageAllocator;
      fElementSize = other.fElementSize;
      fNElements = other.fNElements;
      fMaxElements = other.fMaxElements;
      fRangeFirst = other.fRangeFirst;
      fClusterInfo = other.fClusterInfo;
      other.fPageAllocator = nullptr;
   }
   RPage &operator=(RPage &&other)
   {
      if (this != &other) {
         std::swap(fColumnId, other.fColumnId);
         std::swap(fBuffer, other.fBuffer);
         std::swap(fPageAllocator, other.fPageAllocator);
         std::swap(fElementSize, other.fElementSize);
         std::swap(fNElements, other.fNElements);
         std::swap(fMaxElements, other.fMaxElements);
         std::swap(fRangeFirst, other.fRangeFirst);
         std::swap(fClusterInfo, other.fClusterInfo);
      }
      return *this;
   }
   ~RPage() = default;

   ColumnId_t GetColumnId() const { return fColumnId; }
   /// The space taken by column elements in the buffer
   std::uint32_t GetNBytes() const { return fElementSize * fNElements; }
   std::uint32_t GetNElements() const { return fNElements; }
   std::uint32_t GetMaxElements() const { return fMaxElements; }
   NTupleSize_t GetGlobalRangeFirst() const { return fRangeFirst; }
   NTupleSize_t GetGlobalRangeLast() const { return fRangeFirst + NTupleSize_t(fNElements) - 1; }
   ClusterSize_t::ValueType GetClusterRangeFirst() const { return fRangeFirst - fClusterInfo.GetIndexOffset(); }
   ClusterSize_t::ValueType GetClusterRangeLast() const {
      return GetClusterRangeFirst() + NTupleSize_t(fNElements) - 1;
   }
   const RClusterInfo& GetClusterInfo() const { return fClusterInfo; }

   bool Contains(NTupleSize_t globalIndex) const {
      return (globalIndex >= fRangeFirst) && (globalIndex < fRangeFirst + NTupleSize_t(fNElements));
   }

   bool Contains(RClusterIndex clusterIndex) const
   {
      if (fClusterInfo.GetId() != clusterIndex.GetClusterId())
         return false;
      auto clusterRangeFirst = ClusterSize_t(fRangeFirst - fClusterInfo.GetIndexOffset());
      return (clusterIndex.GetIndex() >= clusterRangeFirst) &&
             (clusterIndex.GetIndex() < clusterRangeFirst + fNElements);
   }

   void* GetBuffer() const { return fBuffer; }
   /// Called during writing: returns a pointer after the last element and increases the element counter
   /// in anticipation of the caller filling nElements in the page. It is the responsibility of the caller
   /// to prevent page overflows, i.e. that fNElements + nElements <= fMaxElements
   void* GrowUnchecked(ClusterSize_t::ValueType nElements) {
      auto offset = GetNBytes();
      fNElements += nElements;
      return static_cast<unsigned char *>(fBuffer) + offset;
   }
   /// Seek the page to a certain position of the column
   void SetWindow(const NTupleSize_t rangeFirst, const RClusterInfo &clusterInfo) {
      fClusterInfo = clusterInfo;
      fRangeFirst = rangeFirst;
   }
   /// Forget all currently stored elements (size == 0) and set a new starting index.
   void Reset(NTupleSize_t rangeFirst) { fNElements = 0; fRangeFirst = rangeFirst; }
   void ResetCluster(const RClusterInfo &clusterInfo) { fNElements = 0; fClusterInfo = clusterInfo; }

   /// Make a 'zero' page for column `columnId` (that is comprised of 0x00 bytes only). The caller is responsible for
   /// invoking `GrowUnchecked()` and `SetWindow()` as appropriate.
   static RPage MakePageZero(ColumnId_t columnId, ClusterSize_t::ValueType elementSize)
   {
      return RPage{columnId, const_cast<void *>(GetPageZeroBuffer()), nullptr, elementSize,
                   /*maxElements=*/(kPageZeroSize / elementSize)};
   }
   /// Return a pointer to the page zero buffer used if there is no on-disk data for a particular deferred column
   static const void *GetPageZeroBuffer();

   /// Transition method, eventually the page will delete itself on destruction
   void ReleaseBuffer();

   bool IsValid() const { return fColumnId != kInvalidColumnId; }
   bool IsNull() const { return fBuffer == nullptr; }
   bool IsPageZero() const { return fBuffer == GetPageZeroBuffer(); }
   bool IsEmpty() const { return fNElements == 0; }
   bool operator ==(const RPage &other) const { return fBuffer == other.fBuffer; }
   bool operator !=(const RPage &other) const { return !(*this == other); }
}; // class RPage

} // namespace Internal
} // namespace Experimental
} // namespace ROOT

#endif
