#!/usr/bin/env python3
"""Compare SPRING2, legacy Spring, and gzip on the same FASTQ inputs."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess

from bench_common import (
    ROOT_DIR,
    decompress_gzip_to_file,
    default_spring_binary,
    detect_max_read_length,
    download_file,
    ensure_directory,
    ensure_spring_binary,
    env_or_default_path,
    normalized_fastq_digest,
    normalized_fastq_match,
    read_decompressed_bytes,
    run_logged_process,
    runner_help_contains,
    sum_decompressed_sizes,
    sum_sizes,
    write_gzip_from_bytes,
)

TMP_DIR = ROOT_DIR / "out" / "tests" / "bench" / "comparison"
TMP_INPUT_DIR = ROOT_DIR / "tests" / "fixtures" / "input"
TMP_LOG_DIR = TMP_DIR / "logs"
TMP_OUTPUT_DIR = TMP_DIR / "runs"
URL_R1 = (
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_1.fastq.gz"
)
URL_R2 = (
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_2.fastq.gz"
)
DEFAULT_PATH_R1 = TMP_INPUT_DIR / "SRR8185389_1.fastq.gz"
DEFAULT_PATH_R2 = TMP_INPUT_DIR / "SRR8185389_2.fastq.gz"
SPRING_V1_ENV_NAME = "spring_v1"
MAX_SHORT_READ_LENGTH = 511
DISK_PATH_SELECTION_MESSAGE = (
    "Disk-backed compression path selected based on estimated peak working "
    "memory and available memory."
)


class BenchmarkResult(dict):
    """Dictionary-backed result record for one benchmark configuration."""


def spring_bin() -> pathlib.Path:
    """Return the SPRING2 executable path for this benchmark run."""

    return env_or_default_path("SPRING_BIN", default_spring_binary())


def thread_count() -> str:
    """Return the configured worker-thread count."""

    return os.environ.get("THREADS", "8")


def input_fastq_1() -> pathlib.Path:
    """Return the primary benchmark FASTQ path."""

    return pathlib.Path(os.environ.get("INPUT_FASTQ_1", str(DEFAULT_PATH_R1)))


def input_fastq_2() -> pathlib.Path | None:
    """Return the optional mate FASTQ path when configured."""

    raw = os.environ.get("INPUT_FASTQ_2", str(DEFAULT_PATH_R2))
    path = pathlib.Path(raw)
    return path if path.exists() or raw == str(DEFAULT_PATH_R2) else None


def ensure_inputs() -> tuple[pathlib.Path, pathlib.Path | None]:
    """Download default inputs when needed and validate custom paths."""

    ensure_directory(TMP_INPUT_DIR)
    ensure_directory(TMP_LOG_DIR)
    ensure_directory(TMP_OUTPUT_DIR)
    path_1 = input_fastq_1()
    path_2 = input_fastq_2()
    if path_1 == DEFAULT_PATH_R1:
        download_file(URL_R1, DEFAULT_PATH_R1)
    if path_2 == DEFAULT_PATH_R2:
        download_file(URL_R2, DEFAULT_PATH_R2)
    if not path_1.exists():
        raise FileNotFoundError(f"Primary INPUT_FASTQ_1 does not exist: {path_1}")
    return path_1, path_2 if path_2 and path_2.exists() else None


def ensure_spring_v1_prefix() -> list[str]:
    """Return the command prefix that runs legacy Spring from mamba."""

    mamba = shutil.which("mamba")
    if not mamba:
        raise RuntimeError(
            f"Install mamba and create the {SPRING_V1_ENV_NAME} environment."
        )
    env_list = subprocess.run(
        [mamba, "env", "list"], capture_output=True, text=True, check=True
    )
    if SPRING_V1_ENV_NAME not in env_list.stdout:
        raise RuntimeError(
            f"Required mamba environment not found: {SPRING_V1_ENV_NAME}."
        )
    subprocess.run(
        [mamba, "run", "-n", SPRING_V1_ENV_NAME, "spring", "--help"],
        cwd=ROOT_DIR,
        capture_output=True,
        text=True,
        check=True,
    )
    return [mamba, "run", "-n", SPRING_V1_ENV_NAME, "spring"]


def benchmark_display_name(label: str) -> str:
    """Map an internal benchmark label to a human-readable display name."""

    return {
        "current_memory": "Current Spring (memory_path)",
        "current_disk": "Current Spring (disk_path)",
        "spring_v1": "Spring v1",
        "gzip": "gzip",
    }[label]


def run_benchmark(
    label: str,
    runner_prefix: list[str],
    input_paths: list[pathlib.Path],
    max_read_length: int,
) -> BenchmarkResult:
    """Run one compression/decompression benchmark variant and summarize it."""

    output_prefix = TMP_OUTPUT_DIR / f"{input_paths[0].stem}.{label}"
    compress_log = TMP_LOG_DIR / f"{label}_compress.log"
    decompress_log = TMP_LOG_DIR / f"{label}_decompress.log"
    if label == "gzip":
        compressed_paths = [
            TMP_OUTPUT_DIR / f"{output_prefix.name}.{index + 1}.fastq.gz"
            for index in range(len(input_paths))
        ]
        decompressed_paths = [
            TMP_OUTPUT_DIR / f"{output_prefix.name}.roundtrip.{index + 1}.fastq"
            for index in range(len(input_paths))
        ]
    else:
        compressed_paths = [TMP_OUTPUT_DIR / f"{output_prefix.name}.sp"]
        if len(input_paths) == 1:
            decompressed_paths = [
                TMP_OUTPUT_DIR / f"{output_prefix.name}.roundtrip.fastq"
            ]
        else:
            decompressed_paths = [
                TMP_OUTPUT_DIR / f"{output_prefix.name}.roundtrip.fastq.1",
                TMP_OUTPUT_DIR / f"{output_prefix.name}.roundtrip.fastq.2",
            ]

    for stale in compressed_paths + decompressed_paths:
        stale.unlink(missing_ok=True)

    print(f"Running {benchmark_display_name(label)} lossless compression")
    print(f"  input:   {input_paths[0]}")
    if len(input_paths) == 2:
        print(f"  input 2: {input_paths[1]}")
    print(f"  output:  {' '.join(str(path) for path in compressed_paths)}")
    if label != "gzip":
        print(f"  threads: {thread_count()}")
    if label == "current_memory":
        memory_budget = os.environ.get("SPRING_MEMORY_PATH_MEMORY_GB", "1024")
        print(f"  storage path: forced memory_path (-m {memory_budget})")
    elif label == "current_disk":
        memory_budget = os.environ.get("SPRING_DISK_PATH_MEMORY_GB", "0.00001")
        print(f"  storage path: forced disk_path (-m {memory_budget})")
    else:
        memory_budget = None
    print(f"  max read length: {max_read_length}")

    if label == "gzip":
        for input_path, output_path in zip(input_paths, compressed_paths):
            write_gzip_from_bytes(read_decompressed_bytes(input_path), output_path)
        compress_metrics = BenchmarkResult(
            elapsed_seconds=0.0,
            user_seconds=None,
            system_seconds=None,
            cpu_percent=None,
            max_rss_kb=None,
        )  # type: ignore[call-arg]
        compress_output = ""
    else:
        spring_args = ["-c"]
        if label == "spring_v1":
            spring_args.extend(["-i", str(input_paths[0])])
            if len(input_paths) == 2:
                spring_args.append(str(input_paths[1]))
            spring_args.extend(
                ["-o", str(compressed_paths[0]), "-t", thread_count(), "-q", "lossless"]
            )
            if max_read_length > MAX_SHORT_READ_LENGTH:
                spring_args.append("-l")
        else:
            spring_args.extend(["--R1", str(input_paths[0])])
            if len(input_paths) == 2:
                spring_args.extend(["--R2", str(input_paths[1])])
            spring_args.extend(
                ["-o", str(compressed_paths[0]), "-t", thread_count(), "-q", "lossless"]
            )
            if memory_budget is not None:
                spring_args.extend(["-m", memory_budget])
        if input_paths[0].suffix == ".gz" and runner_help_contains(
            runner_prefix, "gzipped-fastq"
        ):
            spring_args = ["-g", *spring_args]
        compress_metrics = run_logged_process(
            [*runner_prefix, *spring_args],
            log_path=compress_log,
        )
        compress_output = compress_metrics.output
        if (
            label == "current_disk"
            and DISK_PATH_SELECTION_MESSAGE not in compress_output
        ):
            raise RuntimeError(
                "SPRING2 disk_path benchmark did not report disk-backed path selection."
            )
        if label == "current_memory" and DISK_PATH_SELECTION_MESSAGE in compress_output:
            raise RuntimeError(
                "SPRING2 memory_path benchmark unexpectedly reported "
                "disk-backed path selection."
            )

    print(f"Running {benchmark_display_name(label)} decompression")
    if label == "gzip":
        for archive, restored in zip(compressed_paths, decompressed_paths):
            decompress_gzip_to_file(archive, restored)
        decompress_metrics = BenchmarkResult(
            elapsed_seconds=0.0,
            user_seconds=None,
            system_seconds=None,
            cpu_percent=None,
            max_rss_kb=None,
        )  # type: ignore[call-arg]
    else:
        decompress_args = [
            "-d",
            "-i",
            str(compressed_paths[0]),
            "-o",
            str(TMP_OUTPUT_DIR / f"{output_prefix.name}.roundtrip.fastq"),
        ]
        if compressed_paths[0].suffix == ".gz" and runner_help_contains(
            runner_prefix, "gzipped-fastq"
        ):
            decompress_args = ["-g", *decompress_args]
        decompress_metrics = run_logged_process(
            [*runner_prefix, *decompress_args],
            log_path=decompress_log,
        )

    input_size = sum_decompressed_sizes(input_paths)
    output_size = sum_sizes(compressed_paths)
    decompressed_size = sum_sizes(decompressed_paths)
    original_checksum = normalized_fastq_digest(input_paths)
    decompressed_checksum = normalized_fastq_digest(decompressed_paths)
    checksum_status = (
        "match" if original_checksum == decompressed_checksum else "mismatch"
    )
    roundtrip_ok = normalized_fastq_match(input_paths, decompressed_paths)
    reduction = ((input_size - output_size) * 100.0 / input_size) if input_size else 0.0
    ratio = (input_size / output_size) if output_size else 0.0

    print(f"\nBenchmark result ({benchmark_display_name(label)})")
    print(f"  original bytes:   {input_size}")
    print(f"  compressed bytes: {output_size}")
    print(f"  decompressed bytes: {decompressed_size}")
    print(f"  size reduction:   {reduction:.2f}%")
    print(f"  compression ratio {ratio:.3f}x")
    print(f"  checksum status:  {checksum_status}")
    print(
        f"  decompressed file matches input: {'identical' if roundtrip_ok else 'different'}"
    )

    return BenchmarkResult(
        label=benchmark_display_name(label),
        output_size=output_size,
        reduction_percent=reduction,
        compression_ratio=ratio,
        compress_elapsed_seconds=(
            compress_metrics.get("elapsed_seconds", 0.0)
            if isinstance(compress_metrics, dict)
            else compress_metrics.elapsed_seconds
        ),
        decompress_elapsed_seconds=(
            decompress_metrics.get("elapsed_seconds", 0.0)
            if isinstance(decompress_metrics, dict)
            else decompress_metrics.elapsed_seconds
        ),
        compress_max_rss_kb=(
            compress_metrics.get("max_rss_kb")
            if isinstance(compress_metrics, dict)
            else compress_metrics.max_rss_kb
        ),
        decompress_max_rss_kb=(
            decompress_metrics.get("max_rss_kb")
            if isinstance(decompress_metrics, dict)
            else decompress_metrics.max_rss_kb
        ),
        roundtrip_status="identical" if roundtrip_ok else "different",
    )


def print_current_path_summary(
    memory_result: BenchmarkResult, disk_result: BenchmarkResult
) -> None:
    """Print the storage-path comparison between memory and disk modes."""

    print("\nSPRING2 storage-path comparison")
    print(f"  memory_path compressed bytes: {memory_result['output_size']}")
    print(f"  disk_path compressed bytes:   {disk_result['output_size']}")
    print(f"  memory_path ratio:            {memory_result['compression_ratio']:.3f}x")
    print(f"  disk_path ratio:              {disk_result['compression_ratio']:.3f}x")
    print(f"  memory_path reduction:        {memory_result['reduction_percent']:.2f}%")
    print(f"  disk_path reduction:          {disk_result['reduction_percent']:.2f}%")
    print(
        f"  memory_path compression time: {memory_result['compress_elapsed_seconds']:.3f}s"
    )
    print(
        f"  disk_path compression time:   {disk_result['compress_elapsed_seconds']:.3f}s"
    )
    print(f"  memory_path round-trip:       {memory_result['roundtrip_status']}")
    print(f"  disk_path round-trip:         {disk_result['roundtrip_status']}")


def print_comparison_summary(results: list[BenchmarkResult]) -> None:
    """Print a compact size and speed summary across all benchmark modes."""

    print("\nComparison summary")
    for result in results:
        print(
            f"  {result['label']}: {result['output_size']} bytes, "
            f"{result['compression_ratio']:.3f}x, "
            f"{result['compress_elapsed_seconds']:.3f}s"
        )
    winner = min(results, key=lambda item: item["output_size"])
    print(f"  size winner: {winner['label']}")


def main() -> int:
    """Run all comparison benchmark variants and print a final summary."""

    input_1, input_2 = ensure_inputs()
    ensure_spring_binary(spring_bin())
    spring_v1_prefix = ensure_spring_v1_prefix()
    input_paths = [input_1] + ([input_2] if input_2 is not None else [])
    max_read_length = detect_max_read_length(input_1)

    current_memory = run_benchmark(
        "current_memory", [str(spring_bin())], input_paths, max_read_length
    )
    current_disk = run_benchmark(
        "current_disk", [str(spring_bin())], input_paths, max_read_length
    )
    spring_v1 = run_benchmark(
        "spring_v1", spring_v1_prefix, input_paths, max_read_length
    )
    gzip_result = run_benchmark("gzip", ["gzip"], input_paths, max_read_length)

    print_current_path_summary(current_memory, current_disk)
    print_comparison_summary([current_memory, current_disk, spring_v1, gzip_result])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
