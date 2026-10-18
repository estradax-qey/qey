#include "cli/cli.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/commands.h"
#include "cli/result.h"
#include "qey/db.h"

namespace qey::cli {
namespace {

constexpr char kVersion[] = "qey 0.1.0";

struct GlobalOptions {
  std::string db_path;
  bool json = false;
  SyncPolicy sync = SyncPolicy::kAlways;
};

std::string Usage() {
  return "usage: qey [--db PATH] [--json] [--sync always|interval|manual] "
         "<command> [args...]\n"
         "\n"
         "qey is an embedded key-value store. This tool runs one command "
         "against a\n"
         "database file and exits; it never prompts.\n"
         "\n"
         "options:\n"
         "  --db PATH     the database file (or set QEY_DB). Created by "
         "commands that write.\n"
         "  --json        print {\"ok\": true, \"result\": ...} or {\"ok\": "
         "false, \"error\": ...}\n"
         "                as one line of JSON on standard output\n"
         "  --sync MODE   when to flush to disk: always (default), interval or "
         "manual.\n"
         "                The file is flushed when the command finishes in "
         "every mode.\n"
         "  --version     print the version\n"
         "\n"
         "exit codes: 0 success, 1 the command line is wrong, 2 the database "
         "refused\n"
         "\n"
         "run \"qey commands\" for the commands, \"qey help <command>\" for "
         "one.\n";
}

std::string CommandList() {
  std::ostringstream out;
  for (const CommandSpec& spec : Commands()) {
    out << "  " << spec.usage << "\n      " << spec.summary << "\n";
  }
  return out.str();
}

Result CommandsAsResult() {
  Result all = Result::Array();
  for (const CommandSpec& spec : Commands()) {
    Result one = Result::Object();
    one.fields.emplace_back("name", Result::String(spec.name));
    one.fields.emplace_back("usage", Result::String(spec.usage));
    one.fields.emplace_back("summary", Result::String(spec.summary));
    one.fields.emplace_back("writes", Result::Bool(spec.writes));
    all.items.push_back(std::move(one));
  }
  return all;
}

void ReportFailure(const Failure& failure, const GlobalOptions& global,
                   const CommandSpec* spec, std::ostream& out,
                   std::ostream& err) {
  if (global.json) {
    out << "{\"ok\":false,\"error\":{\"code\":" << JsonQuote(failure.code)
        << ",\"message\":" << JsonQuote(failure.message);
    if (spec != nullptr && failure.kind == Failure::Kind::kUsage) {
      out << ",\"usage\":" << JsonQuote(spec->usage);
    }
    out << "}}\n";
    return;
  }
  if (failure.kind == Failure::Kind::kUsage) {
    err << "qey: " << failure.message << "\n";
    if (spec != nullptr) err << "usage: qey " << spec->usage << "\n";
  } else {
    err << "qey: " << failure.code << ": " << failure.message << "\n";
  }
}

void ReportSuccess(const Result& result, const GlobalOptions& global,
                   std::ostream& out) {
  if (global.json) {
    std::ostringstream body;
    RenderJson(result, body);
    std::string text = body.str();
    if (!text.empty() && text.back() == '\n') text.pop_back();
    out << "{\"ok\":true,\"result\":" << text << "}\n";
  } else {
    RenderText(result, out);
  }
}

// Splits a command's arguments into positional ones and options, checking
// them against what the command accepts.
std::optional<Failure> ParseCommandArgs(const CommandSpec& spec,
                                        const std::vector<std::string>& raw,
                                        Invocation* inv) {
  auto usage = [](std::string message) {
    return Failure{Failure::Kind::kUsage, "USAGE", std::move(message)};
  };
  if (spec.raw_args) {
    inv->args = raw;
  } else {
    bool options_done = false;
    for (size_t i = 0; i < raw.size(); ++i) {
      const std::string& arg = raw[i];
      if (!options_done && arg == "--") {  // everything after is positional
        options_done = true;
        continue;
      }
      if (options_done || arg.size() < 3 || arg.compare(0, 2, "--") != 0) {
        inv->args.push_back(arg);
        continue;
      }
      std::string name = arg;
      std::optional<std::string> value;
      if (size_t eq = arg.find('='); eq != std::string::npos) {
        name = arg.substr(0, eq);
        value = arg.substr(eq + 1);
      }
      const OptionSpec* option = nullptr;
      for (const OptionSpec& candidate : spec.options) {
        if (candidate.name == name) option = &candidate;
      }
      if (option == nullptr) return usage("unknown option " + name);
      if (option->takes_value) {
        if (!value.has_value()) {
          if (i + 1 >= raw.size()) return usage(name + " needs a value");
          value = raw[++i];
        }
        inv->options[name] = *value;
      } else {
        if (value.has_value()) return usage(name + " takes no value");
        inv->options[name] = "";
      }
    }
  }
  if (inv->args.size() < spec.min_args || inv->args.size() > spec.max_args) {
    return usage("wrong number of arguments for \"" + spec.name + "\"");
  }
  return std::nullopt;
}

}  // namespace

int Run(const std::vector<std::string>& args, std::istream& in,
        std::ostream& out, std::ostream& err) {
  GlobalOptions global;
  if (const char* env = std::getenv("QEY_DB"); env != nullptr && *env != '\0') {
    global.db_path = env;
  }

  // Options that come before the command.
  size_t i = 0;
  std::optional<Failure> bad_option;
  for (; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--") {  // the command starts after it
      ++i;
      break;
    }
    if (arg.size() < 2 || arg.compare(0, 2, "--") != 0) break;
    std::string name = arg;
    std::optional<std::string> value;
    if (size_t eq = arg.find('='); eq != std::string::npos) {
      name = arg.substr(0, eq);
      value = arg.substr(eq + 1);
    }
    auto take_value = [&]() -> std::optional<std::string> {
      if (value.has_value()) return value;
      if (i + 1 < args.size()) return args[++i];
      return std::nullopt;
    };
    if (name == "--help") {
      out << Usage();
      return kExitOk;
    } else if (name == "--version") {
      out << kVersion << "\n";
      return kExitOk;
    } else if (name == "--json") {
      global.json = true;
    } else if (name == "--db") {
      std::optional<std::string> v = take_value();
      if (!v.has_value()) {
        bad_option =
            Failure{Failure::Kind::kUsage, "USAGE", "--db needs a path"};
        break;
      }
      global.db_path = *v;
    } else if (name == "--sync") {
      std::optional<std::string> v = take_value();
      if (v == "always") {
        global.sync = SyncPolicy::kAlways;
      } else if (v == "interval") {
        global.sync = SyncPolicy::kInterval;
      } else if (v == "manual") {
        global.sync = SyncPolicy::kManual;
      } else {
        bad_option = Failure{Failure::Kind::kUsage, "USAGE",
                             "--sync must be always, interval or manual"};
        break;
      }
    } else {
      bad_option =
          Failure{Failure::Kind::kUsage, "USAGE", "unknown option " + name};
      break;
    }
  }
  if (bad_option.has_value()) {
    ReportFailure(*bad_option, global, nullptr, out, err);
    if (!global.json) err << "run \"qey --help\" for usage\n";
    return kExitUsage;
  }
  if (i >= args.size()) {
    ReportFailure(Failure{Failure::Kind::kUsage, "USAGE", "no command given"},
                  global, nullptr, out, err);
    if (!global.json) err << Usage();
    return kExitUsage;
  }

  const std::string& name = args[i];
  const std::vector<std::string> rest(
      args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());

  // Commands that do not touch a database.
  if (name == "help") {
    if (rest.empty()) {
      out << Usage() << "\ncommands:\n" << CommandList();
      return kExitOk;
    }
    const CommandSpec* spec = FindCommand(rest[0]);
    if (spec == nullptr) {
      ReportFailure(Failure{Failure::Kind::kUsage, "USAGE",
                            "no command named \"" + rest[0] + "\""},
                    global, nullptr, out, err);
      return kExitUsage;
    }
    out << "usage: qey " << spec->usage << "\n\n" << spec->summary << "\n";
    return kExitOk;
  }
  if (name == "commands") {
    if (global.json) {
      ReportSuccess(CommandsAsResult(), global, out);
    } else {
      out << CommandList();
    }
    return kExitOk;
  }

  const CommandSpec* spec = FindCommand(name);
  if (spec == nullptr) {
    ReportFailure(Failure{Failure::Kind::kUsage, "USAGE",
                          "no command named \"" + name + "\""},
                  global, nullptr, out, err);
    if (!global.json) err << "run \"qey commands\" for the list\n";
    return kExitUsage;
  }

  Invocation inv;
  inv.in = &in;
  if (std::optional<Failure> failure = ParseCommandArgs(*spec, rest, &inv)) {
    ReportFailure(*failure, global, spec, out, err);
    return kExitUsage;
  }
  if (global.db_path.empty()) {
    ReportFailure(Failure{Failure::Kind::kUsage, "USAGE",
                          "no database: give --db PATH or set QEY_DB"},
                  global, spec, out, err);
    return kExitUsage;
  }
  inv.db_path = global.db_path;

  // Only a command that writes may create the file; a typo in the path should
  // not leave an empty database behind.
  if (!spec->writes && !std::filesystem::exists(global.db_path)) {
    ReportFailure(Failure{Failure::Kind::kDatabase, "NOT_FOUND",
                          "no database at " + global.db_path},
                  global, spec, out, err);
    return kExitDatabase;
  }

  OpenOptions options;
  options.sync = global.sync;
  options.sync_thread = false;  // one command, then exit
  StatusOr<std::unique_ptr<Db>> db = Db::Open(global.db_path, options);
  if (!db.ok()) {
    ReportFailure(Outcome::FromStatus(db.status()).failure.value(), global,
                  spec, out, err);
    return kExitDatabase;
  }
  inv.db = db->get();

  Outcome outcome = spec->run(inv);
  if (outcome.failure.has_value()) {
    ReportFailure(*outcome.failure, global, spec, out, err);
    return outcome.failure->kind == Failure::Kind::kUsage ? kExitUsage
                                                          : kExitDatabase;
  }
  ReportSuccess(outcome.result, global, out);
  return kExitOk;
}

}  // namespace qey::cli
