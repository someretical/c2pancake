# c2pancake

Original version developed at https://github.com/zhewenshen/c2pancake. Enhanced version developed by Yankai Zhu.

```
USAGE: c2pancake [options] <source0> [... <sourceN>]

OPTIONS:

Generic Options:

  --help                      - Display available options (--help-hidden for more)
  --help-list                 - Display list of available options (--help-list-hidden for more)
  --version                   - Display the version of this program

c2pancake options:

  --extra-arg=<string>        - Additional argument to append to the compiler command line
  --extra-arg-before=<string> - Additional argument to prepend to the compiler command line
  -p <string>                 - Build path

-p <build-path> is used to read a compile command database.

        For example, it can be a CMake build directory in which a file named
        compile_commands.json exists (use -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        CMake option to get this output). When no build path is specified,
        a search for compile_commands.json will be attempted through all
        parent paths of the first input file . See:
        https://clang.llvm.org/docs/HowToSetupToolingForLLVM.html for an
        example of setting up Clang Tooling on a source tree.

<source0> ... specify the paths of source files. These paths are
        looked up in the compile command database. If the path of a file is
        absolute, it needs to point into CMake's source tree. If the path is
        relative, the current working directory needs to be in the CMake
        source tree and the file must be in a subdirectory of the current
        working directory. "./" prefixes in the relative files will be
        automatically removed, but the rest of a relative path must be a
        suffix of a path in the compile command database.
```

## Examples

```
c2pancake tests/arith.c --
```

## Restrictions

### Before running the tool

1. Find any static symbols within functions that have non-zero initialisers. 
1. Make those static symbols global, and move the non-zero initialising statements to the start of the entry point of the program. Since this transpiler doesn't act as a linker, you'll have to do this manually. 
1. Rewrite any switch statements with loops inside them. This is problematic because the loops can have case statements inside them which cannot be correctly transpiled.
1. Switch statement cases should be rewritten so they don't contain any `break;`s in the middle. Automatically rewriting this requires gotos (or advanced control flow analysis which is really annoying) which are not supported in Pancake. Also, case statements are only allowed at the top level scope in the switch statement since it's too complicated to parse otherwise.

### Other restrictions

All existing structs and unions will be rewritten to use ONLY (u)int32_t/(u)int64_t (same as word size). Bitfields will be promoted to full width types. Warnings will be generated if any field is downsized. Struct definitions outside the global scope will be hoisted to global scope with name mangling.

The only exception are structs which are only accessed through a pointer (e.g. memory mapped regions). They will not be rewritten.

Any externally defined symbol will also be rewritten to the word size and a warning generated if necessary.

All arrays in functions are considered "static" (but not shareable across threads) so no recursion is allowed. They will be hoisted into the global scope with name mangling.

Any stack variable which has its address taken will also be hoisted into the global scope with name mangling.

Floating point types are not supported and will be promoted into ints.

The insertion of `#include <stdint.h>` at the top of a file may fail if there are complex processor directives present.

## Development

The instructions below are for Ubuntu 24.x.

At a high level
- LLVM 22.1.8 (use later versions at risk of breaking compatibility...)
- GCC 16 and libstdc++16 (clang's libc++ doesn't support enough modern C++(26) features)
- Python 3.12.3 (for fetching clang internal headers from GitHub)

### Install LLVM + libstdc++ + other build tools

Install LLVM development dependencies first
```
sudo apt update
sudo apt install libedit-dev zlib1g-dev libzstd-dev libcurl4-openssl-dev -y
```

Install LLVM development libraries, current supported version is 22.1.8. N.B. it is not possible to specify the minor and patch versions.
```
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22 all
```

Install GCC/libstdc++ 16
```
sudo apt install software-properties-common -y
sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y
sudo apt update
sudo apt install gcc-16 g++-16 libstdc++-16-dev -y
```

Install other build tools
```
sudo apt install build-essential ninja-build cmake cmake-format python3 python3-venv -y
```

The clang internal headers for LLVM 22.1.8 are already included. If targeting a newer release (even if it is a minor release), run the helper python script to fetch the newer headers.
```
/usr/bin/python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python fetch_codegen_headers.py --version llvmorg-22.1.8
```

### Installing pancake compiler

Head to https://cakeml.org/regression.cgi and find the latest successful job on master.

Copy the download link for the artefacts.

```
mkdir build && cd build && wget -c <LINK> -O - | tar -xz
```
E.g.
```
mkdir build && cd build && wget -c https://cakeml.org/regression/artefacts/3389/cake-x64-64.tar.gz -O - | tar -xz
```

### VSCode extensions

Install from command line
```
code --install-extension \
  ms-python.black-formatter \
  ms-python.debugpy \
  ms-python.isort \
  ms-python.python \
  ms-python.vscode-pylance \
  ms-python.vscode-python-envs \
  llvm-vs-code-extensions.lldb-dap \
  llvm-vs-code-extensions.vscode-clangd \
  ms-vscode.cmake-tools \
  ms-vscode.cpp-devtools \
  ms-vscode.cpptools \
  cheshirekow.cmake-format
```

### CMake

Easiest method is to use the VSCode CMake extension.

Manual instructions (make sure you are in the project folder):
```
cmake \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm-22/lib/cmake/clang \ 
  -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
  -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang-22 \
  -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-22 \
  --no-warn-unused-cli \
  -S . \
  -B build \
  -G "Ninja Multi-Config"
```

Debug build:
```
cmake --build build --config Debug --target all --
```

Release build:
```
cmake --build build --config Release --target all --
```

### Other commands

Dump Clang AST
```
clang-22 -fsyntax-only -Xclang -ast-dump-all <file.c>
```
