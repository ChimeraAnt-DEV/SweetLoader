#pragma once

/**
 * @file MappedFile.hpp
 * @brief Reads a whole file with a read-only mapping instead of a read into a vector.
 *
 * <p>{@code ReadWholeFile} in the .antegg reader opened the archive, seeked to the end, resized a
 * vector to the file's own length and read it in (audit item 2.16). For a large package that is a
 * full copy of the file in heap, and the ZIP parser only walks it -- it never mutates it.
 *
 * <p>This maps the file instead. The parser reads the same bytes through the pointer, so the read
 * path is a one-line change and the double-copy (read into a vector, then parse) is gone. The map is
 * private and read-only, so a write to the bytes faults rather than silently diverging from the
 * file. On a platform without {@code mmap} it falls back to reading into an owned buffer, so callers
 * have one interface.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(__unix__) || defined(__ANDROID__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define PL_HAS_MMAP 1
#else
#define PL_HAS_MMAP 0
#endif

namespace pl::runtime {

/**
 * @brief A read-only view of a whole file, mapped when possible.
 *
 * <p>Move-only: the mapping has one owner, and copying it would alias the same pages with two
 * destructors. {@code data()} is non-null for a mapped zero-length file's nothing -- an empty file
 * yields a null pointer with size zero, which the ZIP parser already rejects before it dereferences.
 */
class MappedFile {
 public:
  MappedFile() = default;

  /** Maps (or reads) @p path, refusing a file larger than @p maxBytes. */
  static MappedFile Open(const std::string &path, std::uint64_t maxBytes) {
    MappedFile result;
#if PL_HAS_MMAP
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return result;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
      ::close(fd);
      return result;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    if (size > maxBytes) {
      ::close(fd);
      return result;
    }
    if (size == 0) {
      // An empty mapping is not portable (mmap rejects length 0). Report success with a null view;
      // the callers refuse a too-short buffer before they dereference.
      ::close(fd);
      result.mMapped = true;
      result.mValid = true;
      result.mSize = 0;
      return result;
    }
    void *address = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    // The mapping keeps the file alive on its own; the descriptor is not needed after this.
    ::close(fd);
    if (address == MAP_FAILED) return result;
    result.mMapped = true;
    result.mValid = true;
    result.mSize = static_cast<std::size_t>(size);
    result.mData = static_cast<const std::uint8_t *>(address);
    return result;
#else
    (void)maxBytes;
    std::ifstream file(path, std::ios::binary);
    if (!file) return result;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return result;
    if (static_cast<std::uint64_t>(size) > maxBytes) return result;
    file.seekg(0, std::ios::beg);
    result.mOwned.resize(static_cast<std::size_t>(size));
    if (size > 0) file.read(reinterpret_cast<char *>(result.mOwned.data()), size);
    if (!file.good() && !file.eof()) {
      result.mOwned.clear();
      return result;
    }
    result.mValid = true;
    result.mSize = result.mOwned.size();
    result.mData = result.mOwned.data();
    return result;
#endif
  }

  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  MappedFile(MappedFile &&other) noexcept { MoveFrom(other); }

  MappedFile &operator=(MappedFile &&other) noexcept {
    if (this != &other) {
      Release();
      MoveFrom(other);
    }
    return *this;
  }

  ~MappedFile() { Release(); }

  /** The file's bytes, or null when the file is empty or could not be opened. */
  const std::uint8_t *data() const { return mData; }
  /** The file's length in bytes. */
  std::size_t size() const { return mSize; }
  /** True when the file was opened and is available (possibly empty). */
  bool valid() const { return mValid; }

 private:
  void MoveFrom(MappedFile &other) {
    mData = other.mData;
    mSize = other.mSize;
    mMapped = other.mMapped;
    mValid = other.mValid;
    mOwned = std::move(other.mOwned);
#if PL_HAS_MMAP
    other.mData = nullptr;
    other.mSize = 0;
    other.mMapped = false;
    other.mValid = false;
#else
    // The owned buffer moved with the vector; point at the new owner's storage.
    if (!mOwned.empty()) mData = mOwned.data();
#endif
  }

  void Release() {
#if PL_HAS_MMAP
    if (mMapped && mData != nullptr && mSize > 0) {
      ::munmap(const_cast<std::uint8_t *>(mData), mSize);
    }
#endif
    mData = nullptr;
    mSize = 0;
    mMapped = false;
    mValid = false;
  }

  const std::uint8_t *mData = nullptr;
  std::size_t mSize = 0;
  bool mMapped = false;
  bool mValid = false;
  std::vector<std::uint8_t> mOwned;
};

} // namespace pl::runtime
