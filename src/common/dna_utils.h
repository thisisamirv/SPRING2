// Declares DNA sequence helpers for reverse complements and bit-packed read
// serialization.

#ifndef SPRING_DNA_UTILS_H_
#define SPRING_DNA_UTILS_H_

#include <string>

namespace spring {

extern const char chartorevchar[128];

// Map a/c/g/t/n to ACGTN; leave other bytes unchanged.
[[nodiscard]] inline char dna_ascii_upper(char base) {
  return (base >= 'a' && base <= 'z')
             ? static_cast<char>(base - ('a' - 'A'))
             : base;
}

void reverse_complement(char *input_bases, char *output_bases, int readlen);
std::string reverse_complement(const std::string &input_bases, int readlen);

} // namespace spring

#endif // SPRING_DNA_UTILS_H_
