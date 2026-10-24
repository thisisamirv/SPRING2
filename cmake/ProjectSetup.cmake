# Included from the top-level CMakeLists.txt.

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
