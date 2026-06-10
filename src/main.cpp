#include "codegen.h"
#include "ir_builder.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::print("Usage: {} <input.c> [output.pancake]\n", argv[0]);
    std::print("  Transpiles C code to Pancake\n");
    std::print("  If output file is not specified, outputs to stdout\n");
    return 1;
  }

  // Using fs::path handles cross-platform paths properly
  fs::path input_file{argv[1]};

  pancake::IRBuilder builder;
  auto program = builder.build(input_file.string());
  if (!program) {
    // std::println outputs cleanly to stderr when passed std::cerr
    std::println(std::cerr, "Transpilation failed");
    return 1;
  }

  pancake::CodeGen codegen;
  std::string result = codegen.generate(*program);

  // If an output file is provided
  if (argc >= 3) {
    fs::path output_file{argv[2]};
    std::ofstream out(output_file);

    if (!out) {
      std::println(std::cerr, "Failed to open output file: {}",
                   output_file.string());
      return 1;
    }

    out << result;
    std::println("Successfully transpiled {} to {}", input_file.string(),
                 output_file.string());
  } else {
    // Direct format string output without dealing with std::cout streams
    std::print("{}", result);
  }

  return 0;
}