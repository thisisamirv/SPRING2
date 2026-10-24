# Included from the top-level CMakeLists.txt.

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
