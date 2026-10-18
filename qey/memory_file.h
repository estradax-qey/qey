#ifndef QEY_MEMORY_FILE_H_
#define QEY_MEMORY_FILE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "qey/file.h"
#include "qey/status.h"
#include "qey/storage.h"

namespace qey {

// A File backed by a string, with fault injection. For tests.
class MemFile : public File {
 public:
  MemFile() = default;
  explicit MemFile(std::string data) : data_(std::move(data)) {}

  StatusOr<std::string> Read(uint64_t offset, size_t n) override;
  Status Append(std::string_view data) override;
  Status Sync() override;
  Status Truncate(uint64_t size) override;
  uint64_t Size() const override { return data_.size(); }

  // The file contents. Tests may modify them to simulate corruption.
  std::string& data() { return data_; }
  const std::string& data() const { return data_; }

  int sync_count() const { return sync_count_; }

  // Makes the file refuse to grow past `size` bytes. An Append that would
  // cross the limit writes the bytes that fit, then fails with kIoError,
  // like a write torn by a crash. Pass std::nullopt to clear the limit.
  void FailAppendsBeyond(std::optional<uint64_t> size) { limit_ = size; }

  // Makes Sync() or Truncate() fail with kIoError.
  void FailSyncs(bool fail) { fail_syncs_ = fail; }
  void FailTruncates(bool fail) { fail_truncates_ = fail; }

 private:
  std::string data_;
  std::optional<uint64_t> limit_;
  // Atomic because a Db may sync from its background thread.
  std::atomic<int> sync_count_{0};
  std::atomic<bool> fail_syncs_{false};
  bool fail_truncates_ = false;
};

// A Storage over a MemFile that the test keeps ownership of, so the test can
// inspect it and reopen a database on the same bytes. With fault injection.
// `file` must outlive this object.
class MemStorage : public Storage {
 public:
  explicit MemStorage(MemFile* file) : file_(file) {}

  File& log() override { return *file_; }

  // Builds the replacement in memory and swaps it in with one assignment, so
  // it is all-or-nothing.
  Status Replace(const std::function<Status(File&)>& fill) override {
    ++replace_attempts_;
    if (fail_replace_begin_) {
      return Status(StatusCode::kIoError, "injected failure creating file");
    }
    MemFile replacement;
    Status s = fill(replacement);
    if (!s.ok()) return s;
    if (fail_replace_commit_) {
      return Status(StatusCode::kIoError, "injected failure before swap");
    }
    file_->data() = std::move(replacement.data());
    ++replace_count_;
    return Status::Ok();
  }

  // Makes Replace() fail before calling `fill`, or after it but before the
  // swap. In both cases the old log is left untouched.
  void FailReplaceBegin(bool fail) { fail_replace_begin_ = fail; }
  void FailReplaceCommit(bool fail) { fail_replace_commit_ = fail; }

  int replace_attempts() const { return replace_attempts_; }
  int replace_count() const { return replace_count_; }

 private:
  MemFile* file_;
  bool fail_replace_begin_ = false;
  bool fail_replace_commit_ = false;
  int replace_attempts_ = 0;
  int replace_count_ = 0;
};

}  // namespace qey

#endif  // QEY_MEMORY_FILE_H_
