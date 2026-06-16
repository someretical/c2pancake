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

## Development

The instructions below are for Ubuntu 24.x

### Install LLVM + libstdc++ + other build tools

Install dependencies first
```
sudo apt update
sudo apt install libedit-dev zlib1g-dev libzstd-dev libcurl4-openssl-dev -y
```

Install LLVM, current supported version is 22.1.7
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
sudo apt install build-essential ninja-build cmake cmake-format -y
```

### Installing pancake compiler

Head to https://cakeml.org/regression.cgi and find the latest successful job on master.

Copy the download link for the artefacts.

```
mkdir build && cd build && wget -c <LINK> -O - | tar -xz
```
E.g.
```
mkdir build && cd build && wget -c https://cakeml.org/regression/artefacts/3364/cake-x64-64.tar.gz -O - | tar -xz
```

### VSCode extensions

Install from command line
```
code --install-extension \
  cheshirekow.cmake-format \
  cs128.cs128-clang-tidy \
  llvm-vs-code-extensions.lldb-dap \
  llvm-vs-code-extensions.vscode-clangd \
  ms-python.black-formatter \
  ms-python.debugpy \
  ms-python.isort \
  ms-python.python \
  ms-python.vscode-pylance \
  ms-python.vscode-python-envs \
  ms-vscode.cmake-tools \
  ms-vscode.cpp-devtools \
  ms-vscode.cpptools \
  twxs.cmake \
  vadimcn.vscode-lldb \
  xaver.clang-format
```

### CMake

Easiest method is to use the VSCode CMake extension.

Manual instructions (make sure you are in the project folder):
```
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang-22 -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-22 --no-warn-unused-cli -S . -B build -G "Ninja Multi-Config"
```

Debug build:
```
cmake --build build --config Debug --target all --
```

Release build:
```
cmake --build build --config Release --target all --
```