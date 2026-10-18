#include "qey/posix_storage.h"

#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

#include "gtest/gtest.h"

namespace qey {
namespace {

class PosixStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    path_ = testing::TempDir() + "/posix_storage_test_" +
            testing::UnitTest::GetInstance()->current_test_info()->name();
    Cleanup();
  }
  void TearDown() override { Cleanup(); }
  void Cleanup() {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
  }

  std::unique_ptr<PosixStorage> Open() {
    StatusOr<std::unique_ptr<PosixStorage>> s = PosixStorage::Open(path_);
    EXPECT_TRUE(s.ok()) << s.status().message();
    return s.ok() ? *std::move(s) : nullptr;
  }

  bool TmpExists() { return ::access((path_ + ".tmp").c_str(), F_OK) == 0; }

  static std::string ReadAll(File& f) { return *f.Read(0, f.Size()); }

  std::string path_;
};

TEST_F(PosixStorageTest, SecondOpenIsRefusedUntilFirstCloses) {
  auto first = Open();
  ASSERT_NE(first, nullptr);
  StatusOr<std::unique_ptr<PosixStorage>> second = PosixStorage::Open(path_);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.status().code(), StatusCode::kUnavailable);

  first.reset();
  EXPECT_NE(Open(), nullptr);
}

TEST_F(PosixStorageTest, OpenRemovesStaleTempFile) {
  FILE* f = std::fopen((path_ + ".tmp").c_str(), "w");
  ASSERT_NE(f, nullptr);
  std::fputs("junk from a crashed compaction", f);
  std::fclose(f);
  ASSERT_TRUE(TmpExists());

  auto s = Open();
  ASSERT_NE(s, nullptr);
  EXPECT_FALSE(TmpExists());
}

TEST_F(PosixStorageTest, ReplaceSwapsContentsAndKeepsTheLock) {
  auto s = Open();
  ASSERT_TRUE(s->log().Append("old log").ok());

  ASSERT_TRUE(s->Replace([](File& out) { return out.Append("new log"); }).ok());
  EXPECT_EQ(ReadAll(s->log()), "new log");
  EXPECT_FALSE(TmpExists());

  // Locked across the swap.
  EXPECT_EQ(PosixStorage::Open(path_).status().code(),
            StatusCode::kUnavailable);

  // Further appends go to the new file, which is the one at `path_`.
  ASSERT_TRUE(s->log().Append("!").ok());
  s.reset();
  s = Open();
  EXPECT_EQ(ReadAll(s->log()), "new log!");
}

TEST_F(PosixStorageTest, FailedFillLeavesOldLogCurrent) {
  auto s = Open();
  ASSERT_TRUE(s->log().Append("old log").ok());

  Status result = s->Replace([](File& out) {
    EXPECT_TRUE(out.Append("half of the new log").ok());
    return Status(StatusCode::kIoError, "boom");
  });
  EXPECT_EQ(result.code(), StatusCode::kIoError);
  EXPECT_EQ(ReadAll(s->log()), "old log");
  EXPECT_FALSE(TmpExists());

  ASSERT_TRUE(s->log().Append("+").ok());
  ASSERT_TRUE(s->Replace([](File& out) { return out.Append("ok"); }).ok());
  EXPECT_EQ(ReadAll(s->log()), "ok");
}

TEST_F(PosixStorageTest, ReplaceIgnoresStaleTempFile) {
  auto s = Open();
  FILE* f = std::fopen((path_ + ".tmp").c_str(), "w");
  ASSERT_NE(f, nullptr);
  std::fputs("stale", f);
  std::fclose(f);

  ASSERT_TRUE(s->Replace([](File& out) { return out.Append("fresh"); }).ok());
  EXPECT_EQ(ReadAll(s->log()), "fresh");
}

}  // namespace
}  // namespace qey
