include(CMakeFindDependencyMacro)
find_dependency(asio-kcp CONFIG)
include("${CMAKE_CURRENT_LIST_DIR}/asio-kcp-targets.cmake")
