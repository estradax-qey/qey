#ifndef QEY_FILE_H_
#define QEY_FILE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "qey/status.h"

namespace qey {

// The storage operations the log needs. Abstract so tests can inject failures
// and torn writes, and so platforms can supply their own implementation.
//
// Not thread-safe; callers serialize access.
class File {
 public:
  virtual ~File() = default;

  // Reads up to `n` bytes starting at `offset`. Returns fewer bytes (possibly
  // none) if the file ends first.
  virtual StatusOr<std::string> Read(uint64_t offset, size_t n) = 0;

  // Appends `data` to the end of the file. On failure a prefix of `data` may
  // have been written, as after a crash.
  virtual Status Append(std::string_view data) = 0;

  // Flushes appended data to stable storage.
  virtual Status Sync() = 0;

  // Shrinks the file to `size` bytes. `size` must not exceed Size().
  virtual Status Truncate(uint64_t size) = 0;

  // Returns the current size in bytes.
  virtual uint64_t Size() const = 0;
};

}  // namespace qey

#endif  // QEY_FILE_H_
