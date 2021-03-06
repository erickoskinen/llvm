//===- GsymReader.h ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_GSYM_GSYMREADER_H
#define LLVM_DEBUGINFO_GSYM_GSYMREADER_H


#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/GSYM/FileEntry.h"
#include "llvm/DebugInfo/GSYM/FunctionInfo.h"
#include "llvm/DebugInfo/GSYM/Header.h"
#include "llvm/DebugInfo/GSYM/LineEntry.h"
#include "llvm/DebugInfo/GSYM/StringTable.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorOr.h"

#include <inttypes.h>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace llvm {
class MemoryBuffer;
class raw_ostream;

namespace gsym {

/// GsymReader is used to read GSYM data from a file or buffer.
///
/// This class is optimized for very quick lookups when the endianness matches
/// the host system. The Header, address table, address info offsets, and file
/// table is designed to be mmap'ed as read only into memory and used without
/// any parsing needed. If the endianness doesn't match, we swap these objects
/// and tables into GsymReader::SwappedData and then point our header and
/// ArrayRefs to this swapped internal data.
///
/// GsymReader objects must use one of the static functions to create an
/// instance: GsymReader::openFile(...) and GsymReader::copyBuffer(...).

class GsymReader {
  GsymReader(std::unique_ptr<MemoryBuffer> Buffer);
  llvm::Error parse();

  std::unique_ptr<MemoryBuffer> MemBuffer;
  StringRef GsymBytes;
  llvm::support::endianness Endian;
  const Header *Hdr = nullptr;
  ArrayRef<uint8_t> AddrOffsets;
  ArrayRef<uint32_t> AddrInfoOffsets;
  ArrayRef<FileEntry> Files;
  StringTable StrTab;
  /// When the GSYM file's endianness doesn't match the host system then
  /// we must decode all data structures that need to be swapped into
  /// local storage and set point the ArrayRef objects above to these swapped
  /// copies.
  struct SwappedData {
    Header Hdr;
    std::vector<uint8_t> AddrOffsets;
    std::vector<uint32_t> AddrInfoOffsets;
    std::vector<FileEntry> Files;
  };
  std::unique_ptr<SwappedData> Swap;

public:
  GsymReader(GsymReader &&RHS);
  ~GsymReader();

  /// Construct a GsymReader from a file on disk.
  ///
  /// \param Path The file path the GSYM file to read.
  /// \returns An expected GsymReader that contains the object or an error
  /// object that indicates reason for failing to read the GSYM.
  static llvm::Expected<GsymReader> openFile(StringRef Path);

  /// Construct a GsymReader from a buffer.
  ///
  /// \param Bytes A set of bytes that will be copied and owned by the
  /// returned object on success.
  /// \returns An expected GsymReader that contains the object or an error
  /// object that indicates reason for failing to read the GSYM.
  static llvm::Expected<GsymReader> copyBuffer(StringRef Bytes);

  /// Access the GSYM header.
  /// \returns A native endian version of the GSYM header.
  const Header &getHeader() const;

  /// Get the full function info for an address.
  ///
  /// \param Addr A virtual address from the orignal object file to lookup.
  /// \returns An expected FunctionInfo that contains the function info object
  /// or an error object that indicates reason for failing to lookup the
  /// address,
  llvm::Expected<FunctionInfo> getFunctionInfo(uint64_t Addr) const;

  /// Get a string from the string table.
  ///
  /// \param Offset The string table offset for the string to retrieve.
  /// \returns The string from the strin table.
  StringRef getString(uint32_t Offset) const { return StrTab[Offset]; }

protected:
  /// Gets an address from the address table.
  ///
  /// Addresses are stored as offsets frrom the gsym::Header::BaseAddress.
  ///
  /// \param Index A index into the address table.
  /// \returns A resolved virtual address for adddress in the address table
  /// or llvm::None if Index is out of bounds.
  Optional<uint64_t> getAddress(size_t Index) const;

  /// Get the a file entry for the suppplied file index.
  ///
  /// Used to convert any file indexes in the FunctionInfo data back into
  /// files. This function can be used for iteration, but is more commonly used
  /// for random access when doing lookups.
  ///
  /// \param Index An index into the file table.
  /// \returns An optional FileInfo that will be valid if the file index is
  /// valid, or llvm::None if the file index is out of bounds,
  Optional<FileEntry> getFile(uint32_t Index) const {
    if (Index < Files.size())
      return Files[Index];
    return llvm::None;
  }

  /// Get an appropriate address info offsets array.
  ///
  /// The address table in the GSYM file is stored as array of 1, 2, 4 or 8
  /// byte offsets from the The gsym::Header::BaseAddress. The table is stored
  /// internally as a array of bytes that are in the correct endianness. When
  /// we access this table we must get an array that matches those sizes. This
  /// templatized helper function is used when accessing address offsets in the
  /// AddrOffsets member variable.
  ///
  /// \returns An ArrayRef of an appropriate address offset size.
  template <class T> ArrayRef<T>
  getAddrOffsets() const {
    return ArrayRef<T>(reinterpret_cast<const T *>(AddrOffsets.data()),
                       AddrOffsets.size()/sizeof(T));
  }

  /// Get an appropriate address from the address table.
  ///
  /// The address table in the GSYM file is stored as array of 1, 2, 4 or 8
  /// byte address offsets from the The gsym::Header::BaseAddress. The table is
  /// stored internally as a array of bytes that are in the correct endianness.
  /// In order to extract an address from the address table we must access the
  /// address offset using the correct size and then add it to the BaseAddress
  /// in the header.
  ///
  /// \param Index An index into the AddrOffsets array.
  /// \returns An virtual address that matches the original object file for the
  /// address as the specified index, or llvm::None if Index is out of bounds.
  template <class T> Optional<uint64_t>
  addressForIndex(size_t Index) const {
    ArrayRef<T> AIO = getAddrOffsets<T>();
    if (Index < AIO.size())
      return AIO[Index] + Hdr->BaseAddress;
    return llvm::None;
  }
  /// Lookup an address offset in the AddrOffsets table.
  ///
  /// Given an address offset, look it up using a binary search of the
  /// AddrOffsets table.
  ///
  /// \param AddrOffset An address offset, that has already been computed by
  /// subtracting the gsym::Header::BaseAddress.
  /// \returns The matching address offset index. This index will be used to
  /// extract the FunctionInfo data's offset from the AddrInfoOffsets array.
  template <class T>
  uint64_t getAddressOffsetIndex(const uint64_t AddrOffset) const {
    ArrayRef<T> AIO = getAddrOffsets<T>();
    const auto Begin = AIO.begin();
    const auto End = AIO.end();
    auto Iter = std::lower_bound(Begin, End, AddrOffset);
    if (Iter == End || AddrOffset < *Iter)
      --Iter;
    return std::distance(Begin, Iter);
  }

  /// Create a GSYM from a memory buffer.
  ///
  /// Called by both openFile() and copyBuffer(), this function does all of the
  /// work of parsing the GSYM file and returning an error.
  ///
  /// \param MemBuffer A memory buffer that will transfer ownership into the
  /// GsymReader.
  /// \returns An expected GsymReader that contains the object or an error
  /// object that indicates reason for failing to read the GSYM.
  static llvm::Expected<llvm::gsym::GsymReader>
  create(std::unique_ptr<MemoryBuffer> &MemBuffer);


  /// Given an address, find the address index.
  ///
  /// Binary search the address table and find the matching address index.
  ///
  /// \param Addr A virtual address that matches the original object file
  /// to lookup.
  /// \returns An index into the address table. This index can be used to
  /// extract the FunctionInfo data's offset from the AddrInfoOffsets array.
  /// Returns an error if the address isn't in the GSYM with details of why.
  Expected<uint64_t> getAddressIndex(const uint64_t Addr) const;

  /// Given an address index, get the offset for the FunctionInfo.
  ///
  /// Looking up an address is done by finding the corresponding address
  /// index for the address. This index is then used to get the offset of the
  /// FunctionInfo data that we will decode using this function.
  ///
  /// \param Index An index into the address table.
  /// \returns An optional GSYM data offset for the offset of the FunctionInfo
  /// that needs to be decoded.
  Optional<uint64_t> getAddressInfoOffset(size_t Index) const;
};

} // namespace gsym
} // namespace llvm

#endif // #ifndef LLVM_DEBUGINFO_GSYM_GSYMREADER_H
