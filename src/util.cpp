#include "util.h"

#include <functional>

// Helper macro/function to combine hash values (boost::hash_combine style)
inline void hash_combine(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// Helper to hash an individual CXSourceLocation
inline std::size_t hash_location(const CXSourceLocation &loc) {
  CXFile file;
  unsigned line, column, offset;
  // Extract the raw data from the Clang location
  clang_getSpellingLocation(loc, &file, &line, &column, &offset);

  std::size_t seed = 0;
  hash_combine(seed, std::hash<void *>{}(file)); // File pointer
  hash_combine(seed, std::hash<unsigned>{}(line));
  hash_combine(seed, std::hash<unsigned>{}(column));
  hash_combine(seed, std::hash<unsigned>{}(offset));
  return seed;
}

// Inject the specialization into the std namespace
namespace std {
template <> struct hash<CursorHash> {
  std::size_t operator()(const CursorHash &c) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, hash_location(c.startLoc));
    hash_combine(seed, hash_location(c.endLoc));
    return seed;
  }
};
} // namespace std
