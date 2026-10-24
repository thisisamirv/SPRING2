# Included from the top-level CMakeLists.txt.

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
