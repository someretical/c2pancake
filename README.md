# c2pancake

Original version developed at https://github.com/zhewenshen/c2pancake. Enhanced version developed by Yankai Zhu.

## Development

The instructions below are for Ubuntu 24.x

### Install LLVM + libstdc++ + other build tools

Install dependencies first
```
sudo apt update
sudo apt install libedit-dev zlib1g-dev libzstd-dev libcurl4-openssl-dev
```

Install LLVM, current supported version is 22.1.7
```
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22.1.7 all
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
sudo apt install build-essential ninja-build cmake
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