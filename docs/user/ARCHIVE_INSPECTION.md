# SPRING2 Archive Inspection

SPRING2 provides a preview mode for inspecting archive metadata without writing reconstructed FASTQ or FASTA files to disk.

## Preview Mode

Use preview mode with exactly one archive input:

```bash
spring2 --preview -i archive.sp
```

Preview mode reports metadata such as:

- original input names
- custom note
- assay type and detection confidence
- compression ratio
- read counts and maximum read length
- preservation flags for order, IDs, and quality
- quality mode
- compression level
- gzip or BGZF metadata for original compressed inputs

For grouped archives, preview also reports additional lanes such as `R3`, `I1`, and `I2`.

## Audit Mode

Preview mode can run a full archive audit without writing restored FASTQ files:

```bash
spring2 --preview --audit -i archive.sp
```

This verifies the container `integrity.meta` SHA-256 digests (when present), then reconstructs records in memory and checks stored CRC32 digests. It is useful for checking archive health after transfer or long-term storage.

## What Metadata Can Indicate

Preview output may also show assay-aware transforms that were applied during compression, including:

- extracted cellular barcode prefixes
- grouped sc-RNA index ID reconstruction
- ATAC adapter stripping

These are restoration-aware transforms: the archive stores enough information to reconstruct original records for lossless archives.

## Gzip and BGZF Reporting

When the original input stream was gzipped, preview can display details like:

- gzip header information
- original embedded filename
- member count
- compressed and uncompressed sizes
- BGZF detection and block size

This is especially useful when validating archives created from pipeline tools that emit BGZF-style gzip members.

## Notes

Archive notes added with `--note` are shown in preview output. This is a good place to record batch IDs, sample provenance, or workflow labels.

## Archive Format Caveat

`.sp` archives are technically tar containers with compressed internal members, but they should be inspected with SPRING2 rather than generic tar tooling when you care about semantic metadata and integrity reporting.

## See Also

- [Decompression Guide](DECOMPRESSION.md)
- [CLI Reference](CLI_REFERENCE.md)
