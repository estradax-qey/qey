#include "qey/posix_file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "qey/status.h"

namespace qey {
namespace {

Status IoError(std::string_view what, int err) {
  return Status(StatusCode::kIoError,
                std::string(what) + ": " +
                    std::error_code(err, std::generic_category()).message());
}

bool FullSync(int fd) {
#if defined(__APPLE__)
  // Plain fsync does not flush the drive's write cache on macOS. Not every
  // file system supports F_FULLFSYNC, so fall back to fsync if it fails.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}

}  // namespace

Status SyncDirectory(const std::string& path) {
  std::filesystem::path dir = std::filesystem::path(path).parent_path();
  if (dir.empty()) dir = ".";
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return IoError("open directory " + dir.string(), errno);
  const bool ok = FullSync(fd);
  const int err = errno;
  ::close(fd);
  if (!ok) return IoError("sync directory " + dir.string(), err);
  return Status::Ok();
}

StatusOr<std::unique_ptr<PosixFile>> PosixFile::Open(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  const bool created = fd >= 0;
  if (!created && errno == EEXIST) {
    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  }
  if (fd < 0) return IoError("open " + path, errno);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    const int err = errno;
    ::close(fd);
    return IoError("stat " + path, err);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return Status(StatusCode::kInvalidArgument,
                  path + " is not a regular file");
  }

  std::unique_ptr<PosixFile> file(
      new PosixFile(fd, static_cast<uint64_t>(st.st_size)));
  if (created) {
    Status s = SyncDirectory(path);
    if (!s.ok()) return s;
  }
  return StatusOr<std::unique_ptr<PosixFile>>(std::move(file));
}

PosixFile::~PosixFile() { ::close(fd_); }

StatusOr<std::string> PosixFile::Read(uint64_t offset, size_t n) {
  if (offset >= size_) return std::string();
  n = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
  std::string buf(n, '\0');
  size_t got = 0;
  while (got < n) {
    const ssize_t r =
        ::pread(fd_, &buf[got], n - got, static_cast<off_t>(offset + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      return IoError("read", errno);
    }
    if (r == 0) break;
    got += static_cast<size_t>(r);
  }
  buf.resize(got);
  return buf;
}

Status PosixFile::Append(std::string_view data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t w =
        ::pwrite(fd_, data.data() + written, data.size() - written,
                 static_cast<off_t>(size_ + written));
    if (w < 0) {
      if (errno == EINTR) continue;
      const int err = errno;
      size_ += written;  // A prefix reached the file.
      return IoError("write", err);
    }
    written += static_cast<size_t>(w);
  }
  size_ += written;
  return Status::Ok();
}

Status PosixFile::Lock() {
  if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      return Status(StatusCode::kUnavailable,
                    "file is locked by another handle or process");
    }
    return IoError("lock", errno);
  }
  return Status::Ok();
}

Status PosixFile::Sync() {
  if (!FullSync(fd_)) return IoError("sync", errno);
  return Status::Ok();
}

Status PosixFile::Truncate(uint64_t size) {
  if (size > size_) {
    return Status(StatusCode::kInvalidArgument,
                  "cannot truncate to a larger size");
  }
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
    return IoError("truncate", errno);
  }
  size_ = size;
  return Status::Ok();
}

}  // namespace qey
