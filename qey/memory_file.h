#ifndef QEY_MEMORY_FILE_H_
#define QEY_MEMORY_FILE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "qey/file.h"
#include "qey/status.h"

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
  int sync_count_ = 0;
  bool fail_syncs_ = false;
  bool fail_truncates_ = false;
};

// A File that forwards to another File without owning it. Lets a test hand a
// File to code that takes ownership, while keeping access to the underlying
// MemFile, e.g. to reopen a database on the same bytes. `target` must outlive
// this object.
class FileRef : public File {
 public:
  explicit FileRef(File* target) : target_(target) {}

  StatusOr<std::string> Read(uint64_t offset, size_t n) override {
    return target_->Read(offset, n);
  }
  Status Append(std::string_view data) override {
    return target_->Append(data);
  }
  Status Sync() override { return target_->Sync(); }
  Status Truncate(uint64_t size) override { return target_->Truncate(size); }
  uint64_t Size() const override { return target_->Size(); }

 private:
  File* target_;
};

}  // namespace qey

#endif  // QEY_MEMORY_FILE_H_
