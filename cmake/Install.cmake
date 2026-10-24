# Included from the top-level CMakeLists.txt.

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
