# cmake-format: off
# Copyright (c) 2025, Intel Corporation
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Intel Corporation nor the names of its contributors
#       may be used to endorse or promote products derived from this software
#       without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# cmake-format: on
cmake_minimum_required(VERSION 3.12)
cmake_policy(VERSION 3.12)
if(POLICY CMP194)
    cmake_policy(SET CMP194 NEW)
endif()
project(
    ISA-L
    VERSION 2.31.0
    DESCRIPTION "Intel's ISA-L (Intelligent Storage Acceleration Library)"
    LANGUAGES C
)
if(
    CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
    OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64"
)
    if(DEFINED SPRING_HAVE_NASM AND SPRING_HAVE_NASM)
        if(DEFINED SPRING_NASM_EXECUTABLE AND SPRING_NASM_EXECUTABLE)
            set(CMAKE_ASM_NASM_COMPILER
                "${SPRING_NASM_EXECUTABLE}"
                CACHE FILEPATH
                "NASM assembler"
                FORCE
            )
            enable_language(ASM_NASM)
        endif()
        set(USE_NASM ON)
    else()
        set(USE_NASM OFF)
    endif()
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(
    CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
    OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64"
)
    set(CPU_X86_64 ON)
    set(ARCH_DEF "x86_64")
elseif(
    CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"
    OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
)
    set(CPU_AARCH64 ON)
    set(ARCH_DEF "aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "ppc64le")
    set(CPU_PPC64LE ON)
    set(ARCH_DEF "ppc64le")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    set(CPU_RISCV64 ON)
    set(ARCH_DEF "riscv64")
else()
    set(CPU_UNDEFINED ON)
endif()
if(CPU_X86_64)
    if(DEFINED USE_NASM AND USE_NASM)
        if(WIN32)
            set(CMAKE_ASM_NASM_FLAGS "-f win64 -DWIN32")
        else()
            set(CMAKE_ASM_NASM_FLAGS "-f elf64 -D LINUX")
        endif()
        set(CMAKE_ASM_NASM_INCLUDES "-I ${CMAKE_CURRENT_SOURCE_DIR}/include/")
    endif()
elseif(CPU_AARCH64 OR CPU_RISCV64)
    set(ASM_FILTER "${CMAKE_C_COMPILER} -D__ASSEMBLY__")
endif()
set(ISAL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(EXTERN_HEADERS)
if(ARCH_DEF)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D${ARCH_DEF}")
endif()
if(CPU_AARCH64 OR CPU_RISCV64)
    set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -D__ASSEMBLY__")
endif()
set(LIBISAL_VERSION_MAJOR 2)
set(LIBISAL_VERSION_MINOR 31)
set(LIBISAL_VERSION_PATCH 0)
include(cmake/crc.cmake)
include(cmake/igzip.cmake)
option(ISAL_BUILD_SHARED_LIBS "Build shared libraries" ON)
set(BUILD_SHARED_LIBS ${ISAL_BUILD_SHARED_LIBS})
add_library(
    isal
    ${ERASURE_CODE_SOURCES}
    ${RAID_SOURCES}
    ${CRC_SOURCES}
    ${IGZIP_SOURCES}
    ${MEM_SOURCES}
)
set_target_properties(
    isal
    PROPERTIES
        VERSION
            ${LIBISAL_VERSION_MAJOR}.${LIBISAL_VERSION_MINOR}.${LIBISAL_VERSION_PATCH}
        SOVERSION ${LIBISAL_VERSION_MAJOR}
        PUBLIC_HEADER "${EXTERN_HEADERS}"
)
if(
    WIN32
    AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
    AND (
        MSVC
        OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
        OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"
    )
)
    target_compile_options(
        isal
        PRIVATE
            -Wno-cast-align
            -Wno-cast-qual
            -Wno-conditional-uninitialized
            -Wno-declaration-after-statement
            -Wno-implicit-fallthrough
            -Wno-implicit-int-conversion
            -Wno-microsoft-enum-value
            -Wno-missing-prototypes
            -Wno-missing-variable-declarations
            -Wno-padded
            -Wno-reserved-identifier
            -Wno-reserved-macro-identifier
            -Wno-shorten-64-to-32
            -Wno-sign-conversion
            -Wno-switch-default
            -Wno-unreachable-code
            -Wno-unsafe-buffer-usage
            -Wno-c++-keyword
            -Wno-unused-macros
    )
endif()
if(CPU_X86_64 AND USE_NASM)
    foreach(
        source
        IN
        LISTS
            ERASURE_CODE_SOURCES
            RAID_SOURCES
            CRC_SOURCES
            IGZIP_SOURCES
            MEM_SOURCES
    )
        if(source MATCHES "\\.asm$")
            get_filename_component(source_dir ${source} DIRECTORY)
            set_source_files_properties(
                ${source}
                PROPERTIES
                    INCLUDE_DIRECTORIES
                        "${CMAKE_CURRENT_SOURCE_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}"
            )
        endif()
    endforeach()
endif()
target_include_directories(
    isal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)
set(ISAL_HEADER "${CMAKE_BINARY_DIR}/isa-l.h")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/isa-l.h.in
    ${ISAL_HEADER}
    @ONLY
)
