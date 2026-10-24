# Included from the top-level CMakeLists.txt.

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

set(LIBRAPIDARCHIVE_WITH_ISAL ON CACHE BOOL "Build with ISA-L support")
add_vendor_subdirectory_no_install("${INDEXED_BZIP2_DIR}" "${CMAKE_BINARY_DIR}/indexed_bzip2-build" EXCLUDE_FROM_ALL)

# Standard archive support via vendored libarchive.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_vendor_subdirectory_no_install("${LIBARCHIVE_DIR}" "${CMAKE_BINARY_DIR}/libarchive-build" EXCLUDE_FROM_ALL)
