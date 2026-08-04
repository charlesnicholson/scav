#ifndef SCAV_INTERNAL_H_INCLUDED
#define SCAV_INTERNAL_H_INCLUDED

// Brackets the *definition* of a function that would otherwise have internal
// linkage, so a test can declare the prototype itself and link. Never in a header.

#ifdef SCAV_TESTING
#  define SCAV_INTERNAL_BEGIN
#  define SCAV_INTERNAL_END
#else
#  define SCAV_INTERNAL_BEGIN namespace {
#  define SCAV_INTERNAL_END }
#endif

#endif  // SCAV_INTERNAL_H_INCLUDED
