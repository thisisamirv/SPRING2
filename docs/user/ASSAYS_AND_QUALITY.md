# SPRING2 Assays and Quality Modes

This guide explains the higher-level data behaviors that affect how SPRING2
stores and restores reads.

## Assay Selection

Use `--assay` / `-y` to choose an assay explicitly:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --assay atac
```

Supported values are:

- `auto`
- `dna`
- `rna`
- `atac`
- `bisulfite`
- `sc-rna`
- `sc-atac`
- `sc-bisulfite`

`auto` is the default. During compression, SPRING2 samples the startup portion
of the input and classifies the dataset using assay-specific heuristics and the
bundled assay reference.

For technical background on the assay reference, see
[../assays/ASSAY_REFERENCE.md](../assays/ASSAY_REFERENCE.md).

## Cellular Barcode Length

Use `--cb-len` when single-cell assays store cellular barcodes in the start of
R1 instead of a dedicated I1 lane:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --assay sc-rna --cb-len 16
```

Notes:

- Valid range is `1..64`.
- When `--I1` is present, barcode length is inferred from the I1 read length
  and `--cb-len` is ignored.

## Assay-Aware Compression Behaviors

Depending on assay and confidence, SPRING2 can enable internal transforms that
remain reversible for lossless archives.

Examples include:

- RNA and scRNA: poly-A/T tail stripping when assay detection strongly suggests
  it is safe
- ATAC and scATAC: terminal adapter read-through stripping when the overlap is
  strong enough
- scRNA and sc-bisulfite: cellular barcode prefix extraction from R1 when no I1
  lane is used
- grouped scRNA with index lanes: trailing index tokens in IDs can be
  reconstructed from decoded I1/I2 reads

These behaviors are recorded in archive metadata and surfaced by preview mode.

## Quality Modes

Use `--qmod` / `-q` to choose how quality values are stored.

### Lossless

Default mode:

```bash
spring2 -c --R1 reads.fastq -o reads.sp -q lossless
```

This preserves qualities bit-for-bit.

### Illumina 8-level Binning

```bash
spring2 -c --R1 reads.fastq -o reads.sp -q ill_bin
```

Lossy: quality digests are not stored. Decompress/audit still verify sequence
(and ids when preserved).

### Binary Thresholding

```bash
spring2 -c --R1 reads.fastq -o reads.sp -q binary 20 30 10
```

This maps qualities to `high` when `>= threshold` and to `low` otherwise.
Same integrity rules as Illumina binning: no quality digest; sequence/id
checks still apply.

## `--strip` Modes

Use `--strip` / `-s` to intentionally discard archive components:

- `i`: identifiers
- `o`: original order
- `q`: quality scores

Example:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --strip io
```

Be careful: stripping changes what can be faithfully restored later.

## When to Prefer Explicit Assay Selection

Prefer `--assay` over `auto` when:

- you already know the assay and want deterministic behavior
- startup samples are atypical or low quality
- you are benchmarking and want consistent cross-run settings

## See Also

- [End-to-End Examples](EXAMPLES.md)
- [Compression Guide](COMPRESSION.md)
- [Archive Inspection](ARCHIVE_INSPECTION.md)
