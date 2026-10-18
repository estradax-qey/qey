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

 private:
  std::string data_;
  std::optional<uint64_t> limit_;
  int sync_count_ = 0;
};

}  // namespace qey

#endif  // QEY_MEMORY_FILE_H_
