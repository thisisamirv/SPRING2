# SPRING2 Compression Guide

This guide focuses on archive creation.

## Basic Syntax

```bash
spring2 -c --R1 <input_R1> [--R2 <input_R2>] [--R3 <input_R3>] \
  [--I1 <input_I1>] [--I2 <input_I2>] -o <output.sp>
```

## Supported Compression Inputs

- Single-end FASTQ
- Paired-end FASTQ
- FASTA inputs
- Gzipped inputs ending in `.gz`
- Grouped lane sets with optional read-3 and index lanes

SPRING2 auto-detects FASTQ versus FASTA and auto-detects gzipped input files.

## Common Examples

Single-end:

```bash
spring2 -c --R1 reads.fastq -o reads.sp
```

Paired-end:

```bash
spring2 -c --R1 reads_1.fastq --R2 reads_2.fastq -o reads.sp
```

Gzipped paired-end:

```bash
spring2 -c --R1 reads_1.fastq.gz --R2 reads_2.fastq.gz -o reads.sp
```

Grouped paired-end with indexes:

```bash
spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
```

Grouped paired-end with read-3 and indexes:

```bash
spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --R3 R3.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
```

## Grouped Lanes

Grouped archives are created when you pass `--R3` and/or `--I1` / `--I2`.

Rules:

- `--R3` requires `--R2`
- `--I1` currently requires `--R2`
- `--I2` requires `--I1`

Grouped archives preserve all provided lanes together and restore them with a
single decompression command.

When `--R3` points at the same file as `--R1` or `--R2`, SPRING2 stores R3 as an
alias of that mate (no extra payload) and records the mate's digests in
`bundle.meta` so audit/decompress can reject a flipped alias label.

## Performance Controls

### Threads

Use `--threads` to control concurrency:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --threads 8
```

The default thread count is `min(max(1, hw_threads - 1), 16)`.

Single-thread compression (`-t 1`) is bit-deterministic: two runs on the same
input produce identical `.sp` archives. Multi-thread reordering can skip
contested dictionary buckets for throughput, so archive bytes may differ across
runs even though decompression remains lossless. Set
`SPRING_DETERMINISTIC_REORDER=1` to serialize match selection to one thread and
get the same archive bytes as `-t 1` (slower than the default parallel path).

### Memory Budget

Use `--memory` to cap effective worker concurrency based on an approximate
memory budget:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --memory 8
```

Set `--memory 0` to disable this safety knob.

### Adaptive Memory and Disk Routing

SPRING2 selects a compression route at runtime based on the available memory
and disk budget:

- **Memory path**: when the estimated peak working set fits within the budget,
  all intermediates are kept in RAM. This is the fastest route.
- **Disk path**: when memory is tight, preprocessing intermediates are spilled
  to a temporary workspace on disk (beside the output archive by default as
  `<archive>.work-tmp`, or `<archive>.grouped-tmp` for grouped bundles).
  Within the disk path, SPRING2 further adapts:
  - **External-MPHF mode**: if the work directory has sufficient
    free space (roughly 40 bytes × read count), the hash-function builder
    offloads its sort structures to disk, reducing peak RAM by several GB.
  - **Thread-capping mode**: if disk space is also limited,
    SPRING2 automatically reduces the encoding thread count so that per-thread
    buffer overhead stays within the available memory budget.
  - **Early clean-read stream release**: raw clean-read byte streams are freed
    immediately after the bitset array is decoded during reordering, rather
    than held live throughout MPHF construction and read reordering, eliminating
    a multi-GB intermediate peak.
  - **Singleton streaming**: singleton and N-read raw byte buffers are streamed
    from disk at encode time rather than loaded into RAM before decoding,
    eliminating a further multi-GB peak for datasets with many singletons.
  - **Aligned-shard streaming**: aligned read shards are streamed from disk
    per encoder thread rather than loaded into RAM before encoding begins,
    eliminating a multi-GB shard-buffer peak.
  - **Metadata spill**: per-thread encoder metadata (positions, orientations,
    read orders, noise) is written incrementally to disk during encoding and
    merged after the encoding loop, rather than accumulating in RAM until the
    loop completes.
  - **Stream scatter buffering**: during the "Reordering and compressing
    streams" stage, scatter records are accumulated in per-block RAM buffers
    and flushed to scratch files only in large batched appends when the
    aggregate buffer size exceeds 40 % of the memory budget (default cap
    4 GiB), rather than opening, appending, and closing a scratch file per
    read. When the entire scatter fits under the cap, scratch files are skipped
    entirely and the buffers are handed directly to the parallel block rebuild.

In practice, passing `--memory 8` on a machine with 8 GB free lets SPRING2
pick the right route automatically. Lower values increase the chance of
triggering disk-path routing; very low values also trigger thread capping.

#### Advanced routing overrides

Keep the default `auto` planner unless you need an explicit override:

```bash
# Force in-memory intermediates even when the planner would pick disk
spring2 -c --R1 reads.fastq -o reads.sp -r memory

# Force disk-backed intermediates even when RAM looks sufficient
spring2 -c --R1 reads.fastq -o reads.sp --compression-route disk

# Put disk-path workspaces on a RAM-backed filesystem (e.g. /dev/shm)
spring2 -c --R1 reads.fastq -o /data/reads.sp -r disk -w /dev/shm
```

`-w` / `--tmp-dir` only affects disk-path workspaces (`.work-tmp` /
`.grouped-tmp`). It is ignored when `-r memory` / `--compression-route memory`
is selected.

### Compression Level

Use `--level 1..9` to change the archive compression level used for gzip-style
formatting and the internal scaling used for other streams:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --level 9
```

## Notes and Metadata

Attach a custom note with `--note`:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --note "batch=07 replicate=A"
```

That note is stored in archive metadata and shown by preview mode.

## Container Integrity

Every new `.sp` archive includes an `integrity.meta` member listing the SHA-256
of each packed payload (plus a root hash of that sorted digest list). Grouped
bundles hash the outer members the same way; nested lane archives carry their
own manifests. Decompress and audit verify these digests automatically.

## Compression-Time Verification

Add `--audit` to verify the archive immediately after compression:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --audit
```

This runs a dry-run verification pass after archive creation (container
SHA-256 plus reconstructed record digests).

## Logging

- Default: progress bar plus warnings and errors
- `--verbose` or `--verbose info`: informational logs
- `--verbose debug`: detailed diagnostics

Example:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --verbose debug
```

## See Also

- [Assays and Quality Modes](ASSAYS_AND_QUALITY.md)
- [CLI Reference](CLI_REFERENCE.md)
