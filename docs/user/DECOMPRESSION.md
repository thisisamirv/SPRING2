# SPRING2 Decompression Guide

This guide focuses on restoring `.sp` archives.

## Basic Syntax

```bash
spring2 -d -i <archive.sp> [-o <outputs...>]
```

Decompression uses `--input` / `-i` and does not use `--R1`, `--R2`, `--R3`,
`--I1`, or `--I2`.

## Basic Examples

Restore a single-end archive:

```bash
spring2 -d -i reads.sp -o restored.fastq
```

Restore a paired-end archive:

```bash
spring2 -d -i reads.sp -o restored_1.fastq restored_2.fastq
```

If you omit `--output`, SPRING2 uses the original input names stored in the
archive metadata when possible.

## Grouped Archive Output Naming

Grouped archives are restored with one command.

If you provide a single prefix:

```bash
spring2 -d -i run.sp -o run_out
```

SPRING2 expands that to:

- `run_out.R1`
- `run_out.R2`
- `run_out.R3` when a read-3 lane exists
- `run_out.I1` when an index lane exists
- `run_out.I2` when a second index lane exists

Example:

```bash
spring2 -d -i run.sp -o run_out
```

## Gzip Restoration Rules

SPRING2 stores input gzip metadata and uses it during decompression.

Behavior summary:

- If you omit `--output`, SPRING2 prefers the original basenames stored in the
  archive. When a stream was gzipped, generated names keep or regain a `.gz`
  suffix so the restored file is gzipped (matching grouped-archive defaults).
- If you give a single paired-end prefix (`-o out`), SPRING2 expands to
  `out.1` / `out.2` (or grouped `.R1` / `.R2` / …) and appends `.gz` per stream
  that was originally gzipped.
- If you explicitly give full output names, a `.gz` suffix controls whether that
  file is written gzipped; names without `.gz` stay plain.
- Preview mode reports stored gzip and BGZF metadata for each stream.

Examples:

```bash
# Defaults restore original names, including .gz when the inputs were gzipped
spring2 -d -i reads.sp

# Explicit gzipped outputs
spring2 -d -i reads.sp -o restored_1.fastq.gz restored_2.fastq.gz
```

## Force Plain Output

Use `--unzip` / `-u` to force plain-text FASTQ or FASTA output even when the
original input was gzipped. Generated default/prefix names also drop `.gz`:

```bash
spring2 -d -u -i reads.sp
spring2 -d -u -i reads.sp -o restored.fastq
```

## Integrity Verification

Normal decompression already verifies stored CRC32 digests for:

- sequences
- quality values
- identifiers

If verification fails, decompression aborts with an integrity error.

## Logging

Use the same logging levels available during compression:

```bash
spring2 -d -i reads.sp --verbose info
spring2 -d -i reads.sp --verbose debug
```

## See Also

- [Archive Inspection](ARCHIVE_INSPECTION.md)
- [CLI Reference](CLI_REFERENCE.md)
