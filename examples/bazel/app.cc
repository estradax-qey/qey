// A minimal program that uses qey: a string with a TTL and a stream.
#include <chrono>
#include <iostream>

#include "qey/db.h"

int main() {
  qey::Db db;  // in memory; qey::Db::Open("app.qey") is durable

  db.Set("greeting", "hello", std::chrono::seconds(60)).IgnoreError();
  qey::StatusOr<qey::StreamId> id =
      db.XAdd("session:1", R"({"role":"user","content":"hi"})");
  if (!id.ok()) return 1;

  std::cout << *db.Get("greeting") << "\n"
            << "added entry " << id->ToString() << ", stream length "
            << *db.XLen("session:1") << "\n";
}
