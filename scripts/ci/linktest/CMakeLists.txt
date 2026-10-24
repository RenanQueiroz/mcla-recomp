# A program linked against a ReXGlue SDK bundle, to see that what the bundle's libraries
# promise links and runs with the pinned llvm-mingw: scripts/ci/link-test.ps1 builds it.
cmake_minimum_required(VERSION 3.25)
project(mcla_link_test CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(rexglue REQUIRED)
add_executable(link_test main.cpp)
target_link_libraries(link_test PRIVATE rex::runtime)
