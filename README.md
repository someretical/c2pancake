# c2pancake

C to [Pancake](https://cakeml.org/pancake.html) transpiler. Pancake is a new 'thin' language constructed with the aims of (1) ease of source-level verification, (2) user-controlled small memory footprint, and (3) simple transportation of correctness results from source to executable binary code.

```
USAGE: c2pancake [options] <source0> [... <sourceN>]

OPTIONS:

Generic Options:

  --help                      - Display available options (--help-hidden for more)
  --help-list                 - Display list of available options (--help-list-hidden for more)
  --version                   - Display the version of this program

c2pancake options:

  --end=<string>              - End the pipeline after this pass (including retries) (default: empty, meaning end after last pass)
  --extra-arg=<string>        - Additional argument to append to the compiler command line
  --extra-arg-before=<string> - Additional argument to prepend to the compiler command line
  --max-pass-retries=<ulong>  - Maximum number of retries for a pass before moving to the next pass (default: 10)
  -p <string>                 - Build path
  --pointer-bits=<value>      - Override pointer width
    =32                       -   32-bit
    =64                       -   64-bit
  --start=<string>            - Start the pipeline at this pass (default: empty, meaning start at beginning)

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

Note if see a warning in the output like `WARNING: Maximum number of passes (X) reached, consider increasing the limit`, use `--max-pass-retries=` option as described above!

## Restrictions

### Before running the tool

1. Remove any static symbols within functions.
1. Rewrite any switch statements with loops inside them. This is problematic because the loops can have case statements inside them which cannot be correctly transpiled.
1. Switch statement cases should be rewritten so they don't contain any `break;`s in the middle of cases (i.e. `break`'s should only appear at the end of a case statement). Also, case statements are only allowed at the top level scope in the switch statement (i.e. don't nest them inside of `{}`'s).
1. Static inline functions in headers should be placed in a special header file. Then run the preprocessor with args `-nostdinc -I/path/to/header` to ONLY process the special header file. This will ensure the functions are inlined into the source file and thus processed by c2pancake. Don't include anything that shouldn't be converted (e.g. C library functions and #defines).
1. Remove all variadic functions defined within the source file along with calls to them.
1. Remove all variable length arrays include incomplete array members within records (structs/unions).
1. If there are any function declarations that should not be rewritten to follow Pancake's calling convention, move them into header files.
1. If there are any function definitions that should not be rewritten to follow Pancake's calling convention, annotate them with `[[clang::annotate("__c2pnk_external_entry_point")]]`.
    - E.g. the [`init`](https://github.com/seL4/microkit/blob/main/docs/manual.md#entry-points) function in [sDDF](https://github.com/au-ts/sDDF) has the signature `void init(void)` and applications are expected to implement it.
1. Global variables shared across threads should be annotated with `[[clang::annotate("__c2pnk_shared_global")]]`.
1. Macros like `assert()` cannot be converted properly so they should be expanded beforehand or replaced with a function.

### Other restrictions

1. All arrays and structs in functions are considered "static" (but not shareable across threads) so no recursion is allowed. They will be hoisted into the global scope with name mangling.
1. Any stack variable which has its address taken will also be hoisted into the global scope with name mangling.
1. Floating point types are not supported at all.
1. Function argument evaluation order will NOT be preserved. Complicated args requiring hoisting of variables will be evaluated first while simpler expressions will be left as-is.

## Development

The instructions below are for Ubuntu 24.x.

At a high level
- LLVM 22.1.8 (use later versions at risk of breaking compatibility...)
- GCC 16 and libstdc++16 (clang's libc++ doesn't support enough modern C++(26) features)
- Python 3.12.3 (for fetching clang internal headers from GitHub)
- at LEAST 24GB of RAM if using WSL + VSCode and building locally 💀
  - for those at Trustworthy Systems working on this project, message Yankai for the script to build on dedicated infrastructure :-)

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

Test `uart.c`
```
./build/Debug/c2pancake /home/one/sddf/drivers/serial/imx/uart.c -- \
  -ffreestanding -mstrict-align -mcpu=cortex-a53 -mtune=cortex-a53 -target aarch64-none-elf \
  -I/home/one/microkit_tutorial/microkit-sdk-2.2.0/board/maaxboard/debug/include \
  -I/home/one/sddf/examples/serial/include \
  -I/home/one/sddf/include \
  -I/home/one/sddf/include/microkit \
  -I/home/one/sddf/include/sddf/util/custom_libc \
  -I/home/one/sddf/drivers/serial/imx/include \
  -I/usr/lib/gcc-cross/aarch64-linux-gnu/13/include
```