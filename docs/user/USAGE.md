# SPRING2 Usage Guide

This page is the entry point for using SPRING2. The detailed task guides are
split into smaller documents so common workflows are easier to find.

## Quick Start

Compress paired-end FASTQ:

```bash
spring2 -c --R1 sample_1.fastq --R2 sample_2.fastq -o sample.sp
```

Decompress an archive:

```bash
spring2 -d -i sample.sp -o restored_1.fastq restored_2.fastq
```

Preview archive metadata without extracting:

```bash
spring2 --preview -i sample.sp
```

Audit an archive without writing decompressed FASTQ to disk:

```bash
spring2 --preview --audit -i sample.sp
```

## Common Workflows

- [Compression Guide](COMPRESSION.md): creating archives from FASTQ or FASTA,
  grouped lanes, gzip input handling, performance knobs, and notes.
- [End-to-End Examples](EXAMPLES.md): complete command sequences for DNA,
  RNA, ATAC, bisulfite, and single-cell workflows.
- [Decompression Guide](DECOMPRESSION.md): restoring archives, choosing output
  names, gzip restoration rules, grouped outputs, and `--unzip`.
- [Archive Inspection](ARCHIVE_INSPECTION.md): preview mode, audit mode,
  stored metadata, and gzip/BGZF reporting.
- [Assays and Quality Modes](ASSAYS_AND_QUALITY.md): assay selection,
  barcode-related behavior, lossy quality modes, and `--strip`.
- [CLI Reference](CLI_REFERENCE.md): complete flag-by-flag option list.

## Input Layouts

SPRING2 supports these main input patterns:

- Single-end FASTQ or FASTA
- Paired-end FASTQ or FASTA with `--R1` and `--R2`
- Grouped archives using optional `--R3`, `--I1`, and `--I2`
- Plain-text and `.gz` inputs

When grouped lanes are present, a single archive can preserve the read pair,
read-3 lane, and index lanes together.

## Output Behavior Summary

- Compression writes `.sp` archives.
- Decompression uses the original input filenames when possible if you do not
  specify `--output`.
- For paired-end or grouped decompression, `-o prefix` expands to `prefix.R1`
  and `prefix.R2`, plus optional `prefix.R3`, `prefix.I1`, and `prefix.I2` when
  those lanes are present.

## Logging and Verification

- Default logging uses a concise progress display plus warnings and errors.
- `--verbose` or `--verbose info` prints step-by-step informational logs.
- `--verbose debug` enables detailed troubleshooting diagnostics.
- Standard decompression already verifies stored record digests.
- `--audit` adds a verification pass after compression, or performs a full
  dry-run integrity audit when used with `--preview`.

## Related Documentation

- [Building Overview](BUILDING.md)
- [End-to-End Examples](EXAMPLES.md)
- [User Docs Index](README.md)
