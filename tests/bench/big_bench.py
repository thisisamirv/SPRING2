#!/usr/bin/env python3
"""Run the large paired-end benchmark against the SRR8185389 dataset."""

from __future__ import annotations

import argparse
import os
import pathlib

from bench_common import (
    ROOT_DIR,
    default_spring_binary,
    download_file,
    ensure_directory,
    ensure_spring_binary,
    env_or_default_path,
    normalized_fastq_match,
    print_metrics_block,
    run_logged_process,
)

TMP_DIR = ROOT_DIR / "out" / "tests" / "bench" / "big"
TMP_INPUT_DIR = ROOT_DIR / "tests" / "fixtures" / "input"
TMP_LOG_DIR = TMP_DIR / "logs"
TMP_OUTPUT_DIR = TMP_DIR / "runs"
BIG_BENCH_LOG = TMP_LOG_DIR / "big_bench.log"
URL_R1 = (
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_1.fastq.gz"
)
URL_R2 = (
    "https://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_2.fastq.gz"
)
MD5_R1 = "ad5e8dced2ead9f8c181d1c30b5fddad"
MD5_R2 = "635d06de10de8a662a4d07b5064fe802"
PATH_R1 = TMP_INPUT_DIR / "SRR8185389_1.fastq.gz"
PATH_R2 = TMP_INPUT_DIR / "SRR8185389_2.fastq.gz"


def parse_args() -> argparse.Namespace:
    """Parse command-line flags for the big benchmark runner."""

    parser = argparse.ArgumentParser(description="Run the large paired-end benchmark.")
    parser.add_argument("--no_debug", action="store_true")
    return parser.parse_args()


def spring_bin() -> pathlib.Path:
    """Return the SPRING2 executable path for this benchmark run."""

    return env_or_default_path("SPRING_BIN", default_spring_binary())


def threads() -> str:
    """Return the configured worker-thread count."""

    return os.environ.get("THREADS", "8")


def log_line(message: str) -> None:
    """Write a message to stdout and to the benchmark log file."""

    print(message)
    ensure_directory(BIG_BENCH_LOG.parent)
    with BIG_BENCH_LOG.open("a", encoding="utf-8") as handle:
        handle.write(message + "\n")


def show_step_timing_summary(log_path: pathlib.Path) -> None:
    """Extract and print per-step timing lines from the benchmark log."""

    if not log_path.exists():
        print("\nStep timings\n  No step timings found.")
        return
    summary: list[str] = []
    pending: str | None = None
    with log_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if stripped.endswith("..."):
                pending = stripped[:-3].rstrip()
                continue
            if stripped.startswith("Time for this step:") and pending:
                summary.append(
                    f"  {len(summary) + 1:02d}. {pending}: {stripped.split(':', 1)[1].strip()}"
                )
                pending = None
                continue
            if stripped.startswith(
                "Total time for compression:"
            ) or stripped.startswith("Total time for decompression:"):
                summary.append(f"  {len(summary) + 1:02d}. {stripped}")
    print("\nStep timings")
    if not summary:
        print("  No step timings found.")
        return
    for item in summary:
        print(item)


def initialize_environment() -> None:
    """Prepare directories, logs, and benchmark inputs."""

    ensure_directory(TMP_INPUT_DIR)
    ensure_directory(TMP_LOG_DIR)
    ensure_directory(TMP_OUTPUT_DIR)
    if BIG_BENCH_LOG.exists():
        BIG_BENCH_LOG.unlink()
    download_file(URL_R1, PATH_R1, MD5_R1)
    download_file(URL_R2, PATH_R2, MD5_R2)


def main() -> int:
    """Run the large paired-end benchmark and report timing and integrity."""

    args = parse_args()
    initialize_environment()
    ensure_spring_binary(
        spring_bin(),
        extra_config_args=("-DSPRING_STATIC_RUNTIMES=OFF",),
        copy_runtime_dlls=True,
    )

    output_file = TMP_OUTPUT_DIR / "SRR8185389_pe.sp"
    decomp_base = TMP_OUTPUT_DIR / "SRR8185389_pe.roundtrip.fastq"
    decomp_file_1 = TMP_OUTPUT_DIR / "SRR8185389_pe.roundtrip.fastq.1"
    decomp_file_2 = TMP_OUTPUT_DIR / "SRR8185389_pe.roundtrip.fastq.2"
    for path in (output_file, decomp_file_1, decomp_file_2):
        if path.exists():
            path.unlink()

    verbose_args: list[str] = [] if args.no_debug else ["-v", "debug"]
    log_line("Running Spring paired-end compression (SRR2990433)")
    log_line(f"  R1:      {PATH_R1}")
    log_line(f"  R2:      {PATH_R2}")
    log_line(f"  threads: {threads()}")
    compress_metrics = run_logged_process(
        [
            str(spring_bin()),
            *verbose_args,
            "-c",
            "--R1",
            str(PATH_R1),
            "--R2",
            str(PATH_R2),
            "-o",
            str(output_file),
            "-t",
            threads(),
            "-q",
            "lossless",
            "-n",
            "Big Benchmark SRR2990433",
        ],
        log_path=BIG_BENCH_LOG,
    )

    log_line("")
    log_line("Running Spring decompression")
    decompress_metrics = run_logged_process(
        [
            str(spring_bin()),
            *verbose_args,
            "-d",
            "-i",
            str(output_file),
            "-o",
            str(decomp_base),
        ],
        log_path=BIG_BENCH_LOG,
    )

    input_size = PATH_R1.stat().st_size + PATH_R2.stat().st_size
    output_size = output_file.stat().st_size
    decomp_size = decomp_file_1.stat().st_size + decomp_file_2.stat().st_size
    reduction = ((input_size - output_size) * 100.0 / input_size) if input_size else 0.0
    ratio = (input_size / output_size) if output_size else 0.0
    read_1_ok = normalized_fastq_match([PATH_R1], [decomp_file_1])
    read_2_ok = normalized_fastq_match([PATH_R2], [decomp_file_2])

    print("\nBenchmark result (Paired-End combined)")
    print(f"  original bytes:   {input_size:,}")
    print(f"  compressed bytes: {output_size:,}")
    print(f"  decompressed bytes: {decomp_size:,}")
    print(f"  size reduction:   {reduction:.2f}%")
    print(f"  compression ratio {ratio:.3f}x")
    print_metrics_block("\nCompression resources", compress_metrics)
    print_metrics_block("\nDecompression resources", decompress_metrics)
    print("\nRound-trip check")
    print(f"  Read 1 status: {'match' if read_1_ok else 'mismatch'}")
    print(f"  Read 2 status: {'match' if read_2_ok else 'mismatch'}")
    print(f"  Overall status: {'PASSED' if read_1_ok and read_2_ok else 'FAILED'}")
    show_step_timing_summary(BIG_BENCH_LOG)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
