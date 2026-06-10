#include "codegen.h"
#include "ir_builder.h"
#include <fstream>
#include <iostream>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " <input.c> [output.pancake]\n";
    std::cout << "  Transpiles C code to Pancake\n";
    std::cout << "  If output file is not specified, outputs to stdout\n";
    return 1;
  }

  std::string inputFile = argv[1];

  pancake::IRBuilder builder;
  auto program = builder.build(inputFile);
  if (!program) {
    std::cerr << "Transpilation failed" << std::endl;
    return 1;
  }

  pancake::CodeGen codegen;
  std::string result = codegen.generate(*program);

  if (argc >= 3) {
    std::string outputFile = argv[2];
    std::ofstream out(outputFile);
    if (!out) {
      std::cerr << "Failed to open output file: " << outputFile << std::endl;
      return 1;
    }
    out << result;
    std::cout << "Successfully transpiled " << inputFile << " to " << outputFile
              << std::endl;
  } else {
    std::cout << result;
  }

  return 0;
}
