#ifndef QEY_CLI_CLI_H_
#define QEY_CLI_CLI_H_

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace qey::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;     // the command line is wrong
inline constexpr int kExitDatabase = 2;  // the database refused or failed

// Runs one command line, `args` being everything after the program name.
// Writes results to `out` and, unless --json is given, errors to `err`; with
// --json both go to `out` as JSON so that a caller reads one stream. `in` is
// read only by commands that ask for it (set --stdin). Never prompts.
// Returns an exit code.
int Run(const std::vector<std::string>& args, std::istream& in,
        std::ostream& out, std::ostream& err);

}  // namespace qey::cli

#endif  // QEY_CLI_CLI_H_
