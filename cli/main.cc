#include <iostream>
#include <string>
#include <vector>

#include "cli/cli.h"

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::vector<std::string> args(argv + 1, argv + argc);
  return qey::cli::Run(args, std::cin, std::cout, std::cerr);
}
