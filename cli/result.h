#ifndef QEY_CLI_RESULT_H_
#define QEY_CLI_RESULT_H_

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qey::cli {

// What a command produces, independent of how it is printed.
struct Result {
  enum class Kind {
    kOk,   // the command succeeded and has nothing to report
    kNil,  // the key, field or member does not exist
    kBool,
    kInt,
    kDouble,
    kString,  // arbitrary bytes
    kArray,
    kObject,  // named fields, in order
  };

  Kind kind = Kind::kOk;
  bool boolean = false;
  int64_t integer = 0;
  double number = 0;
  std::string text;
  std::vector<Result> items;                           // kArray
  std::vector<std::pair<std::string, Result>> fields;  // kObject

  static Result Ok() { return Result(); }
  static Result Nil() { return Make(Kind::kNil); }
  static Result Bool(bool v) {
    Result r = Make(Kind::kBool);
    r.boolean = v;
    return r;
  }
  static Result Int(int64_t v) {
    Result r = Make(Kind::kInt);
    r.integer = v;
    return r;
  }
  static Result Double(double v) {
    Result r = Make(Kind::kDouble);
    r.number = v;
    return r;
  }
  static Result String(std::string v) {
    Result r = Make(Kind::kString);
    r.text = std::move(v);
    return r;
  }
  static Result Array() { return Make(Kind::kArray); }
  static Result Object() { return Make(Kind::kObject); }

  static Result Strings(const std::vector<std::string>& values) {
    Result r = Array();
    for (const std::string& v : values) r.items.push_back(String(v));
    return r;
  }

 private:
  static Result Make(Kind kind) {
    Result r;
    r.kind = kind;
    return r;
  }
};

// Writes `result` for people and shell scripts: strings are written as they
// are, one value per line.
void RenderText(const Result& result, std::ostream& out);

// Writes `result` as one line of JSON. Strings that are not valid UTF-8 are
// written as {"base64": "..."} so that no byte is lost or altered, and
// infinite numbers as the strings "inf" and "-inf".
void RenderJson(const Result& result, std::ostream& out);

// Writes `bytes` as a JSON string. `bytes` must be valid UTF-8.
std::string JsonQuote(std::string_view bytes);

std::string Base64(std::string_view bytes);
bool IsValidUtf8(std::string_view bytes);

// The shortest text that reads back as exactly `value`.
std::string FormatDouble(double value);

}  // namespace qey::cli

#endif  // QEY_CLI_RESULT_H_
