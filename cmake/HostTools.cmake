# Included from the top-level CMakeLists.txt.

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
