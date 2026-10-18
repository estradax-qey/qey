#include <iostream>
#include <string>
#include <vector>

#include "bench/bench.h"

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return qey::bench::Main(args, std::cout, std::cerr);
}
