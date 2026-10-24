#!/usr/bin/env python3
"""Run small local benchmark flows for SPRING2 and assay-specific inputs."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil

from bench_common import (
    ROOT_DIR,
    default_preview_binary,
    default_spring_binary,
    detect_max_read_length,
    ensure_directory,
    ensure_spring_binary,
    env_or_default_path,
    get_archive_assay_label,
    normalized_fastq_match,
    run_logged_process,
)

INPUT_DIR = ROOT_DIR / "tests" / "data"
OUTPUT_BASE = ROOT_DIR / "out" / "tests" / "bench" / "small"
LOG_DIR = OUTPUT_BASE / "logs"
OUTPUT_DIR = OUTPUT_BASE / "runs"
TEMP_ROOT = OUTPUT_BASE / "temp"


def parse_args() -> argparse.Namespace:
    """Parse the optional input FASTQ path for the benchmark run."""

    parser = argparse.ArgumentParser(description="Run the small SPRING2 benchmark.")
    parser.add_argument(
        "input_fastq", nargs="?", default=str(INPUT_DIR / "sample.fastq")
    )
    return parser.parse_args()


def current_threads() -> str:
    """Return the configured worker-thread count."""

    return str(int(os.environ.get("THREADS", "8")))


def spring_bin() -> pathlib.Path:
    """Return the SPRING2 executable path for this benchmark run."""

    return env_or_default_path("SPRING_BIN", default_spring_binary())


def preview_bin() -> pathlib.Path:
    """Return the preview executable path for archive inspection."""

    return env_or_default_path("SPRING_PREVIEW_BIN", default_preview_binary())


def run_single_file_benchmark(input_path: pathlib.Path) -> None:
    """Compress and decompress one FASTQ file and report the round-trip result."""

    spring = spring_bin()
    preview = preview_bin()
    input_name = input_path.name
    stem = input_name.removesuffix(".gz")
    stem = pathlib.Path(stem).stem
    output_file = OUTPUT_DIR / f"{stem}.sp"
    decompressed_output = OUTPUT_DIR / f"{stem}.roundtrip.fastq"
    compress_log = LOG_DIR / f"{stem}.compress.log"
    decompress_log = LOG_DIR / f"{stem}.decompress.log"

    print(f"\n=== Benchmarking {input_name} ===")
    detect_max_read_length(input_path)
    ensure_directory(LOG_DIR)
    ensure_directory(OUTPUT_DIR)
    if output_file.exists():
        output_file.unlink()
    if decompressed_output.exists():
        decompressed_output.unlink()

    print("Running Spring lossless compression (auto-assay)")
    compress_metrics = run_logged_process(
        [
            str(spring),
            "-c",
            "--R1",
            str(input_path),
            "-o",
            str(output_file),
            "-t",
            current_threads(),
            "-q",
            "lossless",
            "--assay",
            "auto",
        ],
        log_path=compress_log,
    )
    actual_assay = get_archive_assay_label(preview, output_file)

    print("Running Spring decompression")
    decompress_metrics = run_logged_process(
        [str(spring), "-d", "-i", str(output_file), "-o", str(decompressed_output)],
        log_path=decompress_log,
    )

    input_size = input_path.stat().st_size
    output_size = output_file.stat().st_size
    reduction = ((input_size - output_size) * 100.0 / input_size) if input_size else 0.0
    bit_perfect = normalized_fastq_match([input_path], [decompressed_output])

    print(f"  Results for {input_name}")
    print(f"    Stored assay:     {actual_assay}")
    print(f"    Compressed size: {output_size:,} bytes")
    print(f"    Reduction:       {reduction:.2f}%")
    print(f"    Bit-perfect:     {'YES' if bit_perfect else 'NO'}")
    print(f"    Compression time:{compress_metrics.elapsed_seconds:9.3f}s")
    print(f"    Decompression time:{decompress_metrics.elapsed_seconds:7.3f}s")


def assay_samples() -> list[dict[str, str | list[str]]]:
    """Return assay-oriented sample groups used by the benchmark suite."""

    return [
        {
            "name": "Bisulfite (test_3)",
            "files": ["test_3_R1.fastq.gz", "test_3_R2.fastq.gz"],
            "expected": "bisulfite",
        },
        {
            "name": "sc-ATAC (test_4)",
            "files": [
                "test_4_R1.fastq.gz",
                "test_4_R2.fastq.gz",
                "test_4_R3.fastq.gz",
                "test_4_I1.fastq.gz",
            ],
            "expected": "sc-atac",
        },
        {
            "name": "sc-RNA (test_5)",
            "files": [
                "test_5_R1.fastq.gz",
                "test_5_R2.fastq.gz",
                "test_5_I1.fastq.gz",
                "test_5_I2.fastq.gz",
            ],
            "expected": "sc-rna",
        },
        {
            "name": "sc-Bisulfite (test_6)",
            "files": ["test_6_R1.fastq.gz", "test_6_R2.fastq.gz"],
            "expected": "sc-bisulfite",
        },
    ]


def lane_flag(name: str) -> str:
    """Map a fixture filename suffix to the corresponding CLI lane flag."""

    token = name.split("_")[-1].split(".")[0]
    return f"--{token}"


def run_assay_suite() -> None:
    """Benchmark auto and DNA assay modes on the curated assay fixtures."""

    spring = spring_bin()
    preview = preview_bin()
    print("\n--- Running Assay Benchmark Suite ---")
    ensure_directory(LOG_DIR)
    ensure_directory(OUTPUT_DIR)
    ensure_directory(TEMP_ROOT)

    for sample in assay_samples():
        files = [INPUT_DIR / name for name in sample["files"]]  # type: ignore[index]
        if not files[0].exists():
            continue
        print(f"\n>>> Assay: {sample['name']}")
        stem = pathlib.Path(files[0].name.removesuffix(".gz")).stem
        auto_archive = OUTPUT_DIR / f"{stem}.auto.sp"
        dna_archive = OUTPUT_DIR / f"{stem}.dna.sp"
        restore_dir = TEMP_ROOT / stem / "restored"
        if restore_dir.parent.exists():
            shutil.rmtree(restore_dir.parent)
        ensure_directory(restore_dir)

        base_args: list[str] = []
        for file_path in files:
            base_args.extend([lane_flag(file_path.name), str(file_path)])

        assay_mode = (
            sample["expected"] if sample["expected"] == "sc-bisulfite" else "auto"
        )
        # Size comparison must be single-threaded: multi-thread OpenMP
        # reorder/encode is nondeterministic and can swing archive size by
        # ~0.5% independent of assay mode (false domain gain/loss).
        compare_threads = "1"
        print(
            f"  Step 1: Compression with --assay {assay_mode} (expected: {sample['expected']})"
        )
        run_logged_process(
            [
                str(spring),
                "-c",
                *base_args,
                "-o",
                str(auto_archive),
                "-t",
                compare_threads,
                "-q",
                "lossless",
                "--assay",
                str(assay_mode),
            ],
            log_path=LOG_DIR / f"{stem}.auto.compress.log",
        )
        size_auto = auto_archive.stat().st_size
        actual_assay = get_archive_assay_label(preview, auto_archive)
        print(f"    Archive metadata assay: {actual_assay}")

        print("  Step 2: Verifying bit-perfect restoration...")
        restored_files = [
            restore_dir / file_path.name.removesuffix(".gz") for file_path in files
        ]
        run_logged_process(
            [
                str(spring),
                "-d",
                "-i",
                str(auto_archive),
                "-o",
                *[str(path) for path in restored_files],
            ],
            log_path=LOG_DIR / f"{stem}.auto.decompress.log",
        )
        bit_perfect = normalized_fastq_match(files, restored_files)
        print(f"    Bit-perfect: {'YES' if bit_perfect else 'NO'}")

        print("  Step 3: Compression with --assay dna")
        run_logged_process(
            [
                str(spring),
                "-c",
                *base_args,
                "-o",
                str(dna_archive),
                "-t",
                compare_threads,
                "-q",
                "lossless",
                "--assay",
                "dna",
            ],
            log_path=LOG_DIR / f"{stem}.dna.compress.log",
        )
        size_dna = dna_archive.stat().st_size
        gain = ((size_dna - size_auto) * 100.0 / size_dna) if size_dna else 0.0

        print("\n  Assay-specific Optimization Results:")
        print(f"    Expected assay:              {sample['expected']}")
        print(f"    Archive metadata assay:      {actual_assay}")
        print(f"    Auto archive size:           {size_auto:,} bytes")
        print(f"    Generic DNA-mode size:       {size_dna:,} bytes")
        print(f"    Optimization Gain:           {gain:.2f}%")
        # Ignore tiny negatives from metadata/tar padding; real regressions
        # are well above this (previously ~6% for broken bisulfite ternary).
        if gain < -0.5:
            print("    Warning: Domain optimization was larger than DNA mode!")
        elif gain < 0:
            print("    Domain optimization NEUTRAL (within noise vs DNA mode)")
        else:
            print("    Domain optimization SUCCESS")


def main() -> int:
    """Run either the assay suite or a single-file benchmark."""

    args = parse_args()
    ensure_spring_binary(spring_bin())
    input_path = pathlib.Path(args.input_fastq)
    if (
        args.input_fastq == str(INPUT_DIR / "sample.fastq")
        and (INPUT_DIR / "test_3_R1.fastq.gz").exists()
    ):
        run_assay_suite()
        return 0
    run_single_file_benchmark(input_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
