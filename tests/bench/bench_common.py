#!/usr/bin/env python3
"""Shared helpers for the Python benchmark runners in tests/bench."""

from __future__ import annotations

import ctypes
import gzip
import hashlib
import os
import pathlib
import shutil
import subprocess
import threading
import time
import urllib.request
from dataclasses import dataclass
from typing import Iterable, Sequence

try:
    import resource
except ImportError:
    resource = None


ROOT_DIR = pathlib.Path(__file__).resolve().parents[2]
BUILD_DIR = ROOT_DIR / "out" / "build"


@dataclass
class CommandMetrics:
    """Captured runtime and resource-usage information for a subprocess."""

    return_code: int
    elapsed_seconds: float
    user_seconds: float | None
    system_seconds: float | None
    cpu_percent: str | None
    max_rss_kb: int | None
    output: str


def is_windows() -> bool:
    """Return whether the current platform is Windows."""

    return os.name == "nt"


def default_spring_binary() -> pathlib.Path:
    """Return the default path to the SPRING2 executable in the build tree."""

    name = "spring2.exe" if is_windows() else "spring2"
    flat = BUILD_DIR / name
    if flat.exists():
        return flat
    # MSVC multi-config generators place binaries in a config subdirectory.
    for config in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
        candidate = BUILD_DIR / config / name
        if candidate.exists():
            return candidate
    return flat


def default_preview_binary() -> pathlib.Path:
    """Return the default path to the preview-capable SPRING2 executable."""

    return default_spring_binary()


def env_or_default_path(env_name: str, default: pathlib.Path) -> pathlib.Path:
    """Resolve a path from the environment, falling back to a default."""

    raw = os.environ.get(env_name, "")
    if raw:
        return pathlib.Path(raw)
    return default


def ensure_directory(path: pathlib.Path) -> None:
    """Create a directory and its parents when they do not yet exist."""

    path.mkdir(parents=True, exist_ok=True)


def find_ninja_args() -> list[str]:
    """Return CMake generator arguments for Ninja when it is available."""

    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    return []


def ensure_spring_binary(
    spring_bin: pathlib.Path,
    *,
    extra_config_args: Sequence[str] = (),
    copy_runtime_dlls: bool = False,
) -> None:
    """Configure and build SPRING2 when the expected binary is missing."""

    if spring_bin.exists():
        return

    ensure_directory(BUILD_DIR)
    configure_command = ["cmake", "-S", str(ROOT_DIR), "-B", str(BUILD_DIR)]
    configure_command.extend(find_ninja_args())
    configure_command.extend(extra_config_args)
    subprocess.run(configure_command, cwd=ROOT_DIR, check=True)
    subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "spring2", "--parallel"],
        cwd=ROOT_DIR,
        check=True,
    )
    if copy_runtime_dlls and is_windows():
        subprocess.run(
            ["cmake", "--build", str(BUILD_DIR), "--target", "copy_runtime_dlls"],
            cwd=ROOT_DIR,
            check=False,
        )


def download_file(url: str, destination: pathlib.Path) -> None:
    """Download a file once and reuse it on later benchmark runs.

    Downloads to a temporary path first and only renames into place on
    success, so a failed or truncated transfer (e.g. an interrupted FTP
    connection) is never mistaken for a valid cached file on a later run.
    """

    if destination.exists():
        return
    ensure_directory(destination.parent)
    tmp_destination = destination.with_suffix(destination.suffix + ".part")
    try:
        with urllib.request.urlopen(url) as response, tmp_destination.open(
            "wb"
        ) as output:
            shutil.copyfileobj(response, output)
        tmp_destination.replace(destination)
    finally:
        tmp_destination.unlink(missing_ok=True)


def _get_windows_peak_rss_kb(pid: int) -> int | None:
    """Read peak resident memory for a process on Windows."""

    class ProcessMemoryCounters(ctypes.Structure):
        """Subset of PROCESS_MEMORY_COUNTERS used by GetProcessMemoryInfo."""

        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    process = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not process:
        return None
    try:
        counters = ProcessMemoryCounters(ctypes.sizeof(ProcessMemoryCounters))
        if not psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
            return None
        return int(counters.PeakWorkingSetSize // 1024)
    finally:
        kernel32.CloseHandle(process)


def _get_unix_rss_kb(pid: int) -> int | None:
    """Read resident memory for a process on Unix-like systems via ps."""

    try:
        output = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    if not output:
        return None
    try:
        return int(output)
    except ValueError:
        return None


def _get_peak_rss_kb(pid: int) -> int | None:
    """Dispatch platform-specific peak RSS collection for a child process."""

    if is_windows():
        return _get_windows_peak_rss_kb(pid)
    return _get_unix_rss_kb(pid)


def _resource_usage_before() -> tuple[float, float] | None:
    """Capture child-process CPU usage before spawning a subprocess."""

    if resource is None:
        return None
    getrusage = getattr(resource, "getrusage", None)
    rusage_children = getattr(resource, "RUSAGE_CHILDREN", None)
    if getrusage is None or rusage_children is None:
        return None
    usage = getrusage(rusage_children)
    return usage.ru_utime, usage.ru_stime


def _resource_usage_after(
    before: tuple[float, float] | None,
) -> tuple[float | None, float | None]:
    """Compute child-process CPU usage deltas after a subprocess exits."""

    if resource is None or before is None:
        return None, None
    getrusage = getattr(resource, "getrusage", None)
    rusage_children = getattr(resource, "RUSAGE_CHILDREN", None)
    if getrusage is None or rusage_children is None:
        return None, None
    usage = getrusage(rusage_children)
    return usage.ru_utime - before[0], usage.ru_stime - before[1]


def run_logged_process(
    command: Sequence[str],
    *,
    cwd: pathlib.Path = ROOT_DIR,
    log_path: pathlib.Path | None = None,
    echo: bool = True,
    check: bool = True,
) -> CommandMetrics:
    """Run a command, mirror its output, and collect timing/resource metrics."""

    if log_path is not None:
        ensure_directory(log_path.parent)
    log_handle = log_path.open("w", encoding="utf-8") if log_path is not None else None
    start = time.perf_counter()
    before = _resource_usage_before()
    process = subprocess.Popen(
        [str(part) for part in command],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        universal_newlines=True,
    )

    output_lines: list[str] = []
    lock = threading.Lock()

    def reader() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            with lock:
                output_lines.append(line)
                if echo:
                    print(line, end="")
                if log_handle is not None:
                    log_handle.write(line)
                    log_handle.flush()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    peak_rss_kb = 0
    while process.poll() is None:
        rss = _get_peak_rss_kb(process.pid)
        if rss is not None:
            peak_rss_kb = max(peak_rss_kb, rss)
        time.sleep(0.1)
    thread.join()
    process.wait()
    if log_handle is not None:
        log_handle.close()
    elapsed = time.perf_counter() - start
    user_seconds, system_seconds = _resource_usage_after(before)
    cpu_percent = None
    if user_seconds is not None and system_seconds is not None and elapsed > 0:
        cpu_percent = f"{((user_seconds + system_seconds) / elapsed) * 100:.0f}%"
    metrics = CommandMetrics(
        return_code=process.returncode,
        elapsed_seconds=elapsed,
        user_seconds=user_seconds,
        system_seconds=system_seconds,
        cpu_percent=cpu_percent,
        max_rss_kb=peak_rss_kb or None,
        output="".join(output_lines),
    )
    if check and process.returncode != 0:
        raise subprocess.CalledProcessError(
            process.returncode, [str(part) for part in command], output=metrics.output
        )
    return metrics


def parse_preview_assay(preview_output: str) -> str:
    """Extract the assay label from preview output text."""

    for line in preview_output.splitlines():
        if line.startswith("Assay Type:"):
            return line.split(":", 1)[1].strip()
    return "unavailable (assay not found)"


def get_archive_assay_label(
    preview_bin: pathlib.Path, archive_path: pathlib.Path
) -> str:
    """Run preview on an archive and return its assay label when available."""

    if not preview_bin.exists():
        return "unavailable (preview binary missing)"
    try:
        result = subprocess.run(
            [str(preview_bin), "-p", str(archive_path)],
            cwd=ROOT_DIR,
            capture_output=True,
            text=True,
            check=True,
        )
    except subprocess.CalledProcessError:
        return "unavailable (preview failed)"
    return parse_preview_assay(result.stdout)


def open_maybe_gzip_text(path: pathlib.Path):
    """Open plain-text or gzip-compressed text input with normalized newlines."""

    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", newline=None)
    return path.open("r", encoding="utf-8", newline=None)


def detect_max_read_length(path: pathlib.Path) -> int:
    """Scan a FASTQ file and return the maximum observed read length."""

    max_length = 0
    with open_maybe_gzip_text(path) as handle:
        for index, line in enumerate(handle):
            if index % 4 == 1:
                max_length = max(max_length, len(line.rstrip("\r\n")))
    return max_length


def read_decompressed_bytes(path: pathlib.Path) -> bytes:
    """Read a file as raw bytes, transparently decompressing gzip inputs."""

    if path.suffix == ".gz":
        with gzip.open(path, "rb") as handle:
            return handle.read()
    return path.read_bytes()


def decompressed_size(path: pathlib.Path) -> int:
    """Return the logical uncompressed size of a plain or gzip file."""

    if path.suffix == ".gz":
        return len(read_decompressed_bytes(path))
    return path.stat().st_size


def _iter_normalized_fastq_chunks(path: pathlib.Path) -> Iterable[bytes]:
    """Yield canonicalized FASTQ content for digest and equality checks."""

    with open_maybe_gzip_text(path) as handle:
        for index, line in enumerate(handle):
            stripped = line.rstrip("\r\n")
            if index % 4 == 2 and stripped.startswith("+"):
                yield b"+\n"
            else:
                yield stripped.encode("utf-8") + b"\n"


def normalized_fastq_digest(paths: Sequence[pathlib.Path]) -> str:
    """Hash FASTQ content after normalizing quality-separator lines."""

    digest = hashlib.sha256()
    for path in paths:
        for chunk in _iter_normalized_fastq_chunks(path):
            digest.update(chunk)
    return digest.hexdigest()


def normalized_fastq_match(
    input_paths: Sequence[pathlib.Path], output_paths: Sequence[pathlib.Path]
) -> bool:
    """Return whether two FASTQ path lists are equal after normalization."""

    if len(input_paths) != len(output_paths):
        return False
    return normalized_fastq_digest(input_paths) == normalized_fastq_digest(output_paths)


def write_gzip_from_bytes(data: bytes, output_path: pathlib.Path) -> None:
    """Write deterministic gzip output from in-memory bytes."""

    ensure_directory(output_path.parent)
    with output_path.open("wb") as raw_output:
        with gzip.GzipFile(
            filename="", mode="wb", fileobj=raw_output, mtime=0
        ) as handle:
            handle.write(data)


def decompress_gzip_to_file(
    input_path: pathlib.Path, output_path: pathlib.Path
) -> None:
    """Expand a gzip file to a plain-text destination path."""

    ensure_directory(output_path.parent)
    with gzip.open(input_path, "rb") as source, output_path.open("wb") as destination:
        shutil.copyfileobj(source, destination)


def sum_sizes(paths: Sequence[pathlib.Path]) -> int:
    """Return the total on-disk size for all existing paths in a sequence."""

    total = 0
    for path in paths:
        if path.exists():
            total += path.stat().st_size
    return total


def sum_decompressed_sizes(paths: Sequence[pathlib.Path]) -> int:
    """Return the total logical uncompressed size across a path sequence."""

    return sum(decompressed_size(path) for path in paths)


def runner_help_contains(command_prefix: Sequence[str], needle: str) -> bool:
    """Check whether a runner's help text advertises a specific token."""

    try:
        result = subprocess.run(
            [str(part) for part in command_prefix] + ["--help"],
            cwd=ROOT_DIR,
            capture_output=True,
            text=True,
            check=True,
        )
    except subprocess.CalledProcessError:
        return False
    return needle in result.stdout or needle in result.stderr


def print_metrics_block(title: str, metrics: CommandMetrics) -> None:
    """Print a compact timing and resource summary for one subprocess."""

    print(title)
    print(f"  elapsed time:     {metrics.elapsed_seconds:.3f}s")
    if metrics.cpu_percent is not None:
        print(f"  cpu usage:        {metrics.cpu_percent}")
    if metrics.user_seconds is not None or metrics.system_seconds is not None:
        print(
            "  cpu time:         user "
            f"{(metrics.user_seconds or 0.0):.3f}s, system {(metrics.system_seconds or 0.0):.3f}s"
        )
    if metrics.max_rss_kb is not None:
        print(
            f"  peak memory:      {metrics.max_rss_kb} KB ({metrics.max_rss_kb / 1024:.2f} MB RSS)"
        )
