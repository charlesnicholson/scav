#ifndef SCAV_COLD_H_INCLUDED
#define SCAV_COLD_H_INCLUDED

// Marks a rarely run function: compiled for size, and calls to it treated as unlikely.
// Empty under MSVC.
#ifdef _MSC_VER
#  define SCAV_COLD
#elif defined(__clang__)
#  define SCAV_COLD [[gnu::cold, clang::minsize]]
#else
#  define SCAV_COLD [[gnu::cold]]
#endif

#endif  // SCAV_COLD_H_INCLUDED
