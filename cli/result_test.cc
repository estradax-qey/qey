#include "cli/result.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

namespace qey::cli {
namespace {

std::string Text(const Result& r) {
  std::ostringstream out;
  RenderText(r, out);
  return out.str();
}

std::string Json(const Result& r) {
  std::ostringstream out;
  RenderJson(r, out);
  return out.str();
}

TEST(Base64Test, KnownVectors) {
  EXPECT_EQ(Base64(""), "");
  EXPECT_EQ(Base64("f"), "Zg==");
  EXPECT_EQ(Base64("fo"), "Zm8=");
  EXPECT_EQ(Base64("foo"), "Zm9v");
  EXPECT_EQ(Base64("foob"), "Zm9vYg==");
  EXPECT_EQ(Base64("fooba"), "Zm9vYmE=");
  EXPECT_EQ(Base64("foobar"), "Zm9vYmFy");
  EXPECT_EQ(Base64(std::string("\0\xff\xfe", 3)), "AP/+");
}

TEST(Utf8Test, AcceptsValidAndRejectsEverythingElse) {
  EXPECT_TRUE(IsValidUtf8(""));
  EXPECT_TRUE(IsValidUtf8("plain ascii"));
  EXPECT_TRUE(IsValidUtf8("caf\xc3\xa9"));           // é
  EXPECT_TRUE(IsValidUtf8("\xe2\x82\xac"));          // €
  EXPECT_TRUE(IsValidUtf8("\xf0\x9f\x98\x80"));      // an emoji
  EXPECT_TRUE(IsValidUtf8(std::string("a\0b", 3)));  // NUL is valid UTF-8

  EXPECT_FALSE(IsValidUtf8("\xff"));
  EXPECT_FALSE(IsValidUtf8("\x80"));              // a stray continuation
  EXPECT_FALSE(IsValidUtf8("\xc3"));              // truncated
  EXPECT_FALSE(IsValidUtf8("\xe2\x82"));          // truncated
  EXPECT_FALSE(IsValidUtf8("\xc0\xaf"));          // overlong '/'
  EXPECT_FALSE(IsValidUtf8("\xe0\x80\xaf"));      // overlong
  EXPECT_FALSE(IsValidUtf8("\xed\xa0\x80"));      // a surrogate
  EXPECT_FALSE(IsValidUtf8("\xf4\x90\x80\x80"));  // past U+10FFFF
  EXPECT_FALSE(IsValidUtf8("\xc3\x28"));          // bad continuation
}

TEST(JsonQuoteTest, EscapesWhatJsonRequires) {
  EXPECT_EQ(JsonQuote("plain"), "\"plain\"");
  EXPECT_EQ(JsonQuote("a\"b\\c"), "\"a\\\"b\\\\c\"");
  EXPECT_EQ(JsonQuote("line\nbreak\ttab\r"), "\"line\\nbreak\\ttab\\r\"");
  EXPECT_EQ(JsonQuote(std::string("\x01\x1f", 2)), "\"\\u0001\\u001f\"");
  EXPECT_EQ(JsonQuote(std::string("nul\0x", 5)), "\"nul\\u0000x\"");
  EXPECT_EQ(JsonQuote("caf\xc3\xa9"), "\"caf\xc3\xa9\"");  // left as UTF-8
}

TEST(FormatDoubleTest, IsShortestRoundTripAndHandlesInfinity) {
  EXPECT_EQ(FormatDouble(0.1), "0.1");
  EXPECT_EQ(FormatDouble(91.5), "91.5");
  EXPECT_EQ(FormatDouble(78), "78");
  EXPECT_EQ(FormatDouble(-2.5), "-2.5");
  EXPECT_EQ(FormatDouble(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(FormatDouble(-std::numeric_limits<double>::infinity()), "-inf");
  for (double v : {1.0 / 3.0, 1e300, 5e-324, 123456789.123456789}) {
    // strtod, because std::stod throws for denormals.
    EXPECT_EQ(std::strtod(FormatDouble(v).c_str(), nullptr), v)
        << FormatDouble(v);
  }
}

TEST(RenderTest, ScalarsInTextAndJson) {
  EXPECT_EQ(Text(Result::Ok()), "OK\n");
  EXPECT_EQ(Json(Result::Ok()), "true\n");
  EXPECT_EQ(Text(Result::Nil()), "(nil)\n");
  EXPECT_EQ(Json(Result::Nil()), "null\n");
  EXPECT_EQ(Text(Result::Bool(true)), "true\n");
  EXPECT_EQ(Json(Result::Bool(false)), "false\n");
  EXPECT_EQ(Text(Result::Int(-7)), "-7\n");
  EXPECT_EQ(Json(Result::Int(-7)), "-7\n");
  EXPECT_EQ(Text(Result::Double(1.5)), "1.5\n");
  EXPECT_EQ(Json(Result::Double(1.5)), "1.5\n");
  EXPECT_EQ(Text(Result::String("hello")), "hello\n");
  EXPECT_EQ(Json(Result::String("hello")), "\"hello\"\n");
}

TEST(RenderTest, TextWritesStringsRawIncludingNewlinesAndNul) {
  EXPECT_EQ(Text(Result::String(std::string("a\nb\0c", 5))),
            std::string("a\nb\0c\n", 6));
}

TEST(RenderTest, JsonKeepsEveryByteOfNonUtf8Strings) {
  EXPECT_EQ(Json(Result::String(std::string("\xff\x00\x80", 3))),
            "{\"base64\":\"/wCA\"}\n");
  EXPECT_EQ(Json(Result::String("caf\xc3\xa9")), "\"caf\xc3\xa9\"\n");
}

TEST(RenderTest, InfiniteNumbersBecomeStringsInJson) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Json(Result::Double(inf)), "\"inf\"\n");
  EXPECT_EQ(Json(Result::Double(-inf)), "\"-inf\"\n");
  EXPECT_EQ(Text(Result::Double(-inf)), "-inf\n");
}

TEST(RenderTest, ArraysArePerLineInTextAndNestedPairsAreTabbed) {
  Result list = Result::Strings({"a", "b"});
  EXPECT_EQ(Text(list), "a\nb\n");
  EXPECT_EQ(Json(list), "[\"a\",\"b\"]\n");
  EXPECT_EQ(Text(Result::Array()), "");  // an empty list prints nothing
  EXPECT_EQ(Json(Result::Array()), "[]\n");

  Result pairs = Result::Array();
  Result pair = Result::Array();
  pair.items.push_back(Result::String("field"));
  pair.items.push_back(Result::Int(3));
  pairs.items.push_back(pair);
  EXPECT_EQ(Text(pairs), "field\t3\n");
  EXPECT_EQ(Json(pairs), "[[\"field\",3]]\n");
}

TEST(RenderTest, ObjectsKeepFieldOrder) {
  Result inner = Result::Object();
  inner.fields.emplace_back("x", Result::Int(1));
  Result o = Result::Object();
  o.fields.emplace_back("name", Result::String("qey"));
  o.fields.emplace_back("list", Result::Strings({"a", "b"}));
  o.fields.emplace_back("nested", inner);
  o.fields.emplace_back("none", Result::Nil());
  EXPECT_EQ(Json(o),
            "{\"name\":\"qey\",\"list\":[\"a\",\"b\"],\"nested\":{\"x\":1},"
            "\"none\":null}\n");
  EXPECT_EQ(Text(o),
            "name: qey\nlist:\n  a\n  b\nnested:\n  x: 1\nnone: (nil)\n");
}

TEST(RenderTest, RecordsInAListPrintTheirValuesOnATabbedLine) {
  Result list = Result::Array();
  for (const char* value : {"hi", "there"}) {
    Result entry = Result::Object();
    entry.fields.emplace_back("id", Result::String("5-0"));
    entry.fields.emplace_back("value", Result::String(value));
    list.items.push_back(entry);
  }
  EXPECT_EQ(Text(list), "5-0\thi\n5-0\tthere\n");
  EXPECT_EQ(Json(list),
            "[{\"id\":\"5-0\",\"value\":\"hi\"},"
            "{\"id\":\"5-0\",\"value\":\"there\"}]\n");
}

}  // namespace
}  // namespace qey::cli
