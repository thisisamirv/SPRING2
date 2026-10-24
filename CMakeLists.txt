# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies. Implementation details live under cmake/.

cmake_minimum_required(VERSION 3.31)

# CMAKE_POLICY_DEFAULT_CMP0091 must be set before project() so that every
# subsequent cmake_minimum_required() call inside vendor subdirectories also
# inherits NEW behavior for CMP0091.  When a vendor subdirectory calls
# cmake_minimum_required(VERSION 3.10), CMake pushes a new policy stack frame
# and leaves CMP0091 "unset" (because 3.10 < 3.15 when the policy landed).
# Without this variable the built-in default for an unset CMP0091 is OLD,
# which silently ignores CMAKE_MSVC_RUNTIME_LIBRARY and re-injects /MD.
# Setting this default to NEW makes CMAKE_MSVC_RUNTIME_LIBRARY respected in
# every scope, eliminating the LNK4098 defaultlib conflict and ensuring the
# static CRT is used project-wide.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

project(SPRING2)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(ProjectSetup)
include(Toolchain)
include(Utilities)
include(Compiler)
include(OpenMP)
include(HostTools)
include(Clangd)
include(Vendors)
include(Sources)
include(Targets)
include(Install)
include(Tests)
