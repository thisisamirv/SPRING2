#!/usr/bin/env python3

"""Cross-platform lint driver for the SPRING2 repository."""

from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable

ROOT_DIR = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_BUILD_DIR = ROOT_DIR / "out" / "build"
BUILD_DIR = DEFAULT_BUILD_DIR  # Will be updated by parse_args
BUILD_COMPILE_COMMANDS = BUILD_DIR / "compile_commands.json"  # Will be updated
WORKSPACE_COMPILE_COMMANDS = ROOT_DIR / "out" / "clangd" / "compile_commands.json"
DEFAULT_CPP_ROOTS = (ROOT_DIR / "src", ROOT_DIR / "vendor")
SUMMARY_LINE_PATTERN = re.compile(
    r"^[0-9]+ warnings? generated\.$|^[0-9]+ warnings? and [0-9]+ errors? generated\.$"
)
PROCESSING_LINE_PATTERN = re.compile(r"^\[\d+/\d+\] \(\d+/\d+\) Processing file .+\.")

MSYSTEM = os.environ.get("MSYSTEM", "")
OS_UNAME = getattr(os, "uname", None)
UNAME = OS_UNAME().sysname if OS_UNAME is not None else ""
IS_MSYS_WINDOWS = bool(MSYSTEM) or UNAME.startswith(("MSYS_", "MINGW", "CYGWIN"))
IS_MACOS = sys.platform == "darwin"
IS_WINDOWS = os.name == "nt" or IS_MSYS_WINDOWS

LINT_INCLUDE_DIR = ROOT_DIR / "tools" / "dev" / "lint" / "include"
ZSTD_INCLUDE_DIR = ROOT_DIR / "vendor" / "zstd"
LIBBSC_INCLUDE_DIR = ROOT_DIR / "vendor" / "libbsc"
LIBDEFLATE_INCLUDE_DIR = ROOT_DIR / "vendor" / "libdeflate"
LIBARCHIVE_INCLUDE_DIR = ROOT_DIR / "vendor" / "libarchive" / "lib"
ZLIB_INCLUDE_DIR = ROOT_DIR / "vendor" / "cloudflare_zlib"
BZIP2_INCLUDE_DIR = ROOT_DIR / "vendor" / "indexed_bzip2"
BZIP2_ISAL_INCLUDE_DIR = ROOT_DIR / "vendor" / "indexed_bzip2" / "isa-l" / "include"
PTHASH_INCLUDE_DIR = ROOT_DIR / "vendor" / "pthash"
EXTRA_INCLUDES = (
    ROOT_DIR / "src",
    ROOT_DIR / "src" / "common",
    ROOT_DIR / "src" / "assays",
    ROOT_DIR / "src" / "decompress",
    ROOT_DIR / "src" / "encode",
    ROOT_DIR / "src" / "preprocess",
    ROOT_DIR / "src" / "reorder",
    ROOT_DIR / "src" / "workflow",
    ROOT_DIR / "vendor",
    ZSTD_INCLUDE_DIR,
    LIBBSC_INCLUDE_DIR,
    LIBDEFLATE_INCLUDE_DIR,
    LIBARCHIVE_INCLUDE_DIR,
    ZLIB_INCLUDE_DIR,
    BZIP2_INCLUDE_DIR,
    BZIP2_ISAL_INCLUDE_DIR,
    PTHASH_INCLUDE_DIR,
)
TIDY_CHECKS = (
    "*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*"
    ",-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers"
    ",-misc-const-correctness,-readability-identifier-length"
    ",-bugprone-empty-catch,-misc-include-cleaner"
    ",-modernize-use-trailing-return-type"
    ",-cppcoreguidelines-pro-bounds-pointer-arithmetic"
    ",-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access"
    ",-misc-use-internal-linkage,-readability-isolate-declaration"
    ",-readability-math-missing-parentheses"
    ",-modernize-return-braced-init-list,-concurrency-mt-unsafe"
    ",-misc-non-private-member-variables-in-classes"
    ",-bugprone-random-generator-seed,-bugprone-narrowing-conversions"
    ",-cppcoreguidelines-narrowing-conversions"
    ",-hicpp-explicit-conversions,-hicpp-named-parameter"
    ",-readability-named-parameter,-performance-avoid-endl"
    ",-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum"
    ",-modernize-macro-to-enum"
    ",-readability-use-concise-preprocessor-directives"
    ",-modernize-use-using,-modernize-avoid-c-style-cast"
    ",-cppcoreguidelines-pro-type-cstyle-cast"
    ",-cppcoreguidelines-pro-type-reinterpret-cast"
    ",-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc"
    ",-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays"
    ",-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays"
    ",-cppcoreguidelines-pro-bounds-constant-array-index"
    ",-cppcoreguidelines-pro-type-vararg,-hicpp-vararg"
    ",-hicpp-signed-bitwise,-cppcoreguidelines-init-variables"
    ",-openmp-use-default-none"
    ",-readability-function-cognitive-complexity"
    ",-bugprone-easily-swappable-parameters,-modernize-loop-convert"
    ",-bugprone-too-small-loop-variable"
    ",-readability-static-accessed-through-instance"
    ",-readability-use-std-min-max,-readability-container-data-pointer"
    ",-readability-make-member-function-const"
    ",-hicpp-braces-around-statements,-readability-braces-around-statements"
    ",-readability-inconsistent-ifelse-braces"
    ",-portability-template-virtual-member-function,-hicpp-use-auto"
    ",-modernize-use-auto,-readability-redundant-control-flow"
    ",-performance-unnecessary-copy-initialization,-hicpp-use-nullptr"
    ",-modernize-use-nullptr,-readability-implicit-bool-conversion"
    ",-readability-non-const-parameter,-readability-else-after-return"
    ",-cppcoreguidelines-pro-type-member-init,-hicpp-member-init"
    ",-cppcoreguidelines-pro-bounds-array-to-pointer-decay"
    ",-hicpp-no-array-decay,-android-cloexec-fopen"
    ",-openmp-exception-escape,-abseil-string-find-str-contains"
    ",-cppcoreguidelines-avoid-non-const-global-variables"
    ",-bugprone-exception-escape,-bugprone-signal-handler"
    ",-performance-inefficient-string-concatenation"
    ",-bugprone-branch-clone,-bugprone-switch-missing-default-case"
    ",-bugprone-command-processor,-misc-predictable-rand"
    ",-hicpp-uppercase-literal-suffix,-readability-container-size-empty"
    ",-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound"
    ",-readability-simplify-boolean-expr"
    ",-clang-analyzer-cplusplus.NewDeleteLeaks"
    ",-misc-use-anonymous-namespace"
    ",-cppcoreguidelines-pro-type-union-access"
    ",-bugprone-macro-parentheses"
    ",-bugprone-implicit-widening-of-multiplication-result"
    ",-readability-avoid-nested-conditional-operator"
    ",-hicpp-deprecated-headers,-modernize-deprecated-headers"
    ",-misc-redundant-expression,-cppcoreguidelines-avoid-goto"
    ",-hicpp-avoid-goto,-modernize-redundant-void-arg"
    ",-readability-redundant-casting"
    ",-readability-inconsistent-declaration-parameter-name"
    ",-clang-analyzer-core.BitwiseShift,-bugprone-casting-through-void"
    ",-cppcoreguidelines-use-enum-class,-performance-enum-size"
    ",-readability-uppercase-literal-suffix"
    ",-readability-redundant-parentheses"
    ",-bugprone-assignment-in-if-condition,-modernize-use-bool-literals"
    ",-bugprone-inc-dec-in-conditions,-clang-analyzer-core.NullPointerArithm"
    ",-hicpp-function-size,-readability-function-size"
    ",-clang-analyzer-deadcode.DeadStores"
    ",-clang-analyzer-core.uninitialized.Assign"
    ",-bugprone-reserved-identifier,-performance-no-int-to-ptr"
    ",-bugprone-suspicious-string-compare,-hicpp-multiway-paths-covered"
    ",-readability-redundant-member-init,-readability-container-contains"
    ",-misc-no-recursion,-readability-duplicate-include"
    ",-bugprone-signed-char-misuse,-modernize-avoid-variadic-functions"
    ",-bugprone-multi-level-implicit-pointer-conversion"
    ",-readability-avoid-unconditional-preprocessor-if"
    ",-clang-analyzer-core.UndefinedBinaryOperatorResult"
    ",-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling"
    ",-clang-analyzer-security.insecureAPI.strcpy"
    ",-readability-redundant-preprocessor,-misc-confusable-identifiers"
    ",-modernize-use-designated-initializers,-portability-simd-intrinsics"
    ",-modernize-use-integer-sign-comparison,-misc-unused-parameters"
    ",-readability-suspicious-call-argument,-hicpp-no-assembler"
    ",-readability-avoid-return-with-void-value,-clang-analyzer-unix.Stream"
    ",-modernize-concat-nested-namespaces"
    ",-readability-convert-member-functions-to-static"
    ",-bugprone-unchecked-string-to-number-conversion"
    ",-performance-inefficient-vector-operation"
    ",-clang-analyzer-core.NullDereference,-portability-avoid-pragma-once"
    ",-cppcoreguidelines-non-private-member-variables-in-classes"
    ",-cppcoreguidelines-special-member-functions,-hicpp-special-member-functions"
    ",-readability-redundant-string-init,-modernize-use-nodiscard"
    ",-cppcoreguidelines-use-default-member-init"
    ",-modernize-use-default-member-init,-readability-const-return-type"
    ",-cppcoreguidelines-avoid-const-or-ref-data-members"
    ",-performance-unnecessary-value-param"
    ",-misc-anonymous-namespace-in-header,-readability-qualified-auto"
    ",-hicpp-use-emplace,-modernize-use-emplace,-boost-use-ranges"
    ",-modernize-use-ranges,-readability-avoid-const-params-in-decls"
    ",-readability-redundant-access-specifiers"
    ",-readability-redundant-typename"
    ",-bugprone-throwing-static-initialization"
    ",-cppcoreguidelines-pro-type-const-cast"
    ",-bugprone-unintended-char-ostream-output"
    ",-modernize-use-constraints,-misc-override-with-different-visibility"
    ",-bugprone-derived-method-shadowing-base-method"
    ",-bugprone-unchecked-optional-access"
    ",-cppcoreguidelines-rvalue-reference-param-not-moved"
    ",-clang-analyzer-optin.portability.UnixAPI"
    ",-bugprone-suspicious-stringview-data-usage"
    ",-clang-analyzer-unix.StdCLibraryFunctions"
    ",-hicpp-exception-baseclass,-misc-throw-by-value-catch-by-reference"
    ",-bugprone-string-literal-with-embedded-nul"
    ",-modernize-use-scoped-lock"
    ",-cppcoreguidelines-misleading-capture-default-by-value"
    ",-cppcoreguidelines-pro-type-static-cast-downcast,-android-cloexec-dup"
    ",-misc-multiple-inheritance,-readability-use-anyofallof"
    ",-modernize-type-traits,-cppcoreguidelines-missing-std-forward"
    ",-cppcoreguidelines-explicit-virtual-functions,-hicpp-use-override"
    ",-modernize-use-override,-modernize-use-std-numbers"
    ",-clang-analyzer-valist.Uninitialized"
    ",-bugprone-non-zero-enum-to-bool-conversion"
    ",-bugprone-sizeof-expression,-bugprone-not-null-terminated-result"
    # MSVC STL _BITMASK_OPS macro combines valid enum flags via bitwise OR,
    # producing combined values not listed as named enumerators.  This is a
    # known false positive with MSVC's <filesystem> / <xfilesystem_abi.h>.
    ",-clang-analyzer-optin.core.EnumCastOutOfRange"
)


@dataclass
class CommandResult:
    """Captures a subprocess label, exit status, and filtered output."""

    label: str
    exit_code: int
    output: list[str]


def print_error(message: str) -> None:
    """Write an error message to stderr."""

    print(message, file=sys.stderr)


def require_path(path: pathlib.Path, explanation: str) -> None:
    """Fail fast when an expected file or directory is missing."""

    if not path.exists():
        print_error(explanation)
        raise SystemExit(1)


def resolve_parallel_job_count() -> int:
    """Determine the number of concurrent lint jobs to use."""

    requested_jobs = os.environ.get("SPRING_LINT_JOBS", "")
    if requested_jobs:
        if requested_jobs.isdigit() and int(requested_jobs) > 0:
            return int(requested_jobs)
        print_error("SPRING_LINT_JOBS must be a positive integer")
        raise SystemExit(1)

    cpu_count = os.cpu_count() or 1
    if cpu_count > 1:
        return cpu_count - 1
    return 1


def find_required_command(candidates: Iterable[str], description: str) -> str:
    """Return the first available executable from a candidate list."""

    for candidate in candidates:
        resolved = shutil.which(candidate)
        if resolved:
            return candidate
    print_error(f"Missing required command: {description}")
    raise SystemExit(1)


def normalize_compile_db_path(path: pathlib.Path | str) -> str:
    """Normalize a path for compile database membership checks."""

    normalized = pathlib.Path(path).resolve().as_posix()
    if IS_WINDOWS:
        return normalized.lower()
    return normalized


def is_cpp_source(path: pathlib.Path) -> bool:
    """Return whether a path is a supported native C or C++ source file."""

    return path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}


def is_python_source(path: pathlib.Path) -> bool:
    """Return whether a path is a Python source file."""

    return path.suffix.lower() == ".py"


def resolve_repo_path(raw_path: str) -> pathlib.Path:
    """Resolve a user-supplied path relative to the repository root."""

    path = pathlib.Path(raw_path)
    if path.is_absolute():
        return path.resolve()
    return (ROOT_DIR / path).resolve()


def collect_cpp_sources(targets: list[str]) -> list[pathlib.Path]:
    """Collect C and C++ source files from the requested lint targets."""

    search_paths = (
        [resolve_repo_path(target) for target in targets]
        if targets
        else list(DEFAULT_CPP_ROOTS)
    )
    results: list[pathlib.Path] = []
    for path in search_paths:
        if path.is_dir():
            for extension in ("*.c", "*.cc", "*.cpp", "*.cxx"):
                results.extend(sorted(path.rglob(extension)))
            continue
        if path.is_file() and is_cpp_source(path):
            results.append(path)
    return sorted(dict.fromkeys(result.resolve() for result in results))


def collect_python_sources(targets: list[str]) -> list[pathlib.Path]:
    """Collect Python source files from the requested lint targets."""

    if not targets:
        return []
    results: list[pathlib.Path] = []
    for raw_target in targets:
        path = resolve_repo_path(raw_target)
        if path.is_dir():
            results.extend(sorted(path.rglob("*.py")))
            continue
        if path.is_file() and is_python_source(path):
            results.append(path)
    return sorted(dict.fromkeys(result.resolve() for result in results))


def split_command(command: str) -> list[str]:
    """Split a compile command using host-appropriate shell rules."""

    if os.name != "nt":
        return shlex.split(command)

    command_line_to_argv = ctypes.windll.shell32.CommandLineToArgvW
    command_line_to_argv.restype = ctypes.POINTER(ctypes.c_wchar_p)
    command_line_to_argv.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    argc = ctypes.c_int(0)
    argv_ptr = command_line_to_argv(command, ctypes.byref(argc))
    if not argv_ptr:
        return shlex.split(command)
    try:
        return [argv_ptr[index] for index in range(argc.value)]
    finally:
        ctypes.windll.kernel32.LocalFree(argv_ptr)


def is_pch_path(argument: str) -> bool:
    """Return whether an argument references a generated precompiled header."""

    lower = argument.lower()
    return "cmake_pch.h" in lower or "cmake_pch.hxx" in lower


def sanitize_arguments(arguments: list[str]) -> list[str]:
    """Strip compile-command arguments that force clang-tidy through PCHs."""

    sanitized: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]

        if argument in ("-include", "-include-pch") and index + 1 < len(arguments):
            if is_pch_path(arguments[index + 1]):
                index += 2
                continue

        if argument == "-Xclang" and index + 1 < len(arguments):
            next_argument = arguments[index + 1]
            if next_argument in ("-include", "-include-pch"):
                if (
                    index + 3 < len(arguments)
                    and arguments[index + 2] == "-Xclang"
                    and is_pch_path(arguments[index + 3])
                ):
                    index += 4
                    continue
                index += 2
                continue
            if is_pch_path(next_argument):
                index += 2
                continue

        if argument.startswith("-include-pch") and is_pch_path(argument):
            index += 1
            continue

        if argument.startswith("-include") and is_pch_path(argument):
            index += 1
            continue

        if is_pch_path(argument):
            index += 1
            continue

        sanitized.append(argument)
        index += 1

    return sanitized


def ensure_compile_commands() -> pathlib.Path:
    """Locate or generate the compilation database used by clang-tidy."""

    require_path(
        BUILD_DIR,
        (
            f"Expected build directory at {BUILD_DIR}\n"
            f"Configure SPRING2 first, for example: cmake -S {ROOT_DIR} -B {BUILD_DIR}"
        ),
    )
    if not BUILD_COMPILE_COMMANDS.exists() and not WORKSPACE_COMPILE_COMMANDS.exists():
        print(
            "compile_commands.json not found; re-running CMake configure with export enabled...",
            file=sys.stderr,
        )
        configure_result = subprocess.run(
            [
                "cmake",
                "-S",
                str(ROOT_DIR),
                "-B",
                str(BUILD_DIR),
                "-G",
                "Ninja",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            ],
            cwd=ROOT_DIR,
            text=True,
            check=False,
        )
        if configure_result.returncode != 0:
            raise SystemExit(configure_result.returncode)
    if BUILD_COMPILE_COMMANDS.exists():
        return BUILD_COMPILE_COMMANDS
    require_path(
        WORKSPACE_COMPILE_COMMANDS,
        f"Expected compilation database at {WORKSPACE_COMPILE_COMMANDS}",
    )
    return WORKSPACE_COMPILE_COMMANDS


def sanitize_compile_commands(
    source_db: pathlib.Path, target_db: pathlib.Path
) -> pathlib.Path:
    """Write a clang-tidy-friendly copy of compile_commands.json."""

    entries = json.loads(source_db.read_text(encoding="utf-8"))
    for entry in entries:
        if isinstance(entry.get("arguments"), list):
            entry["arguments"] = sanitize_arguments(entry["arguments"])
            continue

        command = entry.get("command")
        if isinstance(command, str) and command.strip():
            try:
                sanitized_arguments = sanitize_arguments(split_command(command))
                entry.pop("command", None)
                entry["arguments"] = sanitized_arguments
            except ValueError:
                pass

    target_db.parent.mkdir(parents=True, exist_ok=True)
    target_db.write_text(json.dumps(entries, indent=2), encoding="utf-8")
    return target_db


def load_compile_commands_file_set(compile_db_path: pathlib.Path) -> set[str]:
    """Load the normalized file set referenced by a compilation database."""

    entries = json.loads(compile_db_path.read_text(encoding="utf-8"))
    normalized_entries: set[str] = set()
    for entry in entries:
        entry_file = entry.get("file", "")
        if not entry_file:
            continue
        entry_dir = entry.get("directory", "")
        candidate_path = pathlib.Path(entry_file)
        if not candidate_path.is_absolute() and entry_dir:
            candidate_path = pathlib.Path(entry_dir) / candidate_path
        normalized_entries.add(normalize_compile_db_path(candidate_path))
    return normalized_entries


def discover_linux_system_include_dirs() -> list[str]:
    """Query GCC for Linux system include directories used by clang-tidy."""

    gxx = shutil.which("g++")
    if not gxx:
        return []

    process = subprocess.run(
        [gxx, "-E", "-x", "c++", "-", "-v"],
        input="",
        text=True,
        capture_output=True,
        check=False,
    )
    lines = process.stderr.splitlines()
    includes: list[str] = []
    capture = False
    for line in lines:
        if "#include <...> search starts here:" in line:
            capture = True
            continue
        if "End of search list." in line:
            break
        if capture:
            include_dir = line.strip()
            if include_dir and not re.search(r"/lib/gcc/.*/include$", include_dir):
                includes.append(include_dir)
    return includes


def _is_clangcl_compile_db() -> bool:
    """Return True if the current build's compile_commands.json uses clang-cl."""

    if not BUILD_COMPILE_COMMANDS.exists():
        return False
    try:
        entries = json.loads(BUILD_COMPILE_COMMANDS.read_text(encoding="utf-8"))
        if not entries:
            return False
        # Check both "command" (string) and "arguments" (list) forms.
        first = entries[0]
        if isinstance(first.get("arguments"), list) and first["arguments"]:
            compiler = pathlib.Path(first["arguments"][0]).name.lower()
            return "clang-cl" in compiler
        command = first.get("command", "")
        if command:
            first_token = pathlib.Path(split_command(command)[0]).name.lower()
            return "clang-cl" in first_token
    except Exception:  # noqa: BLE001
        pass
    return False


def build_clang_tidy_common_args() -> list[str]:
    """Build the host-specific clang-tidy argument prefix."""

    args = [
        "-quiet",
        f"-checks={TIDY_CHECKS}",
        "-warnings-as-errors=*",
        "-header-filter=^$",
        "--system-headers=false",
    ]

    if IS_WINDOWS:
        if _is_clangcl_compile_db():
            # ClangCL compile commands use MSVC-style /std:c++20 flags.  Under
            # the MinGW GNU triple those flags are not mapped to a C++ standard,
            # so C++20 features (starts_with, ranges, …) appear missing.  Use
            # the MSVC triple so clang-tidy interprets /std:c++20 correctly.
            target = "x86_64-pc-windows-msvc"
        else:
            target = "x86_64-w64-windows-gnu"
        args.extend(
            [
                f"--extra-arg-before=--target={target}",
                f"--extra-arg=-I{LINT_INCLUDE_DIR}",
                "--extra-arg=-fopenmp",
                "--extra-arg=-w",
                "--extra-arg=-fconstexpr-steps=4194304",
            ]
        )
        # MSYS2 CLANG64 uses libc++ whose headers live under clang64/include/c++/v1/.
        # clang-tidy cannot find them automatically when the x86_64-w64-windows-gnu
        # target is active because that target normally implies GCC's libstdc++.
        # Detect CLANG64 by resolving clang++ to a path that contains "clang64"
        # and add the libc++ directory as an explicit system include.
        clangxx_path = shutil.which("clang++") or shutil.which("clang")
        if clangxx_path:
            clangxx_resolved = pathlib.Path(clangxx_path).resolve()
            if "clang64" in clangxx_resolved.as_posix().lower():
                libcxx_dir = clangxx_resolved.parent.parent / "include" / "c++" / "v1"
                if libcxx_dir.is_dir():
                    args.extend(
                        [
                            "--extra-arg-before=-isystem",
                            f"--extra-arg-before={libcxx_dir}",
                        ]
                    )
        return args

    if IS_MACOS:
        sdk_root = os.environ.get("SDKROOT", "")
        xcrun = shutil.which("xcrun")
        if xcrun and not sdk_root:
            sdk_process = subprocess.run(
                [xcrun, "--sdk", "macosx", "--show-sdk-path"],
                capture_output=True,
                text=True,
                check=False,
            )
            sdk_root = sdk_process.stdout.strip()
        apple_cxx_include_dir = ""
        if xcrun:
            clangxx_process = subprocess.run(
                [xcrun, "--sdk", "macosx", "--find", "clang++"],
                capture_output=True,
                text=True,
                check=False,
            )
            apple_clangxx_path = clangxx_process.stdout.strip()
            if apple_clangxx_path:
                apple_toolchain_usr_dir = (
                    pathlib.Path(apple_clangxx_path).resolve().parent.parent
                )
                apple_cxx_include_dir = str(
                    apple_toolchain_usr_dir / "include" / "c++" / "v1"
                )

        args.extend(["--extra-arg=-w", "--extra-arg=-fconstexpr-steps=4194304"])
        if sdk_root:
            args.extend(
                [
                    "--extra-arg-before=-isysroot",
                    f"--extra-arg-before={sdk_root}",
                    "--extra-arg-before=-stdlib=libc++",
                ]
            )
        if apple_cxx_include_dir and pathlib.Path(apple_cxx_include_dir).is_dir():
            args.extend(
                [
                    "--extra-arg-before=-isystem",
                    f"--extra-arg-before={apple_cxx_include_dir}",
                ]
            )
        return args

    linux_gxx = shutil.which("g++")
    linux_gcc_toolchain_dir = ""
    if linux_gxx:
        libgcc_process = subprocess.run(
            [linux_gxx, "-print-libgcc-file-name"],
            capture_output=True,
            text=True,
            check=False,
        )
        libgcc_path = libgcc_process.stdout.strip()
        if libgcc_path:
            linux_gcc_toolchain_dir = str(
                pathlib.Path(libgcc_path).resolve().parent.parent
            )

    clangxx_bin = ""
    for candidate in ("clang++", "clang++-18", "clang++-17", "clang++-16"):
        if shutil.which(candidate):
            clangxx_bin = candidate
            break
    linux_clang_resource_include_dir = ""
    if clangxx_bin:
        resource_process = subprocess.run(
            [clangxx_bin, "-print-resource-dir"],
            capture_output=True,
            text=True,
            check=False,
        )
        resource_dir = resource_process.stdout.strip()
        if resource_dir:
            linux_clang_resource_include_dir = str(
                pathlib.Path(resource_dir) / "include"
            )

    args.extend(
        [
            "--extra-arg-before=--target=x86_64-linux-gnu",
            f"--extra-arg=-I{LINT_INCLUDE_DIR}",
            "--extra-arg=-isystem",
            f"--extra-arg={ROOT_DIR / 'tests' / 'support'}",
            "--extra-arg=-fopenmp",
            "--extra-arg=-w",
            "--extra-arg=-fconstexpr-steps=4194304",
            "--extra-arg=-D__malloc__(...)=__malloc__",
        ]
    )

    if linux_gcc_toolchain_dir:
        args.insert(1, f"--extra-arg-before=--gcc-toolchain={linux_gcc_toolchain_dir}")

    if linux_clang_resource_include_dir:
        args.extend(
            [
                "--extra-arg-before=-isystem",
                f"--extra-arg-before={linux_clang_resource_include_dir}",
            ]
        )
    for include_dir in discover_linux_system_include_dirs():
        args.extend(
            ["--extra-arg-before=-isystem", f"--extra-arg-before={include_dir}"]
        )
    return args


def run_command(command: list[str], label: str) -> CommandResult:
    """Execute one lint command and capture filtered output."""

    process = subprocess.run(
        command, cwd=ROOT_DIR, capture_output=True, text=True, check=False
    )
    combined_output = []
    if process.stdout:
        combined_output.extend(process.stdout.splitlines())
    if process.stderr:
        combined_output.extend(process.stderr.splitlines())
    filtered_output = [
        line
        for line in combined_output
        if line
        and not SUMMARY_LINE_PATTERN.match(line)
        and not PROCESSING_LINE_PATTERN.match(line)
    ]
    return CommandResult(
        label=label, exit_code=process.returncode, output=filtered_output
    )


def format_progress(processed_count: int, total_count: int) -> str:
    """Return a compact cppcheck-style progress line."""

    percent_done = 100 if total_count == 0 else (processed_count * 100) // total_count
    return f"{processed_count}/{total_count} files checked {percent_done}% done"


def run_parallel_jobs(work_items: list[tuple[str, list[str]]], job_count: int) -> int:
    """Run lint jobs in batches and report compact progress."""

    overall_status = 0
    if not work_items:
        return overall_status

    total_count = len(work_items)
    processed_count = 0

    if job_count < 2 or len(work_items) < 2:
        for label, command in work_items:
            result = run_command(command, label)
            processed_count += 1
            print(format_progress(processed_count, total_count))
            if result.output:
                print(f"Diagnostics for {result.label}:")
                for line in result.output:
                    print(line)
            if result.exit_code != 0:
                overall_status = 1
        return overall_status

    for start_index in range(0, len(work_items), job_count):
        batch = work_items[start_index : start_index + job_count]
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(batch)) as executor:
            futures = [
                executor.submit(run_command, command, label) for label, command in batch
            ]
            results = [future.result() for future in futures]

        for result in results:
            processed_count += 1
            print(format_progress(processed_count, total_count))
            if result.output:
                print(f"Diagnostics for {result.label}:")
                for line in result.output:
                    print(line)
            if result.exit_code != 0:
                overall_status = 1

    return overall_status


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Cross-platform lint driver for the SPRING2 repository."
    )
    parser.add_argument(
        "--build-dir",
        "-b",
        type=pathlib.Path,
        default=DEFAULT_BUILD_DIR,
        help=f"Build directory containing compile_commands.json (default: {DEFAULT_BUILD_DIR})",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        default=[str(ROOT_DIR / "src"), str(ROOT_DIR / "vendor")],
        help="Directories or files to lint (default: src and vendor)",
    )
    return parser.parse_args()


def main() -> int:
    """Run repository linting for the requested paths."""
    global BUILD_DIR, BUILD_COMPILE_COMMANDS

    args = parse_args()
    BUILD_DIR = args.build_dir.resolve()
    BUILD_COMPILE_COMMANDS = BUILD_DIR / "compile_commands.json"

    clang_tidy_bin = find_required_command(
        ("clang-tidy", "clang-tidy-18", "clang-tidy-17", "clang-tidy-16"),
        "clang-tidy",
    )
    python_bin = sys.executable or find_required_command(
        ("python3", "python"), "python"
    )
    lint_jobs = resolve_parallel_job_count()

    lint_targets = args.targets
    cpp_files = collect_cpp_sources(lint_targets)
    python_files = collect_python_sources(lint_targets)

    if not cpp_files and not python_files:
        print("No C/C++ or Python source files found to lint.")
        return 0

    clang_tidy_common_args = build_clang_tidy_common_args()

    compile_db_files: list[pathlib.Path] = []
    standalone_files: list[pathlib.Path] = []
    tidy_db_dir = BUILD_DIR

    if cpp_files:
        compile_commands_path = ensure_compile_commands()
        tidy_db_dir = BUILD_DIR / "tidy_db"
        tidy_compile_commands = sanitize_compile_commands(
            compile_commands_path, tidy_db_dir / "compile_commands.json"
        )
        compile_commands_file_set = load_compile_commands_file_set(
            tidy_compile_commands
        )

        for file_path in cpp_files:
            if normalize_compile_db_path(file_path) in compile_commands_file_set:
                compile_db_files.append(file_path)
            else:
                standalone_files.append(file_path)

        if not compile_db_files and standalone_files:
            print(
                (
                    "No files matched compile_commands.json directly; falling back "
                    "to compilation-database mode for all files."
                ),
                file=sys.stderr,
            )
            compile_db_files = list(standalone_files)
            standalone_files = []

    compile_db_work: list[tuple[str, list[str]]] = []
    for file_path in compile_db_files:
        compile_db_work.append(
            (
                f"Linting compile-db file {file_path}.",
                [
                    clang_tidy_bin,
                    *clang_tidy_common_args,
                    "-p",
                    str(tidy_db_dir),
                    str(file_path),
                ],
            )
        )

    standalone_work: list[tuple[str, list[str]]] = []
    for file_path in standalone_files:
        include_args = []
        if IS_MSYS_WINDOWS:
            include_args.append(f"-I{LINT_INCLUDE_DIR}")
        include_args.extend(f"-I{include_dir}" for include_dir in EXTRA_INCLUDES)
        driver_args = ["--driver-mode=g++", "-std=c++20", "-x", "c++"]
        if file_path.suffix.lower() == ".c":
            driver_args = ["--driver-mode=gcc", "-std=gnu11", "-x", "c"]
        standalone_work.append(
            (
                f"Linting standalone file {file_path}.",
                [
                    clang_tidy_bin,
                    *clang_tidy_common_args,
                    str(file_path),
                    "--",
                    *driver_args,
                    *include_args,
                    f"-I{file_path.parent}",
                ],
            )
        )

    python_work = [
        (
            f"Linting python file {file_path}.",
            [python_bin, "-m", "py_compile", str(file_path)],
        )
        for file_path in python_files
    ]

    status = 0
    status |= run_parallel_jobs(compile_db_work, lint_jobs)
    status |= run_parallel_jobs(standalone_work, lint_jobs)
    status |= run_parallel_jobs(python_work, lint_jobs)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
