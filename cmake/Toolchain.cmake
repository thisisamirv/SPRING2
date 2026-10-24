# Included from the top-level CMakeLists.txt.

# =============================================================================
# MSVC runtime library (CRT) selection and compiler cache
# =============================================================================

if(MSVC)
    # CMAKE_MSVC_RUNTIME_LIBRARY is also passed as a -D cache argument from
    # bld.bat (conda) and analogous build scripts. The CACHE+FORCE here
    # ensures it also takes effect for local/IDE builds where the -D arg is
    # not provided.
    #
    # ClangCL uses LLVM libomp, which creates OMP worker threads via the raw
    # Win32 CreateThread() API rather than _beginthreadex(). With the static
    # CRT (/MT), the MSVC per-thread runtime state (_tiddata) is not
    # initialised via DllMain(DLL_THREAD_ATTACH) for those worker threads,
    # because static CRT code lives in the EXE rather than a DLL. Clang's
    # C++ exception-handling frames interact with that uninitialised state in
    # a way that causes 0xC0000005 (ACCESS_VIOLATION) crashes inside OMP
    # parallel regions. The dynamic CRT (/MD) avoids this because
    # vcruntime140.dll's DllMain fires DLL_THREAD_ATTACH for every thread,
    # including CreateThread-created worker threads, and fully initialises
    # per-thread state before any user code runs.
    #
    # MSVC uses vcomp.dll for OpenMP, which calls _beginthreadex() and
    # therefore initialises per-thread CRT state correctly even with /MT.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # ClangCL: dynamic CRT (/MD, /MDd).
        # Avoid a generator expression here: CMake 4.x's try_compile for
        # indexed_bzip2's find_package(Threads) runs in Debug config and
        # evaluates "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>" to
        # "MultiThreadedDLLDebug", which is not accepted for the clang-cl
        # C compiler in that context.  A plain conditional on CMAKE_BUILD_TYPE
        # works correctly for the Ninja single-config generator used on CI and
        # for local builds.
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_spring_clangcl_crt "MultiThreadedDLLDebug")
        else()
            set(_spring_clangcl_crt "MultiThreadedDLL")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "${_spring_clangcl_crt}"
            CACHE STRING
            "MSVC runtime library"
            FORCE
        )
    else()
        # MSVC: static CRT (/MT, /MTd)
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            CACHE STRING
            "MSVC runtime library"
            FORCE
        )
    endif()
endif()

option(
    SPRING_ENABLE_COMPILER_CACHE
    "Use sccache/ccache as compiler launcher when available"
    ON
)
if(SPRING_ENABLE_COMPILER_CACHE)
    if(WIN32)
        find_program(SPRING_COMPILER_CACHE_PROGRAM NAMES sccache)
    else()
        find_program(SPRING_COMPILER_CACHE_PROGRAM NAMES ccache sccache)
    endif()

    if(SPRING_COMPILER_CACHE_PROGRAM)
        message(
            NOTICE
            "Using compiler cache launcher: ${SPRING_COMPILER_CACHE_PROGRAM}"
        )
        set(CMAKE_C_COMPILER_LAUNCHER
            "${SPRING_COMPILER_CACHE_PROGRAM}"
            CACHE STRING
            "C compiler launcher"
            FORCE
        )
        set(CMAKE_CXX_COMPILER_LAUNCHER
            "${SPRING_COMPILER_CACHE_PROGRAM}"
            CACHE STRING
            "CXX compiler launcher"
            FORCE
        )
    else()
        message(
            NOTICE
            "Compiler cache requested, but no sccache/ccache executable was found on PATH."
        )
    endif()
endif()

# The project supports GNU-like and MSVC-compatible C++20 toolchains.

# =============================================================================
# Build type
# =============================================================================

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(NOTICE "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE
        "Release"
        CACHE STRING
        "Choose the type of build."
        FORCE
    )
    set_property(
        CACHE CMAKE_BUILD_TYPE
        PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo"
    )
endif()

# =============================================================================
# Platform-specific vendor build options
# =============================================================================

option(LIBRAPIDARCHIVE_WITH_ISAL "Build rapidgzip with Intel ISA-L support" ON)
if(APPLE)
    # ISA-L support is generally omitted on macOS to ensure build success across architectures.
    set(LIBRAPIDARCHIVE_WITH_ISAL OFF CACHE BOOL "" FORCE)
    # Several vendored libs (e.g. libdeflate) unconditionally compile both their
    # arm/ and x86/ CPU-feature-detection sources and rely on #ifdef guards to
    # make the non-matching one an empty translation unit. Apple's ranlib warns
    # "has no symbols" for those empty .o files; this is harmless, so silence it.
    set(CMAKE_C_ARCHIVE_FINISH
        "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>"
    )
    set(CMAKE_CXX_ARCHIVE_FINISH
        "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>"
    )
endif()

# =============================================================================
# Global C++ standard and project-wide settings
# =============================================================================

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckLinkerFlag)
include(CheckIPOSupported)
include(CheckSymbolExists)
include(GNUInstallDirs)
