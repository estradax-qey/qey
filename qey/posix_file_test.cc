#include "qey/posix_file.h"

#include <cstdio>
#include <memory>
#include <string>

#include "gtest/gtest.h"

namespace qey {
namespace {

class PosixFileTest : public testing::Test {
 protected:
  void SetUp() override {
    path_ = testing::TempDir() + "/posix_file_test_" +
            testing::UnitTest::GetInstance()->current_test_info()->name();
    std::remove(path_.c_str());
  }
  void TearDown() override { std::remove(path_.c_str()); }

  std::unique_ptr<PosixFile> Open() {
    StatusOr<std::unique_ptr<PosixFile>> f = PosixFile::Open(path_);
    EXPECT_TRUE(f.ok()) << f.status().message();
    return f.ok() ? *std::move(f) : nullptr;
  }

  std::string path_;
};

TEST_F(PosixFileTest, CreatesEmptyFile) {
  auto f = Open();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->Size(), 0);
}

TEST_F(PosixFileTest, AppendThenRead) {
  auto f = Open();
  ASSERT_TRUE(f->Append("hello ").ok());
  ASSERT_TRUE(f->Append("world").ok());
  EXPECT_EQ(f->Size(), 11);
  EXPECT_EQ(*f->Read(0, 11), "hello world");
  EXPECT_EQ(*f->Read(6, 5), "world");
  EXPECT_EQ(*f->Read(6, 100), "world");  // short read at EOF
  EXPECT_EQ(*f->Read(11, 4), "");
  EXPECT_EQ(*f->Read(500, 4), "");
}

TEST_F(PosixFileTest, DataSurvivesReopen) {
  {
    auto f = Open();
    ASSERT_TRUE(f->Append(std::string("a\0b", 3)).ok());
    ASSERT_TRUE(f->Sync().ok());
  }
  auto f = Open();
  EXPECT_EQ(f->Size(), 3);
  EXPECT_EQ(*f->Read(0, 3), std::string("a\0b", 3));
  ASSERT_TRUE(f->Append("c").ok());
  EXPECT_EQ(*f->Read(0, 4), std::string("a\0bc", 4));
}

TEST_F(PosixFileTest, TruncateShrinksAndAllowsFurtherAppends) {
  auto f = Open();
  ASSERT_TRUE(f->Append("abcdef").ok());
  ASSERT_TRUE(f->Truncate(2).ok());
  EXPECT_EQ(f->Size(), 2);
  ASSERT_TRUE(f->Append("XY").ok());
  EXPECT_EQ(*f->Read(0, 10), "abXY");
  EXPECT_EQ(f->Truncate(100).code(), StatusCode::kInvalidArgument);
}

TEST_F(PosixFileTest, LargeAppendRoundTrips) {
  auto f = Open();
  const std::string big(3 * 1024 * 1024 + 7, 'x');
  ASSERT_TRUE(f->Append(big).ok());
  EXPECT_EQ(f->Size(), big.size());
  EXPECT_EQ(*f->Read(0, big.size()), big);
}

TEST_F(PosixFileTest, OpenFailsForDirectoryAndMissingParent) {
  StatusOr<std::unique_ptr<PosixFile>> dir =
      PosixFile::Open(testing::TempDir());
  EXPECT_FALSE(dir.ok());
  StatusOr<std::unique_ptr<PosixFile>> missing =
      PosixFile::Open(testing::TempDir() + "/no/such/dir/file");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.status().code(), StatusCode::kIoError);
}

}  // namespace
}  // namespace qey
