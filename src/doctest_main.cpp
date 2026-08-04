// Split out so doctest's implementation is compiled exactly once per test
// executable and no test source has to remember the define.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
