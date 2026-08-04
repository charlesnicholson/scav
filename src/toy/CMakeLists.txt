scav_static_library(scavtoy toy.cpp)
scav_install_library(scavtoy toy)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_toy_tests toy_tests.cpp)
  target_link_libraries(scav_toy_tests PRIVATE scavtoy_testable)
  scav_expect_test_failure(scav_toy_tests "*deliberate failure*")
endif()
