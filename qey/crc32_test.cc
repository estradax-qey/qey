#include "qey/crc32.h"

#include "gtest/gtest.h"

namespace qey {
namespace {

TEST(Crc32Test, EmptyInput) { EXPECT_EQ(Crc32(""), 0u); }

TEST(Crc32Test, KnownVectors) {
  EXPECT_EQ(Crc32("123456789"), 0xCBF43926u);
  EXPECT_EQ(Crc32("a"), 0xE8B7BE43u);
  EXPECT_EQ(Crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(Crc32Test, DetectsSingleBitFlip) {
  EXPECT_NE(Crc32("hello"), Crc32("hellp"));
}

}  // namespace
}  // namespace qey
