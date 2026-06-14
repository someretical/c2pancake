#include "codegen.h"
#include "ir_builder.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::println("Usage: {} <input.c> [output.pancake]", argv[0]);
    std::println("  Transpiles C code to Pancake");
    std::println("  If output file is not specified, outputs to stdout");
    return 1;
  }

  // Using fs::path handles cross-platform paths properly
  fs::path input_file{argv[1]};

  pancake::IRBuilder builder;
  auto program = builder.build(input_file.string());
  if (!program) {
    std::println(std::cerr, "Transpilation failed");
    return 1;
  }

  pancake::CodeGen codegen;
  auto result = codegen.generate(*program);

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
    std::print("{}", result);
  }

  return 0;
}