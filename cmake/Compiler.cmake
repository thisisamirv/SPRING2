# Included from the top-level CMakeLists.txt.

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
