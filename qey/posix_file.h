#ifndef QEY_POSIX_FILE_H_
#define QEY_POSIX_FILE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "qey/file.h"
#include "qey/status.h"

namespace qey {

// A File backed by a regular file on a POSIX system.
class PosixFile : public File {
 public:
  // Opens `path` for reading and writing, creating it if needed. When the
  // file is created, its directory is synced so the new entry is durable.
  static StatusOr<std::unique_ptr<PosixFile>> Open(const std::string& path);

  PosixFile(const PosixFile&) = delete;
  PosixFile& operator=(const PosixFile&) = delete;
  ~PosixFile() override;

  StatusOr<std::string> Read(uint64_t offset, size_t n) override;
  Status Append(std::string_view data) override;
  // Flushes to stable storage. On macOS this uses F_FULLFSYNC, because fsync
  // does not flush the drive's write cache there.
  Status Sync() override;
  Status Truncate(uint64_t size) override;
  uint64_t Size() const override { return size_; }

  // Takes an exclusive advisory lock on the file, held until it is closed.
  // Fails with kUnavailable if another open file description holds it, which
  // includes another PosixFile for the same path in this process.
  Status Lock();

 private:
  PosixFile(int fd, uint64_t size) : fd_(fd), size_(size) {}

  int fd_;
  uint64_t size_;
};

// Flushes the directory that contains `path`, making a newly created or
// renamed entry durable.
Status SyncDirectory(const std::string& path);

}  // namespace qey

#endif  // QEY_POSIX_FILE_H_
