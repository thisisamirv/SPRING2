cmake_minimum_required(VERSION 3.17)
project("Indexed BZip2/Gzip Decoder" CXX)
option(
    LIBRAPIDARCHIVE_WITH_ISAL
    "Compile with ISA-L for faster decompression"
    ON
)
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(LIBRAPIDARCHIVE_WITH_ISAL OFF)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()
add_compile_options(
    "$<$<OR:$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>,$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>>:-Wall;-Wextra;-Wshadow;-Werror=return-type;-Wno-unknown-attributes>"
    "$<$<OR:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<COMPILE_LANG_AND_ID:C,GNU>>:-Wall;-Wextra;-Wshadow;-Wunused;-Werror=return-type;-Wno-attributes>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wsuggest-override>"
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fstack-protector-strong>"
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<NOT:$<PLATFORM_ID:Windows>>>:-fstack-clash-protection>"
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/bigobj>"
)
if(
    NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm"
    AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch"
    AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "riscv64"
)
    add_compile_options(
        "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fcf-protection=full>"
    )
endif()
if(
    NOT WIN32
    AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm"
    AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch"
)
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fpie>")
    add_link_options(
        "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<PLATFORM_ID:Linux>>:-Wl,-pie,-z,relro,-z,now,-z,defs>"
    )
endif()
find_package(Threads REQUIRED)
add_library(zlibstatic INTERFACE)
target_link_libraries(zlibstatic INTERFACE ZLIB::ZLIB)
if(LIBRAPIDARCHIVE_WITH_ISAL)
    include(FetchContent)
    FetchContent_Declare(
        isal_project
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/isa-l
    )
    set(ISAL_BUILD_SHARED_LIBS OFF CACHE BOOL "Build static ISA-L" FORCE)
    FetchContent_MakeAvailable(isal_project)
    add_library(ISAL::isal ALIAS isal)
endif()
add_library(filereader INTERFACE)
target_include_directories(filereader INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    filereader
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/BitReader.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Buffered.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BufferView.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FileReader.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Shared.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SinglePass.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Standard.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StreamAdapter.hpp
)
target_compile_features(filereader INTERFACE cxx_std_17)
add_library(core INTERFACE)
target_include_directories(core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    core
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/AffinityHelpers.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AlignedAllocator.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AtomicMutex.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BitManipulation.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BitStringFinder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BlockFetcher.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BlockFinder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BlockFinderInterface.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/BlockMap.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Cache.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/common.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Error.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FasterVector.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FileRanges.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FileUtils.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/JoiningThread.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ParallelBitStringFinder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Prefetcher.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Statistics.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StreamedResults.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ThreadPool.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/VectorView.hpp
)
target_link_libraries(core INTERFACE Threads::Threads filereader)
if(
    WIN32
    AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    AND NOT (
        MSVC
        OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
        OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
    )
)
    target_compile_options(core INTERFACE -fconstexpr-steps=16777216)
endif()
target_compile_features(core INTERFACE cxx_std_17)
add_library(libhuffman INTERFACE)
target_include_directories(libhuffman INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    libhuffman
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingBase.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingLinearSearch.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingShortBitsCached.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingSymbolsPerLength.hpp
)
target_link_libraries(libhuffman INTERFACE core)
target_compile_features(libhuffman INTERFACE cxx_std_17)
add_library(libhuffmangzip INTERFACE)
add_library(rapidgzip::gzip::huffman ALIAS libhuffmangzip)
target_include_directories(libhuffmangzip INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    libhuffmangzip
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingDistanceISAL.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingDoubleLiteralCached.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingISAL.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingReversedBitsCached.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingReversedBitsCachedCompressed.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingReversedBitsCachedSeparateLengths.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingReversedCodesPerLength.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingShortBitsCachedDeflate.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/HuffmanCodingShortBitsMultiCached.hpp
)
target_link_libraries(libhuffmangzip INTERFACE core libhuffman)
target_compile_features(libhuffmangzip INTERFACE cxx_std_17)
add_library(librapidgzipgzip INTERFACE)
add_library(rapidgzip::gzip ALIAS librapidgzipgzip)
target_include_directories(
    librapidgzipgzip
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_sources(
    librapidgzipgzip
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/crc32.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/definitions.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/deflate.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/format.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gzip.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/GzipReader.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InflateWrapper.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/isal.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/precode.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/RFCTables.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/zlib.hpp
)
target_link_libraries(
    librapidgzipgzip
    INTERFACE core rapidgzip::gzip::huffman zlibstatic
)
if(LIBRAPIDARCHIVE_WITH_ISAL)
    target_link_libraries(librapidgzipgzip INTERFACE ISAL::isal)
endif()
target_compile_features(librapidgzipgzip INTERFACE cxx_std_17)
add_library(libblockfinder INTERFACE)
add_library(rapidgzip::libblockfinder ALIAS libblockfinder)
target_include_directories(libblockfinder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    libblockfinder
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Bgzf.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DynamicHuffman.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Interface.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/PigzStringView.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CountAllocatedLeaves.hpp
)
target_link_libraries(libblockfinder INTERFACE core)
target_compile_features(libblockfinder INTERFACE cxx_std_17)
add_library(libchunkdecoding INTERFACE)
add_library(rapidgzip::chunkdecoding ALIAS libchunkdecoding)
target_include_directories(
    libchunkdecoding
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_sources(
    libchunkdecoding
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/GzipChunk.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DecompressionError.hpp
)
target_link_libraries(
    libchunkdecoding
    INTERFACE core rapidgzip::gzip rapidgzip::libblockfinder
)
target_compile_features(libchunkdecoding INTERFACE cxx_std_17)
add_library(librapidgzip INTERFACE)
add_library(rapidgzip::rapidgzip ALIAS librapidgzip)
target_include_directories(librapidgzip INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
    librapidgzip
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ChunkData.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CompressedVector.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DecodedData.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DecodedDataView.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/GzipBlockFinder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/GzipChunkFetcher.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IndexFileFormat.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MarkerReplacement.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ParallelGzipReader.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rapidgzip.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/WindowMap.hpp
)
target_link_libraries(librapidgzip INTERFACE core rapidgzip::chunkdecoding)
target_compile_features(librapidgzip INTERFACE cxx_std_17)
if(LIBRAPIDARCHIVE_WITH_ISAL)
    target_compile_definitions(librapidgzip INTERFACE LIBRAPIDARCHIVE_WITH_ISAL)
endif()
