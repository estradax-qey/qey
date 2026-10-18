#include "qey/status.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace qey {
namespace {

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_TRUE(Status::Ok().ok());
}

TEST(StatusTest, CarriesCodeAndMessage) {
  Status s(StatusCode::kNotInteger, "bad");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kNotInteger);
  EXPECT_EQ(s.message(), "bad");
}

TEST(StatusTest, IoCodesAreNotOk) {
  for (StatusCode code : {StatusCode::kInvalidArgument, StatusCode::kIoError,
                          StatusCode::kCorruption, StatusCode::kUnavailable}) {
    EXPECT_FALSE(Status(code, "x").ok());
  }
}

TEST(StatusOrTest, HoldsValue) {
  StatusOr<int64_t> r = 42;
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
  EXPECT_TRUE(r.status().ok());
}

TEST(StatusOrTest, HoldsError) {
  StatusOr<int64_t> r = Status(StatusCode::kOutOfRange, "overflow");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), StatusCode::kOutOfRange);
}

}  // namespace
}  // namespace qey
