#include "qey/memory_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "qey/status.h"

namespace qey {

StatusOr<std::string> MemFile::Read(uint64_t offset, size_t n) {
  if (offset >= data_.size()) return std::string();
  return data_.substr(offset, n);
}

Status MemFile::Append(std::string_view data) {
  if (limit_.has_value() && data_.size() + data.size() > *limit_) {
    const uint64_t room = *limit_ > data_.size() ? *limit_ - data_.size() : 0;
    data_.append(data.substr(0, room));
    return Status(StatusCode::kIoError, "injected append failure");
  }
  data_.append(data);
  return Status::Ok();
}

Status MemFile::Sync() {
  ++sync_count_;
  return Status::Ok();
}

Status MemFile::Truncate(uint64_t size) {
  if (size > data_.size()) {
    return Status(StatusCode::kInvalidArgument,
                  "cannot truncate to a larger size");
  }
  data_.resize(size);
  return Status::Ok();
}

}  // namespace qey
