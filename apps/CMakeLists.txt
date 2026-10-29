add_executable(xport_demo xport_demo.cpp)
target_link_libraries(xport_demo PRIVATE xport)

add_executable(xport_bench xport_bench.cpp)
target_link_libraries(xport_bench PRIVATE xport)

add_executable(xport_ccprobe xport_ccprobe.cpp)
target_link_libraries(xport_ccprobe PRIVATE xport)

add_executable(xport_cpubench xport_cpubench.cpp)
target_link_libraries(xport_cpubench PRIVATE xport)

add_executable(xport_udp_bench xport_udp_bench.cpp)
target_link_libraries(xport_udp_bench PRIVATE xport)
