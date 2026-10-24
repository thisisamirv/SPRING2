#!/usr/bin/env python3
"""Mint frozen SPRING2 archives for backward-compat decompression tests.

Writes tests/data/compat/<VERSION>/*.sp using the provided spring2 binary at
-t 1, then upserts the corresponding rows in tests/data/compat/manifest.tsv.

Historical compat/<old_version>/ trees are never deleted or overwritten for a
different VERSION. Re-running for the same VERSION replaces that version's
archives and manifest rows only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_DIR = REPO_ROOT / "tests" / "data"
COMPAT_DIR = DATA_DIR / "compat"
MANIFEST_PATH = COMPAT_DIR / "manifest.tsv"

MANIFEST_HEADER = (
    "id\tkind\tarchive\texpected_version\tref_r1\tref_r2\trefs_gzipped\tchecks\n"
)

# Small checked-in inputs only — keep the golden set cheap for CI.
SPRING2_CASES = (
    {
        "suffix": "se_fastq",
        "r1": "test_1.fastq",
        "r2": None,
        "checks": "decompress,preview,audit,reader",
    },
    {
        "suffix": "pe_fastq",
        "r1": "test_1.fastq",
        "r2": "test_2.fastq",
        "checks": "decompress,preview,audit",
    },
    {
        "suffix": "se_fasta",
        "r1": "test_1.fasta",
        "r2": None,
        "checks": "decompress,preview,audit",
    },
)

LEGACY_ROWS = (
    "legacy_spring_sample\tlegacy_spring\tcompat/legacy/sample.spring\t"
    "legacy spring\tsample.fastq\t\t0\tdecompress,preview\n",
    "legacy_spring_se_fastq\tlegacy_spring\tcompat/legacy/se_fastq.spring\t"
    "legacy spring\ttest_1.fastq\t\t0\tdecompress,preview,reader\n",
    "legacy_spring_se_fasta\tlegacy_spring\tcompat/legacy/se_fasta.spring\t"
    "legacy spring\ttest_1.fasta\t\t0\tdecompress,preview\n",
    "legacy_spring_pe_fastq\tlegacy_spring\tcompat/legacy/pe_fastq.spring\t"
    "legacy spring\ttest_3_R1.fastq.gz\ttest_3_R2.fastq.gz\t1\tdecompress,reader\n",
)


def run(cmd: list[str]) -> str:
    completed = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    )
    return (completed.stdout or "") + (completed.stderr or "")


def detect_version(spring2: Path) -> str:
    output = run([str(spring2), "-V"])
    match = re.search(r"(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)", output)
    if not match:
        raise RuntimeError(f"Could not parse version from: {output!r}")
    return match.group(1)


def load_manifest_rows() -> list[str]:
    if not MANIFEST_PATH.exists():
        return []
    lines = MANIFEST_PATH.read_text(encoding="utf-8").splitlines()
    if not lines:
        return []
    # Drop header if present.
    if lines[0].startswith("id\t"):
        lines = lines[1:]
    return [line + "\n" for line in lines if line.strip() and not line.startswith("#")]


def write_manifest(rows: list[str]) -> None:
    COMPAT_DIR.mkdir(parents=True, exist_ok=True)
    # Stable order: legacy rows first (by id), then spring2 rows by id.
    legacy = [r for r in rows if "\tlegacy_spring\t" in r]
    spring2 = [r for r in rows if "\tspring2\t" in r]
    other = [r for r in rows if r not in legacy and r not in spring2]
    legacy.sort()
    spring2.sort()
    other.sort()
    MANIFEST_PATH.write_text(
        MANIFEST_HEADER + "".join(legacy + spring2 + other), encoding="utf-8"
    )


def upsert_rows(existing: list[str], new_rows: list[str], version: str) -> list[str]:
    drop_prefix = f"spring2_{version.replace('.', '_').replace('-', '_')}_"
    kept = [
        row
        for row in existing
        if not (row.startswith("spring2_") and f"compat/{version}/" in row)
        and not row.startswith(drop_prefix)
    ]
    # Also drop any previous rows for this version by archive path.
    kept = [row for row in kept if f"\tcompat/{version}/" not in row]
    return kept + new_rows


def ensure_legacy_rows(rows: list[str]) -> list[str]:
    by_id = {row.split("\t", 1)[0]: row for row in rows}
    for legacy in LEGACY_ROWS:
        by_id[legacy.split("\t", 1)[0]] = legacy
    return list(by_id.values())


def mint(spring2: Path, version: str) -> None:
    out_dir = COMPAT_DIR / version
    out_dir.mkdir(parents=True, exist_ok=True)
    new_rows: list[str] = []

    for case in SPRING2_CASES:
        archive_rel = f"compat/{version}/{case['suffix']}.sp"
        archive_path = DATA_DIR / archive_rel
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        if archive_path.exists():
            archive_path.unlink()

        cmd = [
            str(spring2),
            "-c",
            "--R1",
            str(DATA_DIR / case["r1"]),
            "-o",
            str(archive_path),
            "-t",
            "1",
        ]
        if case["r2"] is not None:
            cmd.extend(["--R2", str(DATA_DIR / case["r2"])])
        print("mint:", " ".join(cmd))
        run(cmd)
        if not archive_path.exists() or archive_path.stat().st_size == 0:
            raise RuntimeError(f"Mint failed to produce {archive_path}")

        fixture_id = (
            f"spring2_{version.replace('.', '_').replace('-', '_')}_{case['suffix']}"
        )
        ref_r2 = case["r2"] or ""
        new_rows.append(
            f"{fixture_id}\tspring2\t{archive_rel}\t{version}\t"
            f"{case['r1']}\t{ref_r2}\t0\t{case['checks']}\n"
        )

    rows = ensure_legacy_rows(load_manifest_rows())
    rows = upsert_rows(rows, new_rows, version)
    write_manifest(rows)
    print(f"Updated {MANIFEST_PATH} with {len(new_rows)} spring2 rows for {version}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--spring2",
        type=Path,
        required=True,
        help="Path to the spring2 binary that will mint the goldens",
    )
    parser.add_argument(
        "--version",
        default=None,
        help="Override creator version directory name (default: spring2 -V)",
    )
    args = parser.parse_args()
    spring2 = args.spring2.resolve()
    if not spring2.exists():
        print(f"spring2 not found: {spring2}", file=sys.stderr)
        return 1
    version = args.version or detect_version(spring2)
    mint(spring2, version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
