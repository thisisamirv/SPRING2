cmake_minimum_required(VERSION 3.15)
project(pthash LANGUAGES CXX)
add_library(pthash INTERFACE)
# pthash's own headers are SYSTEM (suppress their warnings when instantiated
# inside first-party TUs); src/ stays a regular include so first-party
# headers like pthash_windefs.h remain fully diagnosed.
target_include_directories(pthash SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(pthash INTERFACE ${CMAKE_SOURCE_DIR}/src)
