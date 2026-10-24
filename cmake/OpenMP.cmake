# Included from the top-level CMakeLists.txt.

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
elseif(
    WIN32
    AND (
        CMAKE_C_COMPILER_ID STREQUAL "GNU"
         OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    )
)
    # CMake FindOpenMP on MinGW/WinLibs often fails the C language probe even
    # when -fopenmp works for both C and CXX. Require CXX, then reuse for C.
    message(
        NOTICE
        "Configuring OpenMP for Windows GCC via CXX (reuse settings for C)"
    )
    set(SPRING_OPENMP_COMPONENTS CXX)
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
        set(OpenMP_C_FOUND TRUE)
        set(OpenMP_C_FLAGS "${OpenMP_CXX_FLAGS}")
        set(OpenMP_C_LIB_NAMES "${OpenMP_CXX_LIB_NAMES}")
        set(OpenMP_C_LIBRARIES "${OpenMP_CXX_LIBRARIES}")
        set(OpenMP_C_INCLUDE_DIRS "${OpenMP_CXX_INCLUDE_DIRS}")
        if(NOT TARGET OpenMP::OpenMP_C AND TARGET OpenMP::OpenMP_CXX)
            add_library(OpenMP::OpenMP_C INTERFACE IMPORTED)
            set_target_properties(
                OpenMP::OpenMP_C
                PROPERTIES INTERFACE_LINK_LIBRARIES OpenMP::OpenMP_CXX
            )
        endif()
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
