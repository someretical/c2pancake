#ifndef C2PANCAKE_UTIL_H
#define C2PANCAKE_UTIL_H

#include <clang-c/Index.h>

struct CursorHash {
  CXSourceLocation startLoc;
  CXSourceLocation endLoc;

  bool operator==(const CursorHash &other) const {
    return clang_equalLocations(startLoc, other.startLoc) &&
           clang_equalLocations(endLoc, other.endLoc);
  }
};

#endif // C2PANCAKE_UTIL_H