#include "cli/result.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace qey::cli {
namespace {

void WriteJson(const Result& r, std::ostream& out) {
  using Kind = Result::Kind;
  switch (r.kind) {
    case Kind::kOk:
      out << "true";
      break;
    case Kind::kNil:
      out << "null";
      break;
    case Kind::kBool:
      out << (r.boolean ? "true" : "false");
      break;
    case Kind::kInt:
      out << r.integer;
      break;
    case Kind::kDouble:
      if (std::isinf(r.number)) {
        out << (r.number > 0 ? "\"inf\"" : "\"-inf\"");
      } else {
        out << FormatDouble(r.number);
      }
      break;
    case Kind::kString:
      if (IsValidUtf8(r.text)) {
        out << JsonQuote(r.text);
      } else {
        out << "{\"base64\":" << JsonQuote(Base64(r.text)) << "}";
      }
      break;
    case Kind::kArray: {
      out << "[";
      bool first = true;
      for (const Result& item : r.items) {
        if (!first) out << ",";
        first = false;
        WriteJson(item, out);
      }
      out << "]";
      break;
    }
    case Kind::kObject: {
      out << "{";
      bool first = true;
      for (const auto& [name, value] : r.fields) {
        if (!first) out << ",";
        first = false;
        out << JsonQuote(name) << ":";
        WriteJson(value, out);
      }
      out << "}";
      break;
    }
  }
}

// One value on one line, for use inside a list.
void WriteScalar(const Result& r, std::ostream& out) {
  using Kind = Result::Kind;
  switch (r.kind) {
    case Kind::kOk:
      out << "OK";
      break;
    case Kind::kNil:
      out << "(nil)";
      break;
    case Kind::kBool:
      out << (r.boolean ? "true" : "false");
      break;
    case Kind::kInt:
      out << r.integer;
      break;
    case Kind::kDouble:
      out << FormatDouble(r.number);
      break;
    case Kind::kString:
      out.write(r.text.data(), static_cast<std::streamsize>(r.text.size()));
      break;
    case Kind::kArray: {
      // A nested list, such as a field and its value, is joined by tabs.
      bool first = true;
      for (const Result& item : r.items) {
        if (!first) out << '\t';
        first = false;
        WriteScalar(item, out);
      }
      break;
    }
    case Kind::kObject: {
      // A record in a list, such as a stream entry: its values joined by tabs.
      bool first = true;
      for (const auto& field : r.fields) {
        if (!first) out << '\t';
        first = false;
        WriteScalar(field.second, out);
      }
      break;
    }
  }
}

}  // namespace

// Writes the fields of an object, one per line, indented by `indent` spaces.
void WriteFields(const Result& object, int indent, std::ostream& out) {
  using Kind = Result::Kind;
  const std::string pad(static_cast<size_t>(indent), ' ');
  for (const auto& [name, value] : object.fields) {
    if (value.kind == Kind::kArray) {
      out << pad << name << ":\n";
      for (const Result& item : value.items) {
        out << pad << "  ";
        WriteScalar(item, out);
        out << '\n';
      }
    } else if (value.kind == Kind::kObject) {
      out << pad << name << ":\n";
      WriteFields(value, indent + 2, out);
    } else {
      out << pad << name << ": ";
      WriteScalar(value, out);
      out << '\n';
    }
  }
}

void RenderText(const Result& result, std::ostream& out) {
  using Kind = Result::Kind;
  if (result.kind == Kind::kArray) {
    for (const Result& item : result.items) {
      WriteScalar(item, out);
      out << '\n';
    }
    return;
  }
  if (result.kind == Kind::kObject) {
    WriteFields(result, 0, out);
    return;
  }
  WriteScalar(result, out);
  out << '\n';
}

void RenderJson(const Result& result, std::ostream& out) {
  WriteJson(result, out);
  out << '\n';
}

std::string JsonQuote(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char ch : bytes) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0xf];
        } else {
          out += ch;
        }
    }
  }
  out += '"';
  return out;
}

std::string Base64(std::string_view bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t n = (static_cast<uint8_t>(bytes[i]) << 16) |
                       (static_cast<uint8_t>(bytes[i + 1]) << 8) |
                       static_cast<uint8_t>(bytes[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
    i += 3;
  }
  if (i + 1 == bytes.size()) {
    const uint32_t n = static_cast<uint8_t>(bytes[i]) << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == bytes.size()) {
    const uint32_t n = (static_cast<uint8_t>(bytes[i]) << 16) |
                       (static_cast<uint8_t>(bytes[i + 1]) << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

bool IsValidUtf8(std::string_view bytes) {
  size_t i = 0;
  while (i < bytes.size()) {
    const uint8_t c = static_cast<uint8_t>(bytes[i]);
    size_t length;
    uint32_t code;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xe0) == 0xc0) {
      length = 2;
      code = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      length = 3;
      code = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      length = 4;
      code = c & 0x07;
    } else {
      return false;
    }
    if (i + length > bytes.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const uint8_t next = static_cast<uint8_t>(bytes[i + k]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    // Reject overlong forms, surrogates and values past U+10FFFF.
    static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code < kMin[length] || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) {
      return false;
    }
    i += length;
  }
  return true;
}

std::string FormatDouble(double value) {
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  char buffer[64];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value);
  return std::string(buffer, end);
}

}  // namespace qey::cli
