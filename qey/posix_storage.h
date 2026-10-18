#ifndef QEY_POSIX_STORAGE_H_
#define QEY_POSIX_STORAGE_H_

#include <functional>
#include <memory>
#include <string>

#include "qey/file.h"
#include "qey/status.h"
#include "qey/storage.h"

namespace qey {

// A log in a single file at `path`. Replacement is done by writing
// `<path>.tmp` and renaming it over `path`; the temporary file exists only
// while that happens.
class PosixStorage : public Storage {
 public:
  // Opens `path`, creating it if needed, and takes an exclusive lock on it
  // that is held for the lifetime of the object, including across Replace().
  // Fails with kUnavailable if the file is already open elsewhere. Removes a
  // stale `<path>.tmp` left by an interrupted replacement.
  static StatusOr<std::unique_ptr<PosixStorage>> Open(const std::string& path);

  File& log() override { return *log_; }
  Status Replace(const std::function<Status(File&)>& fill) override;

 private:
  PosixStorage(std::string path, std::unique_ptr<File> log)
      : path_(std::move(path)), log_(std::move(log)) {}

  std::string path_;
  std::unique_ptr<File> log_;
};

}  // namespace qey

#endif  // QEY_POSIX_STORAGE_H_
