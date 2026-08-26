# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies.

cmake_minimum_required(VERSION 3.31)

# =============================================================================
# Project metadata, policies, and versioning
# =============================================================================

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

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX
        "${CMAKE_SOURCE_DIR}/out"
        CACHE PATH
        "Install path prefix, prepended onto install directories."
        FORCE
    )
endif()

set(SPRING_VERSION_HEADER "${CMAKE_SOURCE_DIR}/src/common/version.h")
file(
    STRINGS "${SPRING_VERSION_HEADER}"
    SPRING_VERSION_DECLARATION
    REGEX "constexpr char VERSION\\[\\] = \"[^\"]+\";"
)
if(NOT SPRING_VERSION_DECLARATION)
    message(
        FATAL_ERROR
        "Unable to extract the SPRING2 version string from ${SPRING_VERSION_HEADER}"
    )
endif()

string(
    REGEX REPLACE ".*\"([^\"]+)\".*"
    "\\1"
    SPRING_VERSION_STRING
    "${SPRING_VERSION_DECLARATION}"
)

set(SPRING_VERSION_PRERELEASE 0)
if(
    SPRING_VERSION_STRING
        MATCHES
        "^([0-9]+)\\.([0-9]+)\\.([0-9]+)-rc\\.([0-9]+)$"
)
    set(SPRING_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(SPRING_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(SPRING_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(SPRING_VERSION_PRERELEASE "${CMAKE_MATCH_4}")
elseif(SPRING_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(SPRING_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(SPRING_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(SPRING_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
    message(
        FATAL_ERROR
        "Unsupported SPRING2 version format: ${SPRING_VERSION_STRING}"
    )
endif()

set(SPRING_WINDOWS_FILE_VERSION
    "${SPRING_VERSION_MAJOR},${SPRING_VERSION_MINOR},${SPRING_VERSION_PATCH},${SPRING_VERSION_PRERELEASE}"
)
set(SPRING_WINDOWS_PRODUCT_VERSION "${SPRING_WINDOWS_FILE_VERSION}")
set(SPRING_WINDOWS_PUBLISHER "SPRING2 contributors")
set(SPRING_WINDOWS_PRODUCT_NAME "SPRING2")
set(SPRING_WINDOWS_FILE_DESCRIPTION "SPRING2 FASTQ/FASTA compressor")
set(SPRING_WINDOWS_INTERNAL_NAME "spring2")
set(SPRING_WINDOWS_ORIGINAL_FILENAME "spring2.exe")
set(SPRING_WINDOWS_PROJECT_URL "https://github.com/thisisamirv/SPRING2")

if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/docs/assets/icons/logo.ico")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(
        TO_NATIVE_PATH
        "${CMAKE_SOURCE_DIR}/docs/assets/icons/logo.ico"
        SPRING_WINDOWS_ICON_NATIVE_PATH
    )
    string(
        REPLACE "\\"
        "\\\\"
        SPRING_WINDOWS_ICON_RESOURCE_PATH
        "${SPRING_WINDOWS_ICON_NATIVE_PATH}"
    )
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/spring2.rc"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/spring2.rc"
        @ONLY
    )
endif()

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()

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

# =============================================================================
# Utility functions
# =============================================================================

function(replace_in_file_if_needed file_path old_text new_text)
    if(NOT EXISTS "${file_path}")
        message(FATAL_ERROR "Cannot patch missing file: ${file_path}")
    endif()

    file(READ "${file_path}" file_contents)
    string(FIND "${file_contents}" "${new_text}" replacement_index)
    if(NOT replacement_index EQUAL -1)
        return()
    endif()

    string(
        REPLACE "${old_text}"
        "${new_text}"
        updated_contents
        "${file_contents}"
    )
    if(file_contents STREQUAL updated_contents)
        message(
            FATAL_ERROR
            "Expected text not found while patching ${file_path}"
        )
    endif()

    file(WRITE "${file_path}" "${updated_contents}")
endfunction()

function(run_command_checked working_directory description)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        RESULT_VARIABLE command_result
    )
    if(NOT command_result EQUAL 0)
        message(
            FATAL_ERROR
            "${description} failed with exit code ${command_result}"
        )
    endif()
endfunction()

# =============================================================================
# Compiler frontend and toolchain detection
# =============================================================================

set(SPRING_MSVC_FRONTEND FALSE)
if(
    MSVC
    OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
    OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
)
    set(SPRING_MSVC_FRONTEND TRUE)
endif()

set(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS FALSE)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    message(NOTICE "Detected compiler: GNU Compiler Collection")
    set(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS TRUE)
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    if(SPRING_MSVC_FRONTEND)
        message(
            NOTICE
            "Detected compiler: LLVM Clang with an MSVC-compatible frontend"
        )
    else()
        message(NOTICE "Detected compiler: LLVM Clang or Apple Clang")
        set(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS TRUE)
    endif()
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    message(NOTICE "Detected compiler: Microsoft Visual C++")
else()
    message(
        FATAL_ERROR
        "Compiler currently not supported by CMake build: ${CMAKE_CXX_COMPILER_ID}"
    )
endif()

# =============================================================================
# Linker selection
# =============================================================================

option(
    SPRING_ENABLE_FAST_LINKER
    "Use a faster linker when supported (mold/lld)"
    ON
)
if(SPRING_ENABLE_FAST_LINKER)
    set(SPRING_FAST_LINKER_FLAG "")

    if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS AND NOT APPLE AND NOT WIN32)
        find_program(SPRING_MOLD_PROGRAM NAMES mold ld.mold)
        if(SPRING_MOLD_PROGRAM)
            check_linker_flag(CXX "-fuse-ld=mold" SPRING_LINKER_SUPPORTS_MOLD)
            if(SPRING_LINKER_SUPPORTS_MOLD)
                set(SPRING_FAST_LINKER_FLAG "-fuse-ld=mold")
            endif()
        endif()
    endif()

    if(
        SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS
        AND NOT APPLE
        AND NOT SPRING_FAST_LINKER_FLAG
    )
        find_program(SPRING_LLD_PROGRAM NAMES ld.lld lld)
        if(SPRING_LLD_PROGRAM)
            check_linker_flag(CXX "-fuse-ld=lld" SPRING_LINKER_SUPPORTS_LLD)
            if(SPRING_LINKER_SUPPORTS_LLD)
                set(SPRING_FAST_LINKER_FLAG "-fuse-ld=lld")
            endif()
        endif()
    endif()

    if(SPRING_FAST_LINKER_FLAG)
        message(NOTICE "Using faster linker flag: ${SPRING_FAST_LINKER_FLAG}")
        add_link_options("${SPRING_FAST_LINKER_FLAG}")
    else()
        message(
            NOTICE
            "Fast linker requested, but no supported linker flag was detected."
        )
    endif()
endif()

# =============================================================================
# Interprocedural optimization (LTO)
# =============================================================================

option(SPRING_ENABLE_IPO "Enable Interprocedural Optimization if supported" ON)

if(
    NOT WIN32
    AND SPRING_ENABLE_IPO
    AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION
)
    check_ipo_supported(RESULT result)
    if(result)
        message(NOTICE "Support for Interprocedural Optimization detected.")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# =============================================================================
# Optimization and compiler flags
# =============================================================================

# Native tuning is the default; portability mode restricts x86-family code
# generation to SSE4.1-compatible instructions.
option(spring_optimize_for_native "Build with -march=native" OFF)
option(
    spring_optimize_for_portability
    "Build with -msse4.1 (Maximum portability)"
    ON
)

set(SPRING_X86_FAMILY FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(SPRING_X86_FAMILY TRUE)
endif()

message(NOTICE "Setting compiler flags")
if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS)
    set(FLAGS "${FLAGS} -O3")
else()
    message(
        NOTICE
        "Skipping GNU-style optimization flags for the current compiler frontend"
    )
endif()
if(spring_optimize_for_portability)
    if(SPRING_X86_FAMILY)
        if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS)
            set(FLAGS "${FLAGS} -msse4.1")
        else()
            message(
                NOTICE
                "spring_optimize_for_portability is already satisfied by the MSVC-compatible frontend defaults; skipping explicit SSE4.1 flag"
            )
        endif()
    else()
        message(
            NOTICE
            "spring_optimize_for_portability requested on non-x86 target ${CMAKE_SYSTEM_PROCESSOR}; skipping -msse4.1"
        )
    endif()
elseif(spring_optimize_for_native)
    if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS)
        set(FLAGS "${FLAGS} -march=native")
    else()
        message(
            NOTICE
            "spring_optimize_for_native is not currently mapped for the MSVC-compatible frontend; leaving architecture selection to the generator/toolchain"
        )
    endif()
endif()
if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS AND NOT APPLE)
    # Omitting the frame pointer is skipped on Apple platforms: Apple's
    # compact-unwind "frameless" stepper has been observed to crash inside
    # libunwind.dylib while walking the stack for a thrown C++ exception on
    # frame-pointer-less code, turning a caught std::runtime_error into a
    # SIGSEGV instead of a clean error message.
    set(FLAGS "${FLAGS} -fomit-frame-pointer")
endif()
if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS)
    set(FLAGS "${FLAGS} -fstrict-aliasing")
endif()

if(CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    if(SPRING_COMPILER_SUPPORTS_GNU_STYLE_FLAGS)
        set(FLAGS "${FLAGS} -g")
    endif()
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FLAGS}")

# =============================================================================
# OpenMP configuration
# =============================================================================

# OpenMP is required across Spring and several bundled compression components.
if(
    WIN32
    AND (
        CMAKE_C_COMPILER_ID STREQUAL "Clang"
        OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    )
)
    if(MSVC)
        message(
            NOTICE
            "Configuring OpenMP manually for ClangCL using LLVM runtime"
        )
        set(SPRING_CONFIGURE_OPENMP_MANUALLY ON)
        set(SPRING_OPENMP_MANUAL_COMPILE_FLAG "/openmp:llvm")
        # Find LLVM's libomp.lib
        get_filename_component(
            CLANGCL_BIN_DIR
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY
        )
        get_filename_component(
            LLVM_INSTALL_PREFIX
            "${CLANGCL_BIN_DIR}"
            DIRECTORY
        )
        find_library(
            SPRING_OPENMP_MANUAL_LIBRARY
            NAMES omp libomp
            HINTS "${LLVM_INSTALL_PREFIX}/lib" "${CLANGCL_BIN_DIR}/../lib"
            PATH_SUFFIXES x64
        )
        if(NOT SPRING_OPENMP_MANUAL_LIBRARY)
            message(
                FATAL_ERROR
                "Unable to locate LLVM libomp for ClangCL under ${LLVM_INSTALL_PREFIX}/lib"
            )
        endif()
        unset(SPRING_OPENMP_COMPONENTS)
    else()
        message(
            NOTICE
            "Configuring OpenMP manually for Windows Clang using -fopenmp"
        )
        set(SPRING_CONFIGURE_OPENMP_MANUALLY ON)
        set(SPRING_OPENMP_MANUAL_COMPILE_FLAG "-fopenmp")
        get_filename_component(
            SPRING_CLANG_BIN_DIR
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY
        )
        get_filename_component(
            SPRING_CLANG_PREFIX
            "${SPRING_CLANG_BIN_DIR}"
            DIRECTORY
        )
        find_library(
            SPRING_OPENMP_MANUAL_LIBRARY
            NAMES omp libomp.dll.a libomp
            HINTS "${SPRING_CLANG_PREFIX}/lib"
        )
        if(NOT SPRING_OPENMP_MANUAL_LIBRARY)
            message(
                FATAL_ERROR
                "Unable to locate libomp for Windows Clang under ${SPRING_CLANG_PREFIX}/lib"
            )
        endif()
        unset(SPRING_OPENMP_COMPONENTS)
    endif()
elseif(
    CMAKE_C_COMPILER_ID STREQUAL "MSVC"
    OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
)
    # MSVC ARM64 has internal compiler errors with OpenMP in libbsc/libsais.c
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
        message(NOTICE "Disabling OpenMP on MSVC ARM64 due to compiler bug")
        set(SPRING_DISABLE_OPENMP ON)
    else()
        message(NOTICE "Configuring OpenMP for MSVC using the LLVM runtime")
        set(OpenMP_RUNTIME_MSVC llvm)
        set(SPRING_OPENMP_COMPONENTS C CXX)
    endif()
else()
    set(SPRING_OPENMP_COMPONENTS C CXX)
endif()

if(APPLE)
    # Automatically locate Homebrew libomp if not already specified
    if(NOT OpenMP_FOUND AND NOT DEFINED OpenMP_ROOT)
        set(BREW_LIBOMP_DIR "")
        if(EXISTS "/opt/homebrew/opt/libomp")
            set(BREW_LIBOMP_DIR "/opt/homebrew/opt/libomp")
        elseif(EXISTS "/usr/local/opt/libomp")
            set(BREW_LIBOMP_DIR "/usr/local/opt/libomp")
        endif()

        if(BREW_LIBOMP_DIR)
            message(
                NOTICE
                "Found Homebrew libomp at ${BREW_LIBOMP_DIR}, configuring OpenMP automatically"
            )
            set(OpenMP_ROOT
                "${BREW_LIBOMP_DIR}"
                CACHE PATH
                "Path to OpenMP root"
            )
            set(OpenMP_C_FLAGS
                "-Xpreprocessor -fopenmp"
                CACHE STRING
                "C flags for OpenMP"
            )
            set(OpenMP_CXX_FLAGS
                "-Xpreprocessor -fopenmp"
                CACHE STRING
                "CXX flags for OpenMP"
            )
            set(OpenMP_C_LIB_NAMES
                "omp"
                CACHE STRING
                "C library names for OpenMP"
            )
            set(OpenMP_CXX_LIB_NAMES
                "omp"
                CACHE STRING
                "CXX library names for OpenMP"
            )
            set(OpenMP_omp_LIBRARY
                "${BREW_LIBOMP_DIR}/lib/libomp.dylib"
                CACHE FILEPATH
                "Path to omp library"
            )
        endif()
    endif()
endif()

if(NOT SPRING_DISABLE_OPENMP)
    if(SPRING_CONFIGURE_OPENMP_MANUALLY)
        set(OpenMP_FOUND TRUE)
        set(OpenMP_CXX_FOUND TRUE)
        set(OpenMP_CXX_FLAGS "${SPRING_OPENMP_MANUAL_COMPILE_FLAG}")
        get_filename_component(
            SPRING_OPENMP_MANUAL_LIBRARY_NAME
            "${SPRING_OPENMP_MANUAL_LIBRARY}"
            NAME_WE
        )
        set(OpenMP_CXX_LIB_NAMES "${SPRING_OPENMP_MANUAL_LIBRARY_NAME}")
        set(OpenMP_CXX_LIBRARIES "${SPRING_OPENMP_MANUAL_LIBRARY}")
        if(NOT TARGET OpenMP::OpenMP_CXX)
            add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
            set_target_properties(
                OpenMP::OpenMP_CXX
                PROPERTIES
                    INTERFACE_COMPILE_OPTIONS
                        "${SPRING_OPENMP_MANUAL_COMPILE_FLAG}"
                    INTERFACE_LINK_LIBRARIES "${SPRING_OPENMP_MANUAL_LIBRARY}"
            )
        endif()
    else()
        find_package(OpenMP REQUIRED COMPONENTS ${SPRING_OPENMP_COMPONENTS})
    endif()

    if(NOT OpenMP_C_FOUND AND OpenMP_CXX_FOUND)
        message(NOTICE "Reusing OpenMP CXX settings for C sources")
        set(OpenMP_C_FLAGS "${OpenMP_CXX_FLAGS}")
        set(OpenMP_C_LIB_NAMES "${OpenMP_CXX_LIB_NAMES}")
        set(OpenMP_C_LIBRARIES "${OpenMP_CXX_LIBRARIES}")
        set(OpenMP_C_INCLUDE_DIRS "${OpenMP_CXX_INCLUDE_DIRS}")
    endif()

    message(NOTICE "OPENMP FOUND")
    set(OpenMP_DEFINITIONS "-DLIBBSC_OPENMP_SUPPORT -DLIBSAIS_OPENMP")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS} ${OpenMP_DEFINITIONS}"
    )
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} ${OpenMP_DEFINITIONS}"
    )

    set(SPRING_OPENMP_INCLUDE_DIRS)
    if(OpenMP_C_INCLUDE_DIRS)
        list(APPEND SPRING_OPENMP_INCLUDE_DIRS ${OpenMP_C_INCLUDE_DIRS})
    endif()
    if(OpenMP_CXX_INCLUDE_DIRS)
        list(APPEND SPRING_OPENMP_INCLUDE_DIRS ${OpenMP_CXX_INCLUDE_DIRS})
    endif()
    if(NOT SPRING_OPENMP_INCLUDE_DIRS AND DEFINED OpenMP_ROOT)
        set(SPRING_OPENMP_ROOT_INCLUDE_DIR "${OpenMP_ROOT}/include")
        if(EXISTS "${SPRING_OPENMP_ROOT_INCLUDE_DIR}/omp.h")
            list(
                APPEND SPRING_OPENMP_INCLUDE_DIRS
                "${SPRING_OPENMP_ROOT_INCLUDE_DIR}"
            )
        endif()
    endif()
    list(REMOVE_DUPLICATES SPRING_OPENMP_INCLUDE_DIRS)
else()
    message(NOTICE "OpenMP disabled")
    set(OpenMP_DEFINITIONS "")
endif()

# =============================================================================
# Path variables
# =============================================================================

set(source_dir ${CMAKE_SOURCE_DIR}/src)
set(vendor_dir ${CMAKE_SOURCE_DIR}/vendor)
set(build_vendor_dir ${vendor_dir})
set(include_dir ${CMAKE_SOURCE_DIR}/src)
set(tools_host_dir ${CMAKE_SOURCE_DIR}/tools/host)

# =============================================================================
# Host tools: Ninja
# =============================================================================

if(CMAKE_GENERATOR MATCHES "^Ninja")
    set(SPRING_VENDORED_NINJA_EXECUTABLE "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
            set(SPRING_VENDORED_NINJA_EXECUTABLE
                "${tools_host_dir}/ninja/winarm64/ninja.exe"
            )
        else()
            set(SPRING_VENDORED_NINJA_EXECUTABLE
                "${tools_host_dir}/ninja/win/ninja.exe"
            )
        endif()
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(SPRING_VENDORED_NINJA_EXECUTABLE
            "${tools_host_dir}/ninja/mac/ninja"
        )
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
            set(SPRING_VENDORED_NINJA_EXECUTABLE
                "${tools_host_dir}/ninja/linux-aarch64/ninja"
            )
        else()
            set(SPRING_VENDORED_NINJA_EXECUTABLE
                "${tools_host_dir}/ninja/linux/ninja"
            )
        endif()
    endif()

    if(CMAKE_MAKE_PROGRAM)
        file(
            REAL_PATH "${CMAKE_MAKE_PROGRAM}"
            SPRING_CONFIGURED_NINJA_EXECUTABLE_REAL
        )
        set(CMAKE_MAKE_PROGRAM
            "${SPRING_CONFIGURED_NINJA_EXECUTABLE_REAL}"
            CACHE FILEPATH
            "Ninja executable used by CMake"
            FORCE
        )
        message(NOTICE "Using configured Ninja: ${CMAKE_MAKE_PROGRAM}")
    elseif(
        SPRING_VENDORED_NINJA_EXECUTABLE
        AND EXISTS "${SPRING_VENDORED_NINJA_EXECUTABLE}"
    )
        file(
            REAL_PATH "${SPRING_VENDORED_NINJA_EXECUTABLE}"
            SPRING_NINJA_EXECUTABLE_REAL
        )
        set(CMAKE_MAKE_PROGRAM
            "${SPRING_NINJA_EXECUTABLE_REAL}"
            CACHE FILEPATH
            "Ninja executable used by CMake"
            FORCE
        )
        message(NOTICE "Using vendored Ninja: ${CMAKE_MAKE_PROGRAM}")
    endif()
endif()

# =============================================================================
# Host tools: NASM
# =============================================================================

set(SPRING_HAVE_NASM
    OFF
    CACHE BOOL
    "Whether a NASM assembler is available for ISA-L"
    FORCE
)

if(DEFINED SPRING_NASM_EXECUTABLE AND SPRING_NASM_EXECUTABLE)
    if(NOT EXISTS "${SPRING_NASM_EXECUTABLE}")
        message(
            FATAL_ERROR
            "Configured SPRING_NASM_EXECUTABLE does not exist: ${SPRING_NASM_EXECUTABLE}"
        )
    endif()
    file(REAL_PATH "${SPRING_NASM_EXECUTABLE}" SPRING_NASM_EXECUTABLE_REAL)
    set(SPRING_HAVE_NASM
        ON
        CACHE BOOL
        "Whether a NASM assembler is available for ISA-L"
        FORCE
    )
    set(SPRING_NASM_EXECUTABLE
        "${SPRING_NASM_EXECUTABLE_REAL}"
        CACHE FILEPATH
        "Path to the NASM assembler used for ISA-L"
        FORCE
    )
    message(NOTICE "Using configured NASM: ${SPRING_NASM_EXECUTABLE}")
else()
    set(SPRING_NASM_EXECUTABLE
        ""
        CACHE FILEPATH
        "Path to the NASM assembler used for ISA-L"
        FORCE
    )
    set(SPRING_VENDORED_NASM_EXECUTABLE "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
            set(SPRING_VENDORED_NASM_EXECUTABLE
                "${tools_host_dir}/nasm/win64/nasm.exe"
            )
        elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
            set(SPRING_VENDORED_NASM_EXECUTABLE
                "${tools_host_dir}/nasm/win32/nasm.exe"
            )
        endif()
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(SPRING_VENDORED_NASM_EXECUTABLE
            "${tools_host_dir}/nasm/macosx/nasm"
        )
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(SPRING_VENDORED_NASM_EXECUTABLE "${tools_host_dir}/nasm/linux/nasm")
    endif()

    if(
        SPRING_VENDORED_NASM_EXECUTABLE
        AND EXISTS "${SPRING_VENDORED_NASM_EXECUTABLE}"
    )
        file(
            REAL_PATH "${SPRING_VENDORED_NASM_EXECUTABLE}"
            SPRING_NASM_EXECUTABLE_REAL
        )
        set(SPRING_HAVE_NASM
            ON
            CACHE BOOL
            "Whether a NASM assembler is available for ISA-L"
            FORCE
        )
        set(SPRING_NASM_EXECUTABLE
            "${SPRING_NASM_EXECUTABLE_REAL}"
            CACHE FILEPATH
            "Path to the NASM assembler used for ISA-L"
            FORCE
        )
        message(NOTICE "Using vendored NASM: ${SPRING_NASM_EXECUTABLE}")
    else()
        find_program(SPRING_SYSTEM_NASM_EXECUTABLE NAMES nasm)
        if(SPRING_SYSTEM_NASM_EXECUTABLE)
            set(SPRING_HAVE_NASM
                ON
                CACHE BOOL
                "Whether a NASM assembler is available for ISA-L"
                FORCE
            )
            set(SPRING_NASM_EXECUTABLE
                "${SPRING_SYSTEM_NASM_EXECUTABLE}"
                CACHE FILEPATH
                "Path to the NASM assembler used for ISA-L"
                FORCE
            )
            message(NOTICE "Using system NASM: ${SPRING_NASM_EXECUTABLE}")
        else()
            message(
                NOTICE
                "NASM was not found; ISA-L will use non-NASM fallback sources where available."
            )
        endif()
    endif()
endif()

# =============================================================================
# Editor integration (clangd)
# =============================================================================

# Emit a matching clangd configuration so editor diagnostics see the same
# compiler include path and OpenMP flags as the CMake build.
set(SPRING_CLANGD_FLAGS
    "\"-fopenmp\", \"-I${include_dir}\", \"-I${source_dir}/assays\", \"-I${source_dir}/common\", \"-I${source_dir}/decompress\", \"-I${source_dir}/encode\", \"-I${source_dir}/preprocess\", \"-I${source_dir}/reorder\", \"-I${source_dir}/workflow\", \"-I${vendor_dir}\", \"-I${vendor_dir}/zstd\", \"-I${build_vendor_dir}/cloudflare_zlib\", \"-I${build_vendor_dir}/libarchive\""
)
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/pthash\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/libbsc\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/libdeflate\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/indexed_bzip2\"")
string(
    APPEND SPRING_CLANGD_FLAGS
    ", \"-I${build_vendor_dir}/indexed_bzip2/isa-l/include\""
)
string(
    APPEND SPRING_CLANGD_FLAGS
    ", \"-I${build_vendor_dir}/indexed_bzip2/isa-l\""
)
foreach(openmp_include_dir IN LISTS SPRING_OPENMP_INCLUDE_DIRS)
    string(APPEND SPRING_CLANGD_FLAGS ", \"-I${openmp_include_dir}\"")
endforeach()
set(CLANGD_CONTENT
    "CompileFlags:\n  Add: [${SPRING_CLANGD_FLAGS}, \"-Wno-empty-translation-unit\"]\n  Remove: [\"-Winvalid-pch\", \"-include\", \"-include-pch\"]\n---\nIf:\n  PathMatch: ['.*\\.(hpp|cpp|cc|cxx)$', 'src/.*\\.h$']\nCompileFlags:\n  Add: [\"-std=c++20\"]\n"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/.clangd")
    file(READ "${CMAKE_SOURCE_DIR}/.clangd" EXISTING_CLANGD_CONTENT)
endif()
if(NOT CLANGD_CONTENT STREQUAL EXISTING_CLANGD_CONTENT)
    file(WRITE "${CMAKE_SOURCE_DIR}/.clangd" "${CLANGD_CONTENT}")
endif()
# Keep a workspace-stable compile_commands.json copy under out/clangd.
# This avoids symlink permission/portability issues across platforms.
if(
    CMAKE_GENERATOR
        MATCHES
        "^(Ninja|Unix Makefiles|MinGW Makefiles|MSYS Makefiles)$"
)
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/out/clangd")
    add_custom_target(
        copy_compile_commands
        ALL
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/out/clangd/compile_commands.json"
        COMMENT "Copying compile_commands.json into out/clangd"
    )
else()
    message(
        NOTICE
        "Skipping compile_commands.json out/clangd copy for generator: ${CMAKE_GENERATOR}"
    )
endif()

# =============================================================================
# Runtime linking options
# =============================================================================

# Option to statically link common GCC runtimes so the executable can run
# on Windows `cmd.exe` without shipping DLLs next to the binary.
set(_spring_static_runtimes_default OFF)
if(WIN32)
    set(_spring_static_runtimes_default ON)
endif()
option(
    SPRING_STATIC_RUNTIMES
    "Statically link libgcc/libstdc++/libgomp on supported toolchains"
    ${_spring_static_runtimes_default}
)
unset(_spring_static_runtimes_default)

# =============================================================================
# Vendor subdirectory helper
# =============================================================================

macro(add_vendor_subdirectory_no_install source_dir binary_dir)
    set(_spring_prev_suppress_vendor_install
        "${SPRING_SUPPRESS_VENDOR_INSTALL}"
    )
    set(SPRING_SUPPRESS_VENDOR_INSTALL TRUE)
    set(SPRING_SUPPRESS_VENDOR_INSTALL
        TRUE
        CACHE INTERNAL
        "Suppress install rules for vendored dependencies"
        FORCE
    )
    add_subdirectory("${source_dir}" "${binary_dir}" ${ARGN})
    set(SPRING_SUPPRESS_VENDOR_INSTALL
        "${_spring_prev_suppress_vendor_install}"
    )
    set(SPRING_SUPPRESS_VENDOR_INSTALL
        "${_spring_prev_suppress_vendor_install}"
        CACHE INTERNAL
        "Suppress install rules for vendored dependencies"
        FORCE
    )
endmacro()

# =============================================================================
# Vendor directories
# =============================================================================

set(PTHASH_DIR "${build_vendor_dir}/pthash")
set(CLOUDFLARE_ZLIB_DIR ${build_vendor_dir}/cloudflare_zlib)
set(ZSTD_DIR ${build_vendor_dir}/zstd)
set(INDEXED_BZIP2_DIR ${build_vendor_dir}/indexed_bzip2)
set(LIBBSC_DIR ${build_vendor_dir}/libbsc)
set(LIBDEFLATE_DIR ${build_vendor_dir}/libdeflate)
set(LIBARCHIVE_DIR ${build_vendor_dir}/libarchive)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

# =============================================================================
# Toolchain miscompilation workarounds
# =============================================================================

# Workaround for ClangCL x64 miscompilation on Windows.
# ClangCL with /O2 miscompiles various code paths for complex real-world inputs
# (e.g., reads >80bp, varied quality, complex IDs), causing access violations
# (0xC0000005). The issue affects both vendor libraries and the main application.
# We override the Release-build optimization flags globally in this scope so
# that all targets -- including spring2.exe and the test executables -- compile
# with /Od. Vendor subdirectories that already apply this override locally
# (libbsc, cloudflare_zlib) will continue to work correctly.
if(
    WIN32
    AND SPRING_MSVC_FRONTEND
    AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$"
)
    get_filename_component(
        _spring_compiler_name_we
        "${CMAKE_CXX_COMPILER}"
        NAME_WE
    )
    if(_spring_compiler_name_we STREQUAL "clang-cl")
        foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
            string(
                REGEX REPLACE "/O[0-9dtx]"
                "/Od"
                CMAKE_C_FLAGS_${cfg}
                "${CMAKE_C_FLAGS_${cfg}}"
            )
            string(
                REGEX REPLACE "/O[0-9dtx]"
                "/Od"
                CMAKE_CXX_FLAGS_${cfg}
                "${CMAKE_CXX_FLAGS_${cfg}}"
            )
            # Also remove /Ob[0-9] (inline expansion control) that appears in RelWithDebInfo
            string(
                REGEX REPLACE "/Ob[0-9]"
                ""
                CMAKE_C_FLAGS_${cfg}
                "${CMAKE_C_FLAGS_${cfg}}"
            )
            string(
                REGEX REPLACE "/Ob[0-9]"
                ""
                CMAKE_CXX_FLAGS_${cfg}
                "${CMAKE_CXX_FLAGS_${cfg}}"
            )
        endforeach()
        message(
            NOTICE
            "Using /Od (no optimization) globally for ClangCL x64 to avoid access violations"
        )
    endif()
endif()

# Workaround for MSVC ARM64 miscompilation on Windows.
# MSVC ARM64 with /O2 miscompiles gzip-related code paths in the main application
# (io_utils.cpp and related), causing access violations (0xC0000005) when reading
# gzip-compressed input files. Vendor libraries (cloudflare_zlib, libbsc) already
# have their own per-directory /Od overrides; this global override extends the same
# protection to the main spring2 application code and any other targets in scope.
if(
    WIN32
    AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$"
)
    foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        string(
            REGEX REPLACE "/O[0-9dtx]"
            "/Od"
            CMAKE_C_FLAGS_${cfg}
            "${CMAKE_C_FLAGS_${cfg}}"
        )
        string(
            REGEX REPLACE "/O[0-9dtx]"
            "/Od"
            CMAKE_CXX_FLAGS_${cfg}
            "${CMAKE_CXX_FLAGS_${cfg}}"
        )
        string(
            REGEX REPLACE "/Ob[0-9]"
            ""
            CMAKE_C_FLAGS_${cfg}
            "${CMAKE_C_FLAGS_${cfg}}"
        )
        string(
            REGEX REPLACE "/Ob[0-9]"
            ""
            CMAKE_CXX_FLAGS_${cfg}
            "${CMAKE_CXX_FLAGS_${cfg}}"
        )
    endforeach()
    message(
        NOTICE
        "Using /Od (no optimization) globally for MSVC ARM64 to avoid access violations"
    )
endif()

add_vendor_subdirectory_no_install("${PTHASH_DIR}" "${CMAKE_BINARY_DIR}/pthash-build")

# =============================================================================
# Vendor subdirectories
# =============================================================================

# Zstd build configuration
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_compile_definitions(ZSTD_NO_TRACE)
add_vendor_subdirectory_no_install("${ZSTD_DIR}" "${CMAKE_BINARY_DIR}/zstd-build")
add_vendor_subdirectory_no_install("${LIBBSC_DIR}" "${CMAKE_BINARY_DIR}/libbsc-build")
if(TARGET libbsc AND SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(
        libbsc
        SYSTEM
        PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS}
    )
endif()
check_symbol_exists(wcrtomb wchar.h HAVE_WCRTOMB)
if(HAVE_WCRTOMB)
    set(HAVE_WCRTOMB 1 CACHE INTERNAL "Have wcrtomb" FORCE)
    message(
        NOTICE
        "Detected wcrtomb; enabling libarchive's preferred wide-character conversion path."
    )
endif()

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_vendor_subdirectory_no_install("${LIBDEFLATE_DIR}" "${CMAKE_BINARY_DIR}/libdeflate-build")

add_vendor_subdirectory_no_install("${CLOUDFLARE_ZLIB_DIR}" "${CMAKE_BINARY_DIR}/cloudflare_zlib-build")

set(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_USE_ZLIB_NG OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_ISAL ON CACHE BOOL "Build with ISA-L support")
add_vendor_subdirectory_no_install("${INDEXED_BZIP2_DIR}" "${CMAKE_BINARY_DIR}/indexed_bzip2-build" EXCLUDE_FROM_ALL)

# Standard archive support via vendored libarchive.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_vendor_subdirectory_no_install("${LIBARCHIVE_DIR}" "${CMAKE_BINARY_DIR}/libarchive-build" EXCLUDE_FROM_ALL)

# =============================================================================
# Reference data generation and source lists
# =============================================================================

# Reference embedding and shared source lists.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
set(REFERENCE_GENERATED ${CMAKE_BINARY_DIR}/generated/reference_data.cpp)

add_custom_command(
    OUTPUT ${REFERENCE_GENERATED}
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/codegen/embed_reference.py
        ${CMAKE_SOURCE_DIR}/tools/assay-reference/ref_hg38_gencode49.fa
        ${REFERENCE_GENERATED}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/tools/codegen/embed_reference.py
        ${CMAKE_SOURCE_DIR}/tools/assay-reference/ref_hg38_gencode49.fa
    COMMENT "Embedding reference data into binary"
)

set(core_source_files
    # Common
    ${source_dir}/common/bitset_dictionary.cpp
    ${source_dir}/common/bgzf.cpp
    ${source_dir}/common/dna_utils.cpp
    ${source_dir}/common/fs_utils.cpp
    ${source_dir}/common/io_utils.cpp
    ${source_dir}/common/legacy_id_codec.cpp
    ${source_dir}/common/params.cpp
    ${source_dir}/common/parse_utils.cpp
    ${source_dir}/common/progress.cpp
    # Assays
    ${source_dir}/assays/assay_atac.cpp
    ${source_dir}/assays/assay_detector.cpp
    ${source_dir}/assays/assay_bisulfite.cpp
    ${source_dir}/assays/assay_rna.cpp
    ${source_dir}/assays/assay_sc_atac.cpp
    ${source_dir}/assays/assay_sc_bisulfite.cpp
    ${source_dir}/assays/assay_sc_rna.cpp
    # Preprocess / reorder / encode
    ${source_dir}/preprocess/preprocess_artifacts.cpp
    ${source_dir}/preprocess/input_preparation.cpp
    ${source_dir}/encode/encoder.cpp
    ${source_dir}/encode/compression_dispatch_encoder.cpp
    ${source_dir}/reorder/compression_dispatch_reorder.cpp
    ${source_dir}/reorder/paired_end_mate_ordering.cpp
    ${source_dir}/reorder/quality_id_reordering.cpp
    ${source_dir}/reorder/stream_reordering.cpp
    # Decompression / archive IO
    ${source_dir}/decompress/archive_stream_reader.cpp
    ${source_dir}/decompress/archive_record_reconstruction.cpp
    ${source_dir}/decompress/decompress_archive_io.cpp
    # Workflow
    ${source_dir}/workflow/archive_preview.cpp
    ${source_dir}/workflow/compression_dispatch.cpp
    ${source_dir}/workflow/compression_workflow.cpp
    ${source_dir}/workflow/decompression_workflow.cpp
    ${source_dir}/workflow/spring_audit.cpp
    ${source_dir}/workflow/workflow_shared.cpp
    # Generated
    ${REFERENCE_GENERATED}
)

set(spring_src_include_dirs
    ${source_dir}
    ${source_dir}/assays
    ${source_dir}/common
    ${source_dir}/decompress
    ${source_dir}/encode
    ${source_dir}/preprocess
    ${source_dir}/reorder
    ${source_dir}/workflow
)

set(spring_app_include_dirs ${include_dir} ${spring_src_include_dirs})
set(spring_test_include_dirs
    ${include_dir}
    ${vendor_dir}
    ${CMAKE_SOURCE_DIR}/tests/support
    ${spring_src_include_dirs}
)

# =============================================================================
# Target helper functions
# =============================================================================

# Convenience targets for repository development checks.
function(add_dev_script_target target_name script_path)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${script_path}")
        add_custom_target(
            ${target_name}
            COMMAND ${CMAKE_SOURCE_DIR}/${script_path}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL
        )
    endif()
endfunction()

set(win_resources "")
if(WIN32)
    if(EXISTS "${CMAKE_SOURCE_DIR}/docs/assets/icons/logo.ico")
        set(win_resources "${CMAKE_CURRENT_BINARY_DIR}/generated/spring2.rc")
    else()
        message(
            NOTICE
            "logo.ico not found in docs/assets/icons, skipping Windows icon resource"
        )
    endif()
endif()

# =============================================================================
# Main application target
# =============================================================================

add_executable(
    spring2
    ${source_dir}/main.cpp
    ${core_source_files}
    ${win_resources}
)

# Shared target configuration helpers.
option(
    SPRING_ENABLE_PRECOMPILED_HEADERS
    "Use precompiled headers to speed up compilation"
    ON
)

set(spring_libs
    libdeflate::libdeflate_static
    libzstd_static
    ZLIB::ZLIB
    libbsc
    pthash
    rapidgzip::rapidgzip
    archive_static
)

function(spring_enable_precompiled_headers_if_requested target_name)
    if(SPRING_ENABLE_PRECOMPILED_HEADERS)
        target_precompile_headers(${target_name} PRIVATE ${source_dir}/pch.h)
    endif()
endfunction()

if(SPRING_ENABLE_PRECOMPILED_HEADERS)
    message(NOTICE "Precompiled headers enabled for faster rebuilds")
endif()

function(spring_apply_common_target_settings target_name)
    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE LIBARCHIVE_STATIC)
    endif()

    # Fail the build on any warning in this target's compilation units.
    # Warnings from third-party headers are only suppressed when those headers
    # are provided via SYSTEM include directories (e.g. vendored targets);
    # any non-SYSTEM third-party header included here is still diagnosed.
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Werror)
    endif()

    if(SPRING_OPENMP_INCLUDE_DIRS)
        target_include_directories(
            ${target_name}
            SYSTEM
            PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS}
        )
    endif()
endfunction()

function(spring_link_project_libraries target_name link_scope)
    target_link_libraries(${target_name} ${link_scope} ${spring_libs})
    if(TARGET OpenMP::OpenMP_CXX)
        target_link_libraries(${target_name} ${link_scope} OpenMP::OpenMP_CXX)
    endif()
endfunction()

function(spring_link_windows_system_libraries target_name)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE ws2_32 psapi)
    endif()
endfunction()

# compression_dispatch_encoder.cpp instantiates encoder_main<> for 24 bitset
# sizes; each instantiation now includes substantial inline disk-spill logic.
# On MSVC/ClangCL the resulting object file can exceed the default COFF section
# limit - raise it with /bigobj.
if(MSVC)
    set_source_files_properties(
        ${source_dir}/encode/compression_dispatch_encoder.cpp
        PROPERTIES COMPILE_OPTIONS "/bigobj"
    )
endif()

# Application target.
spring_enable_precompiled_headers_if_requested(spring2)

# Spring headers live in src/, while vendor/ exposes bundled libraries such
# as BBHash, libbsc, id_compression, and the vendored libdeflate sources.
target_include_directories(spring2 PRIVATE ${spring_app_include_dirs})
target_include_directories(spring2 SYSTEM PRIVATE ${LIBARCHIVE_DIR}/libarchive)
spring_apply_common_target_settings(spring2)
spring_link_project_libraries(spring2 PUBLIC)
spring_link_windows_system_libraries(spring2)

if(WIN32)
    target_compile_definitions(
        spring2
        PRIVATE IDOFILE_PATH_ROOT=".spring-idtmp/idoFile."
    )
endif()

# On Windows with MSVC-frontend compilers (MSVC and ClangCL), the default
# thread stack size is 1 MB. libsais.c (suffix array construction inside
# libbsc) is recursively implemented and the recursion depth is proportional
# to the entropy/complexity of the input. Real FASTQ data with varied
# sequences and Illumina-style IDs drives significantly deeper recursion than
# synthetic cycling test data. When /Od disables inlining, every call frame
# is full size, further increasing stack consumption. Exceeding the stack
# limit on Windows produces an access violation (0xC0000005) rather than
# STATUS_STACK_OVERFLOW (0xC00000FD) because the guard page triggers first.
# Raise the committed stack to 8 MB (8 * 1024 * 1024 = 8388608 bytes) to
# provide headroom for worst-case recursion depths.
if(WIN32 AND MSVC)
    target_link_options(spring2 PRIVATE /STACK:8388608)
endif()

# =============================================================================
# Windows runtime DLL bundling
# =============================================================================

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Base GCC runtime set. OpenMP runtime is additionally resolved below from
    # compiler/OpenMP metadata to support toolchain variations.
    set(SPRING_RUNTIME_DLLS
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
    )

    function(resolve_gcc_runtime_dll dll_name out_path)
        get_filename_component(_gcc_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        set(_candidate "${_gcc_bin_dir}/${dll_name}")
        if(EXISTS "${_candidate}")
            set(${out_path} "${_candidate}" PARENT_SCOPE)
            return()
        endif()

        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -print-file-name=${dll_name}
            OUTPUT_VARIABLE _print_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(
            _print_path
            AND NOT _print_path STREQUAL dll_name
            AND EXISTS "${_print_path}"
        )
            set(${out_path} "${_print_path}" PARENT_SCOPE)
            return()
        endif()

        set(${out_path} "" PARENT_SCOPE)
    endfunction()

    function(resolve_openmp_runtime_dll out_path)
        set(_openmp_candidates
            libgomp-1.dll
            libgomp.dll
            libomp.dll
            libomp-1.dll
        )

        if(DEFINED OpenMP_CXX_LIB_NAMES)
            foreach(_omp_name IN LISTS OpenMP_CXX_LIB_NAMES)
                list(APPEND _openmp_candidates "lib${_omp_name}-1.dll")
                list(APPEND _openmp_candidates "lib${_omp_name}.dll")
            endforeach()
        endif()
        list(REMOVE_DUPLICATES _openmp_candidates)

        foreach(_candidate IN LISTS _openmp_candidates)
            resolve_gcc_runtime_dll("${_candidate}" _resolved)
            if(_resolved)
                set(${out_path} "${_resolved}" PARENT_SCOPE)
                return()
            endif()
        endforeach()

        set(${out_path} "" PARENT_SCOPE)
    endfunction()

    set(SPRING_RUNTIME_DLL_PATHS)
    foreach(_dll IN LISTS SPRING_RUNTIME_DLLS)
        resolve_gcc_runtime_dll("${_dll}" _resolved_dll_path)
        if(_resolved_dll_path)
            list(APPEND SPRING_RUNTIME_DLL_PATHS "${_resolved_dll_path}")
        else()
            message(NOTICE "SPRING: runtime DLL ${_dll} not found for bundling")
        endif()
    endforeach()

    resolve_openmp_runtime_dll(_resolved_openmp_runtime)
    if(_resolved_openmp_runtime)
        list(APPEND SPRING_RUNTIME_DLL_PATHS "${_resolved_openmp_runtime}")
    else()
        message(NOTICE "SPRING: OpenMP runtime DLL not found for bundling")
    endif()
    list(REMOVE_DUPLICATES SPRING_RUNTIME_DLL_PATHS)

    if(SPRING_STATIC_RUNTIMES)
        message(NOTICE "SPRING: linking libgcc/libstdc++/libgomp statically")
        # Prefer modern CMake link options when available
        if(COMMAND target_link_options)
            target_link_options(spring2 PRIVATE -static)
        else()
            set_property(
                TARGET spring2
                APPEND_STRING
                PROPERTY LINK_FLAGS " -static"
            )
        endif()
    else()
        # Convenience target to copy commonly required GCC runtime DLLs for
        # all distributed Windows binaries.
        add_custom_target(
            copy_runtime_dlls
            ALL
            COMMENT "Copying GCC runtime DLLs next to spring2 executable"
        )
        add_dependencies(copy_runtime_dlls spring2)
        foreach(_src IN LISTS SPRING_RUNTIME_DLL_PATHS)
            get_filename_component(_dll "${_src}" NAME)
            if(EXISTS "${_src}")
                add_custom_command(
                    TARGET copy_runtime_dlls
                    POST_BUILD
                    COMMAND
                        ${CMAKE_COMMAND} -E copy_if_different "${_src}"
                        "$<TARGET_FILE_DIR:spring2>/${_dll}"
                )
            endif()
        endforeach()
    endif()
endif()

# =============================================================================
# Installation
# =============================================================================

# Installation logic for a clean release footprint
if(APPLE)
    set(SPRING_INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX AND NOT WIN32)
    set(SPRING_INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(DEFINED SPRING_INSTALL_RPATH)
    foreach(_target IN ITEMS spring2)
        set_target_properties(
            ${_target}
            PROPERTIES
                BUILD_WITH_INSTALL_RPATH OFF
                INSTALL_RPATH "${SPRING_INSTALL_RPATH}"
        )
    endforeach()
endif()

function(resolve_unix_runtime_library out_path)
    foreach(_candidate IN LISTS ARGN)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_candidate}
            OUTPUT_VARIABLE _resolved
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(
            _resolved
            AND NOT _resolved STREQUAL _candidate
            AND EXISTS "${_resolved}"
        )
            set(${out_path} "${_resolved}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_path} "" PARENT_SCOPE)
endfunction()

set(SPRING_OPENMP_RUNTIME_LIB "")
if(APPLE)
    if(DEFINED OpenMP_omp_LIBRARY AND EXISTS "${OpenMP_omp_LIBRARY}")
        set(SPRING_OPENMP_RUNTIME_LIB "${OpenMP_omp_LIBRARY}")
    endif()
    if(NOT SPRING_OPENMP_RUNTIME_LIB AND DEFINED OpenMP_CXX_LIB_NAMES)
        foreach(_omp_name IN LISTS OpenMP_CXX_LIB_NAMES)
            resolve_unix_runtime_library(_resolved_omp_dylib
                "lib${_omp_name}.dylib"
                "${_omp_name}.dylib"
            )
            if(_resolved_omp_dylib)
                set(SPRING_OPENMP_RUNTIME_LIB "${_resolved_omp_dylib}")
                break()
            endif()
        endforeach()
    endif()
elseif(UNIX)
    # GNU toolchains generally use libgomp; Clang toolchains may use libomp.
    resolve_unix_runtime_library(SPRING_OPENMP_RUNTIME_LIB
        "libgomp.so.1"
        "libgomp.so"
        "libomp.so.1"
        "libomp.so"
    )
endif()

set(INSTALL_TARGETS spring2)
install(
    TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if((APPLE OR (UNIX AND NOT WIN32)) AND SPRING_OPENMP_RUNTIME_LIB)
    install(
        FILES "${SPRING_OPENMP_RUNTIME_LIB}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
elseif(APPLE OR (UNIX AND NOT WIN32))
    message(
        NOTICE
        "SPRING: OpenMP runtime library not found for install bundling"
    )
endif()

if(WIN32 AND NOT SPRING_STATIC_RUNTIMES)
    foreach(_src IN LISTS SPRING_RUNTIME_DLL_PATHS)
        if(EXISTS "${_src}")
            install(FILES "${_src}" DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endforeach()
endif()

# =============================================================================
# Tests
# =============================================================================

# Tests.
enable_testing()
set(unit_test_sources
    tests/unit/unit_tests.cpp
    tests/unit/dna_parse_unit_tests.cpp
    tests/unit/assay_detector_unit_tests.cpp
    tests/unit/assay_detector_real_data_unit_tests.cpp
    tests/unit/compression_storage_plan_unit_tests.cpp
    tests/unit/archive_metadata_version_unit_tests.cpp
    tests/unit/gzip_preprocess_progress_unit_tests.cpp
    tests/unit/disk_path_memory_reduction_unit_tests.cpp
    tests/unit/reorder_performance_unit_tests.cpp
)

add_executable(unit-tests ${unit_test_sources} ${core_source_files})
spring_enable_precompiled_headers_if_requested(unit-tests)
target_include_directories(unit-tests PRIVATE ${spring_test_include_dirs})
spring_apply_common_target_settings(unit-tests)
spring_link_project_libraries(unit-tests PRIVATE)
spring_link_windows_system_libraries(unit-tests)

target_compile_definitions(
    unit-tests
    PRIVATE
        DOCTEST_CONFIG_USE_STD_HEADERS
        UNIT_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/tests/data"
        UNIT_TEST_REFERENCE_DIR="${CMAKE_SOURCE_DIR}/tools/assay-reference"
)
add_test(NAME unit_tests COMMAND unit-tests)

set(integration_test_sources
    tests/integration/integration_tests.cpp
    tests/integration/integration_test_support.cpp
    tests/integration/archive_integration_tests.cpp
    tests/integration/reader_integration_tests.cpp
    tests/integration/storage_path_integration_tests.cpp
    tests/integration/workflow_metadata_integration_tests.cpp
    tests/integration/assay_integration_tests.cpp
    tests/integration/grouped_archive_integration_tests.cpp
)
add_executable(
    integration-tests
    ${integration_test_sources}
    ${core_source_files}
)
spring_enable_precompiled_headers_if_requested(integration-tests)
target_include_directories(
    integration-tests
    PRIVATE ${spring_test_include_dirs}
)
spring_apply_common_target_settings(integration-tests)
spring_link_project_libraries(integration-tests PRIVATE)
spring_link_windows_system_libraries(integration-tests)

target_compile_definitions(
    integration-tests
    PRIVATE
        DOCTEST_CONFIG_USE_STD_HEADERS
        SPRING2_EXECUTABLE=\"$<TARGET_FILE:spring2>\"
        INTEGRATION_TEST_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/tests/data\"
)
add_test(NAME integration_tests COMMAND integration-tests)

set(smoke_test_sources
    tests/smoke/smoke_tests.cpp
    tests/smoke/smoke_test_support.cpp
    tests/smoke/basic_roundtrip_smoke_tests.cpp
    tests/smoke/grouped_roundtrip_smoke_tests.cpp
    tests/smoke/gzip_roundtrip_smoke_tests.cpp
    tests/smoke/cli_mode_smoke_tests.cpp
)
add_executable(smoke-tests ${smoke_test_sources})
add_dependencies(smoke-tests spring2)
target_include_directories(smoke-tests PRIVATE ${spring_test_include_dirs})
spring_apply_common_target_settings(smoke-tests)
spring_link_project_libraries(smoke-tests PRIVATE)
spring_link_windows_system_libraries(smoke-tests)

target_compile_definitions(
    smoke-tests
    PRIVATE
        DOCTEST_CONFIG_USE_STD_HEADERS
        SPRING2_EXECUTABLE="$<TARGET_FILE:spring2>"
        SMOKE_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/tests/data"
        SMOKE_TEST_OUTPUT_ROOT="${CMAKE_SOURCE_DIR}/out/tests/smoke"
)
add_test(NAME smoke_tests COMMAND smoke-tests)
