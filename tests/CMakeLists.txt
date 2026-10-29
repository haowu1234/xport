add_executable(xport_tests
  test_main.cpp
  test_wire.cpp
  test_cc.cpp
  test_transport.cpp
)
target_link_libraries(xport_tests PRIVATE xport)
target_include_directories(xport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME xport_tests COMMAND xport_tests)
