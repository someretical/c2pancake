cmake_minimum_required(VERSION 3.20)
project(c2pancake)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SECURE_BUILD_FOR_PRODUCTION "Enable production-specific hardening" OFF)
option(SECURE_BUILD_FOR_DEVELOPMENT "Enable development-specific strictness" ON)
include(CheckPIESupported)
check_pie_supported()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
  message(FATAL_ERROR "LLVM 22 required")
endif()
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_definitions(${LLVM_DEFINITIONS})

set(GCC_TOOLCHAIN_PATH
    "/usr/lib/gcc/x86_64-linux-gnu/16"
    CACHE PATH "Path to the GCC/libstdc++ 16+ toolchain directory")
set(GCC_BASE_INCLUDE_PATH
    "/usr/include/c++/16"
    CACHE PATH "Path to the main GCC/libstdc++ 16+ include directory")
set(GCC_ARCH_INCLUDE_PATH
    "/usr/include/x86_64-linux-gnu/c++/16"
    CACHE
      PATH
      "Path to the architecture-specific GCC/libstdc++ 16+ include directory")
foreach(PATH_VAR GCC_TOOLCHAIN_PATH GCC_BASE_INCLUDE_PATH GCC_ARCH_INCLUDE_PATH)
  if(NOT EXISTS "${${PATH_VAR}}")
    message(FATAL_ERROR "Path does not exist: ${PATH_VAR} -> ${${PATH_VAR}}")
  endif()
endforeach()

add_executable(
  c2pancake
  src/Utils.cpp
  src/Pipeline.cpp
  src/Pass_LowerBitfieldOps.cpp
  src/Pass_TransformLogicalExpressions.cpp
  src/Pass_NormaliseIfStatements.cpp
  src/Pass_SwitchToIf.cpp
  src/Pass_LoopsToWhile.cpp
  src/Pass_PromoteRecords.cpp
  src/Pass_HoistArraysAndAddresses.cpp
  src/Pass_CompoundAssignment.cpp
  src/CodeGen.cpp
  src/IRBuilder.cpp
  src/main.cpp)

target_include_directories(c2pancake PRIVATE include)

# recommended security hardening flags
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

target_compile_options(
  c2pancake
  PRIVATE -O2
          -Wall
          -Wformat
          -Wformat=2
          -Wconversion
          -Wimplicit-fallthrough
          -Werror=format-security
          -fstrict-flex-arrays=3
          -fstack-clash-protection
          -fstack-protector-strong
          -U_FORTIFY_SOURCE)

target_compile_definitions(c2pancake PRIVATE _FORTIFY_SOURCE=3
                                             _GLIBCXX_ASSERTIONS)

target_link_options(
  c2pancake
  PRIVATE
  -Wl,-z,nodlopen
  -Wl,-z,noexecstack
  -Wl,-z,relro
  -Wl,-z,now
  -Wl,--as-needed
  -Wl,--no-copy-dt-needed-entries)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(
    c2pancake PRIVATE -Wtrampolines -fzero-init-padding-bits=all
                      -Wbidi-chars=any)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
  target_compile_options(c2pancake PRIVATE -fcf-protection=full)
  target_link_options(c2pancake PRIVATE -fcf-protection=full)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
  target_compile_options(c2pancake PRIVATE -mbranch-protection=standard)
  target_link_options(c2pancake PRIVATE -mbranch-protection=standard)
endif()

if(SECURE_BUILD_FOR_PRODUCTION)
  target_compile_options(
    c2pancake PRIVATE -fno-delete-null-pointer-checks -fno-strict-overflow
                      -fno-strict-aliasing -ftrivial-auto-var-init=zero)
endif()

# if(SECURE_BUILD_FOR_DEVELOPMENT) target_compile_options(c2pancake PRIVATE
# -Werror) endif()

target_compile_options(
  c2pancake
  PRIVATE $<$<COMPILE_LANGUAGE:C>: -Werror=implicit
          -Werror=incompatible-pointer-types -Werror=int-conversion >
          $<$<COMPILE_LANGUAGE:C>:-fexceptions>)
# end of recommended security hardening flags

target_compile_definitions(c2pancake
                           PRIVATE GCC_TOOLCHAIN="${GCC_TOOLCHAIN_PATH}")
target_include_directories(
  c2pancake SYSTEM PRIVATE ${GCC_BASE_INCLUDE_PATH} ${GCC_ARCH_INCLUDE_PATH}
                           ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_link_libraries(
  c2pancake
  PRIVATE clangTooling
          clangToolingCore
          clangTransformer
          clangASTMatchers
          clangAST
          clangRewrite
          clangFrontend
          clangFormat
          clangSerialization
          clangDriver
          clangParse
          clangSema
          clangAnalysis
          clangEdit
          clangLex
          clangBasic
          LLVMSupport)
