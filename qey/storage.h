#ifndef QEY_STORAGE_H_
#define QEY_STORAGE_H_

#include <functional>

#include "qey/file.h"
#include "qey/status.h"

namespace qey {

// Where a database's log lives, and how to replace it wholesale. Compaction
// uses Replace() to swap in a rewritten log.
//
// Not thread-safe; callers serialize access.
class Storage {
 public:
  virtual ~Storage() = default;

  // The current log. The reference is invalidated by Replace().
  virtual File& log() = 0;

  // Atomically replaces the log. Creates a new empty file, calls `fill` to
  // write the new contents, makes the file durable, and then swaps it in.
  //
  // If `fill` or any step before the swap fails, the old log is untouched and
  // still current, and the error is returned. A crash at any point leaves
  // either the old or the new log on disk, never a mix.
  //
  // If a failure after the swap leaves the new log's durability in doubt, the
  // storage makes log() fail every operation, so the owner cannot acknowledge
  // writes that might be lost.
  virtual Status Replace(const std::function<Status(File&)>& fill) = 0;
};

}  // namespace qey

#endif  // QEY_STORAGE_H_
