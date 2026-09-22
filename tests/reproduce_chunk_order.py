#!/usr/bin/env python3
"""Exercise SPRING2 paired-end chunk boundaries with distinguishable records.

Usage: python tests/reproduce_chunk_order.py /path/to/spring2 /new/results/dir
Optional --expect-pass makes a failing round trip return a nonzero exit status.
Results include commands, logs, SHA-256, per-field comparisons and sequence origins.
Only Python's standard library is required. Existing result dirs are rejected.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess


def fixture(kind, mate, n=200):
    rng = random.Random(9382 + mate)
    template = "".join(random.Random(751).choices("ACGT", k=150))
    records = []
    for i in range(n):
        seq = "".join(rng.choices("ACGT", k=150))
        if kind == "aligned":
            tag = i + (mate - 1) * n
            seq = "".join("ACGT"[(tag >> (2 * j)) & 3] for j in range(8)) + template[8:]
        if kind == "uneven_n" and i % (13 if mate == 1 else 7) == 0:
            seq = seq[:37] + "N" + seq[38:]
        if kind == "no_clean_r2" and mate == 2:
            seq = seq[:37] + "N" + seq[38:]
        qual = "".join(chr(53 + (i * 3 + j + mate * 5) % 21) for j in range(150))
        records.append((f"@pair{i + 1:06}/{mate}", seq, "+", qual))
    return records


def fastq_bytes(records):
    return ("\n".join(line for record in records for line in record) + "\n").encode()


def read_records(data):
    lines = data.decode().splitlines()
    if len(lines) % 4:
        raise ValueError("Incomplete FASTQ output record")
    return [tuple(lines[i:i + 4]) for i in range(0, len(lines), 4)]


def run(binary, root, case):
    name, kind, pe, chunk, memory, threads, gz = case
    work = root / name
    work.mkdir()
    expected = [fixture(kind, 1)] + ([fixture(kind, 2)] if pe else [])
    paths = []
    for mate, records in enumerate(expected, 1):
        path = work / f"in_R{mate}.fastq{'.gz' if gz else ''}"
        data = fastq_bytes(records)
        path.write_bytes(gzip.compress(data, mtime=0) if gz else data)
        paths.append(path)
    archive = work / "archive.sp"
    env = dict(os.environ, SPRING2_REORDER_CHUNK_SIZE=str(chunk))
    command = [str(binary), "-c", "--R1", str(paths[0])]
    if pe:
        command += ["--R2", str(paths[1])]
    command += ["-o", str(archive), "-t", str(threads), "-m", str(memory),
                "--assay", "dna", "-q", "lossless", "--audit", "-v", "info"]
    with (work / "compress.log").open("wb") as log:
        comp = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    out_paths = [work / f"out_R{mate}.fastq" for mate in range(1, len(paths) + 1)]
    dec_command = [str(binary), "-d", "-u", "-i", str(archive), "-o", *map(str, out_paths), "-t", str(threads)]
    with (work / "decompress.log").open("wb") as log:
        dec = subprocess.run(dec_command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    origin = {r[1]: f"R{mate}:{i}" for mate, records in enumerate(expected, 1)
              for i, r in enumerate(records, 1)}
    outputs, comparisons = [], []
    for mate, (records, path) in enumerate(zip(expected, out_paths), 1):
        original = fastq_bytes(records)
        actual = path.read_bytes() if path.exists() else b""
        decoded = read_records(actual)
        outputs += decoded
        first = next((i for i, (a, b) in enumerate(zip(records, decoded), 1) if a[1] != b[1]), None)
        comparisons.append({
            "mate": mate, "record_count": len(decoded), "bytes_equal": actual == original,
            "input_sha256": hashlib.sha256(original).hexdigest(),
            "output_sha256": hashlib.sha256(actual).hexdigest(),
            "ids_equal": [r[0] for r in records] == [r[0] for r in decoded],
            "sequences_equal": [r[1] for r in records] == [r[1] for r in decoded],
            "qualities_equal": [r[3] for r in records] == [r[3] for r in decoded],
            "first_sequence_mismatch_1based": first,
            "first_mismatch_sequence_origin": origin.get(decoded[first - 1][1]) if first else None,
            "first_60_sequence_origins": [origin.get(r[1]) for r in decoded[:60]],
        })
    log_text = (work / "compress.log").read_text()
    result = {
        "name": name, "fixture": kind, "paired_end": pe, "chunk_size": chunk,
        "memory_arg": memory, "threads": threads, "gz_input": gz,
        "compress_command": command, "decompress_command": dec_command,
        "compress_returncode": comp.returncode, "decompress_returncode": dec.returncode,
        "audit_success": "Audit successful:" in log_text,
        "disk_path": "Disk-backed compression path selected" in log_text,
        "multi_chunk_log": "Reorder chunk 2" in log_text,
        "all_sequences_preserved_as_multiset": Counter(r[1] for rows in expected for r in rows) == Counter(r[1] for r in outputs),
        "comparisons": comparisons,
    }
    result["passed"] = comp.returncode == dec.returncode == 0 and result["audit_success"] and all(c["bytes_equal"] for c in comparisons)
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"name": name, "pass": result["passed"], "audit": result["audit_success"],
                      "comp_rc": comp.returncode, "dec_rc": dec.returncode,
                      "first_mismatch": [(c["first_sequence_mismatch_1based"], c["first_mismatch_sequence_origin"]) for c in comparisons]}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("--expect-pass", action="store_true")
    args = parser.parse_args()
    root = args.results.resolve()
    root.mkdir(parents=True, exist_ok=False)
    # name, fixture, paired-end, chunk size, memory budget, threads, gzip input
    cases = [
        ("se_memory", "random", False, 100, 1024, 1, False),
        ("se_disk", "random", False, 100, 0.00001, 1, False),
        ("pe_one_chunk", "random", True, 400, 1024, 1, False),
        ("pe_just_below_total", "random", True, 399, 1024, 1, False),
        ("pe_memory_t1", "random", True, 100, 1024, 1, False),
        ("pe_memory_t4", "random", True, 100, 1024, 4, False),
        ("pe_disk", "random", True, 100, 0.00001, 1, False),
        ("pe_odd_chunk", "random", True, 101, 1024, 1, False),
        ("pe_aligned_memory", "aligned", True, 100, 1024, 1, False),
        ("pe_aligned_disk", "aligned", True, 100, 0.00001, 1, False),
        ("pe_uneven_n_memory", "uneven_n", True, 101, 1024, 1, False),
        ("pe_uneven_n_disk", "uneven_n", True, 101, 0.00001, 1, False),
        ("pe_no_clean_r2", "no_clean_r2", True, 100, 1024, 1, False),
        ("pe_gzip", "random", True, 100, 1024, 1, True),
    ]
    results = [run(args.binary.resolve(), root, case) for case in cases]
    (root / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    passed = sum(r["passed"] for r in results)
    print(f"{passed}/{len(results)} round trips passed", flush=True)
    return int(args.expect_pass and passed != len(results))


if __name__ == "__main__":
    raise SystemExit(main())
