#ifndef QEY_CLI_COMMANDS_H_
#define QEY_CLI_COMMANDS_H_

#include <cstddef>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/result.h"
#include "qey/db.h"
#include "qey/status.h"

namespace qey::cli {

// Why a command did not succeed.
struct Failure {
  enum class Kind {
    kUsage,     // the command line is wrong
    kDatabase,  // the command was understood but the database refused it
  };

  Kind kind = Kind::kUsage;
  std::string code;  // a stable name such as "WRONG_TYPE"
  std::string message;
};

struct Outcome {
  Result result;
  std::optional<Failure> failure;

  static Outcome Ok(Result result) {
    Outcome o;
    o.result = std::move(result);
    return o;
  }
  static Outcome Usage(std::string message) {
    Outcome o;
    o.failure = Failure{Failure::Kind::kUsage, "USAGE", std::move(message)};
    return o;
  }
  static Outcome FromStatus(const Status& status);
};

// The stable name of a status code, such as "WRONG_TYPE".
const char* StatusCodeName(StatusCode code);

// A command line, split into what a command works with.
struct Invocation {
  Db* db = nullptr;
  std::vector<std::string> args;               // positional arguments
  std::map<std::string, std::string> options;  // "--ttl" -> "30s"; flags -> ""
  std::istream* in = nullptr;
  std::string db_path;
};

struct OptionSpec {
  std::string name;  // "--ttl"
  bool takes_value = false;
};

struct CommandSpec {
  std::string name;
  std::string usage;
  std::string summary;
  bool writes = false;  // may change the database; creates the file if needed
  size_t min_args = 0;
  size_t max_args = 0;
  std::vector<OptionSpec> options;
  // Pass every argument through untouched, including ones that start with
  // "--". For commands that do their own parsing.
  bool raw_args = false;
  std::function<Outcome(Invocation&)> run;
};

// All commands, in the order they are listed.
const std::vector<CommandSpec>& Commands();

const CommandSpec* FindCommand(std::string_view name);

}  // namespace qey::cli

#endif  // QEY_CLI_COMMANDS_H_
