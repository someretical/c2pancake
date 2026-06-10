cmake_minimum_required(VERSION 3.10)
project(c2pancake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${LLVM_CMAKE_DIR}")
include(AddLLVM)

# Get the LLVM installation prefix
get_filename_component(LLVM_INSTALL_PREFIX "${LLVM_DIR}/../../.." ABSOLUTE)
message(STATUS "LLVM install prefix: ${LLVM_INSTALL_PREFIX}")

find_library(LIBCLANG_LIBRARY clang
    HINTS ${LLVM_LIBRARY_DIRS} "${LLVM_INSTALL_PREFIX}/lib")
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
    HINTS ${LLVM_INCLUDE_DIRS} "${LLVM_INSTALL_PREFIX}/include")

if(NOT LIBCLANG_LIBRARY OR NOT LIBCLANG_INCLUDE_DIR)
    message(FATAL_ERROR "libclang not found!")
endif()

include_directories(${LIBCLANG_INCLUDE_DIR})
include_directories(include)

set(SOURCES
    src/ir_builder.cpp
    src/codegen.cpp
    src/main.cpp
)

add_executable(c2pancake ${SOURCES})
target_link_libraries(c2pancake ${LIBCLANG_LIBRARY})
