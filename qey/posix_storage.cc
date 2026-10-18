#include "qey/posix_storage.h"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "qey/file.h"
#include "qey/posix_file.h"
#include "qey/status.h"

namespace qey {
namespace {

Status IoError(std::string_view what, int err) {
  return Status(StatusCode::kIoError,
                std::string(what) + ": " +
                    std::error_code(err, std::generic_category()).message());
}

// Stands in for the log after a replacement whose durability is in doubt.
// Every operation fails, so the owner stops acknowledging writes.
class FailedFile : public File {
 public:
  explicit FailedFile(uint64_t size) : size_(size) {}

  StatusOr<std::string> Read(uint64_t, size_t) override { return Fail(); }
  Status Append(std::string_view) override { return Fail(); }
  Status Sync() override { return Fail(); }
  Status Truncate(uint64_t) override { return Fail(); }
  uint64_t Size() const override { return size_; }

 private:
  static Status Fail() {
    return Status(StatusCode::kIoError,
                  "log replacement was not confirmed durable; reopen the "
                  "database");
  }

  uint64_t size_;
};

// Removes `path` if it exists.
Status Remove(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return IoError("remove " + path, errno);
  }
  return Status::Ok();
}

}  // namespace

StatusOr<std::unique_ptr<PosixStorage>> PosixStorage::Open(
    const std::string& path) {
  StatusOr<std::unique_ptr<PosixFile>> file = PosixFile::Open(path);
  if (!file.ok()) return file.status();
  Status s = (*file)->Lock();
  if (!s.ok()) return Status(s.code(), path + ": " + s.message());

  // We hold the lock, so any leftover temporary file is from a crash.
  s = Remove(path + ".tmp");
  if (!s.ok()) return s;

  return StatusOr<std::unique_ptr<PosixStorage>>(std::unique_ptr<PosixStorage>(
      new PosixStorage(path, std::unique_ptr<File>(*std::move(file)))));
}

Status PosixStorage::Replace(const std::function<Status(File&)>& fill) {
  const std::string tmp_path = path_ + ".tmp";
  Status s = Remove(tmp_path);
  if (!s.ok()) return s;

  StatusOr<std::unique_ptr<PosixFile>> tmp = PosixFile::Open(tmp_path);
  if (!tmp.ok()) return tmp.status();
  std::unique_ptr<PosixFile> replacement = *std::move(tmp);

  // Lock the new file before it becomes the log, so the lock is held
  // continuously across the swap.
  s = replacement->Lock();
  if (s.ok()) s = fill(*replacement);
  if (s.ok()) s = replacement->Sync();
  if (!s.ok()) {
    Remove(tmp_path).IgnoreError();
    return s;
  }

  if (::rename(tmp_path.c_str(), path_.c_str()) != 0) {
    const int err = errno;
    Remove(tmp_path).IgnoreError();
    return IoError("rename " + tmp_path, err);
  }

  // The path now names the new file; the old descriptor points at an
  // unlinked inode and must not be used again.
  log_ = std::move(replacement);
  s = SyncDirectory(path_);
  if (!s.ok()) {
    log_ = std::make_unique<FailedFile>(log_->Size());
    return s;
  }
  return Status::Ok();
}

}  // namespace qey
