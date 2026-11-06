add_executable(xport_tests
  test_main.cpp
  test_wire.cpp
  test_cc.cpp
  test_transport.cpp
  test_batch_io.cpp
  test_rate_sample.cpp
  test_bbr.cpp
)
target_link_libraries(xport_tests PRIVATE xport)
target_include_directories(xport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME xport_tests COMMAND xport_tests)
