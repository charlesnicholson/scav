#ifndef SCAV_INTERNAL_H_INCLUDED
#define SCAV_INTERNAL_H_INCLUDED

// Wraps a function definition in an anonymous namespace unless SCAV_TESTING is
// defined, so a test can declare and link it. For .cpp files only.

#ifdef SCAV_TESTING
#  define SCAV_INTERNAL_BEGIN
#  define SCAV_INTERNAL_END
#else
#  define SCAV_INTERNAL_BEGIN namespace {
#  define SCAV_INTERNAL_END }
#endif

#endif  // SCAV_INTERNAL_H_INCLUDED
