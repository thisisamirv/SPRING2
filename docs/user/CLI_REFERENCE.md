# SPRING2 CLI Reference

This page lists the main command-line options by mode.

## General Syntax

Compression:

```bash
spring2 -c --R1 <input_R1> [--R2 <input_R2>] [--R3 <input_R3>] [--I1 <input_I1>] [--I2 <input_I2>] [-o <archive.sp>]
```

Decompression:

```bash
spring2 -d -i <archive.sp> [-o <outputs...>] [--unzip]
```

Preview:

```bash
spring2 --preview -i <archive.sp> [--audit]
```

## General Options

- `-h`, `--help`: show command-line help
- `-V`, `--version`: print version information
- `-o`, `--output <arg>`: output archive or restored output path(s)
- `-t`, `--threads <arg>`: number of worker threads
- `-m`, `--memory <arg>`: approximate memory budget in GB; `0` disables the
  limit
- `-v`, `--verbose [info|debug]`: logging level

## Compression Options

- `-c`, `--compress`: compression mode
- `-R1`, `--R1 <arg>`: required read-1 input
- `-R2`, `--R2 <arg>`: optional read-2 input; enables paired-end mode
- `-R3`, `--R3 <arg>`: optional read-3 input; requires `--R2`
- `-I1`, `--I1 <arg>`: optional index-read-1 input; currently requires `--R2`
- `-I2`, `--I2 <arg>`: optional index-read-2 input; requires `--I1`
- `-l`, `--level <arg>`: compression level `1..9`
- `-s`, `--strip <arg>`: discard one or more of `i`, `o`, `q`
- `-q`, `--qmod ...`: quality storage mode
- `-n`, `--note <arg>`: store a custom note in archive metadata
- `-y`, `--assay <arg>`: select assay mode
- `-b`, `--cb-len <arg>`: cellular barcode length for eligible single-cell
  assays
- `-a`, `--audit`: run post-compression verification

## Decompression Options

- `-d`, `--decompress`: decompression mode
- `-i`, `--input <arg>`: input archive path
- `-a`, `--audit`: run a full in-memory archive integrity audit before writing
  outputs
- `-u`, `--unzip`: force plain-text output even if the original input was
  gzipped

## Preview Options

- `-p`, `--preview`: inspect archive metadata without writing outputs
- `-i`, `--input <arg>`: input archive path
- `-a`, `--audit`: perform full in-memory archive integrity audit

## Quality Mode Forms

- `-q lossless`
- `-q ill_bin`
- `-q binary <thr> <high> <low>`

## Assay Choices

- `auto`
- `dna`
- `rna`
- `atac`
- `bisulfite`
- `sc-rna`
- `sc-atac`
- `sc-bisulfite`

## Output Naming Notes

- For grouped archives, `-o prefix` expands to lane-specific suffixes such as
  `.R1`, `.R2`, `.R3`, `.I1`, and `.I2`.
- For paired-end decompression, multiple output names can be passed explicitly.
- Generated defaults and prefixes append `.gz` when the original stream was
  gzipped (unless `--unzip` is set). Explicit paths ending in `.gz` write
  gzipped restored output.

## See Also

- [Compression Guide](COMPRESSION.md)
- [Decompression Guide](DECOMPRESSION.md)
- [Archive Inspection](ARCHIVE_INSPECTION.md)
