cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

project(asio-kcp)

find_package(kcp REQUIRED)
find_package(asio REQUIRED)

set(ASIO_KCP_HEADERS
    include/kcp.hpp
    include/error.hpp)
include_directories(include)

add_library(asio-kcp INTERFACE ${ASIO_KCP_HEADERS})
target_link_libraries(asio-kcp INTERFACE
    kcp::kcp
    asio::asio)
target_include_directories(asio-kcp INTERFACE $<INSTALL_INTERFACE:include>)

install(FILES ${ASIO_KCP_HEADERS} DESTINATION include)
install(FILES asio-kcp-config.cmake DESTINATION share/asio-kcp)

install(TARGETS
    asio-kcp
    EXPORT asio-kcp
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(EXPORT asio-kcp
    DESTINATION share/asio-kcp
    NAMESPACE asio-kcp::
    FILE asio-kcp-targets.cmake
)

add_executable(asio-kcp-client src/tcp_over_kcp.cpp)
target_link_libraries(asio-kcp-client PRIVATE asio-kcp)
