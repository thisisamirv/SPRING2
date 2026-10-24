<!-- markdownlint-disable MD033 MD034 -->
# SPRING2

<p align="center">
  <a href="https://github.com/thisisamirv/SPRING2/actions/workflows/ci.yml">
    <img src="https://github.com/thisisamirv/SPRING2/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

<p align="center">
  <img src="docs/assets/icons/logo.png" alt="SPRING2 logo" width="128" />
</p>

SPRING2 is a compressor for FASTQ and FASTA sequencing data, including paired-end data and gzipped FASTQ inputs. It is built on top of the original **[SPRING](https://github.com/shubhamchandak94/SPRING)** project and represents a substantial modernization of that codebase.

> [!TIP]
> SPRING2 substantially modernizes the original SPRING codebase. It upgrades the build system to C++20 and CMake 3.31+, removes the remaining Boost dependency, adds first-class cross-platform support for Linux, macOS, and native Windows builds, and expands the runtime to cover grouped lane archives, assay-aware compression behavior, preview and audit workflows, richer gzip metadata preservation, and cleaner developer and benchmarking tooling.

## Features

- Near-optimal compression for single-end, paired-end, and grouped multi-lane sequencing datasets
- Lossless preservation of reads, quality scores, and identifiers, with reversible assay-aware transforms where applicable
- Optional lossy quality compression using [Illumina 8-level binning](https://www.illumina.com/documents/products/whitepapers/whitepaper_datacompression.pdf) or binary thresholding
- Automatic FASTQ versus FASTA detection, plain-text versus gzipped input handling, and gzip-aware output restoration
- Support for grouped archives with `R1`/`R2` plus optional `R3`, `I1`, and `I2` lanes
- Automatic and explicit assay handling for `dna`, `rna`, `atac`, `bisulfite`, `sc-rna`, `sc-atac`, and `sc-bisulfite`
- Adaptive memory and disk routing: selects in-memory, disk-backed with external MPHF, or thread-capped mode automatically based on available memory and disk budget
- Preview mode for archive inspection and audit mode for dry-run integrity verification without full decompression
- Record-level CRC32 verification for lossless archives during decompression and optional post-compression auditing
- Cross-platform builds and CI coverage for Linux, macOS, and native Windows toolchains
- Free for non-profit research and educational use

## Platform Support

SPRING2 is built and validated on:

- **Linux**: x86_64, ARM64
- **macOS**: Universal (Intel/Apple Silicon)
- **Windows**: Native via MinGW-w64, Native ARM64 via MSVC

## Documentation

> [!NOTE]
> 📚 Published documentation, with tips, examples, and full reference is available at:
> <https://spring2.readthedocs.io/>

## Installation

### Conda (bioconda)

If you are using conda environments, install SPRING2 with:

```bash
conda install -c bioconda spring2
```

You can then verify the install:

```bash
spring2 --version
```

### Pre-built Binaries

The easiest way to use SPRING2 is to download pre-built artifacts from the **[Releases](https://github.com/thisisamirv/SPRING2/releases)** page.

Current release artifacts include:

- Linux AppImages (`spring2-linux-*.AppImage`)
- macOS `.app` bundles packaged as zip archives (`spring2-macos-*.app.zip`)
- Windows standalone executables (`spring2-windows-*.exe`)

For platform-specific installation steps, see [docs/user/INSTALLING.md](docs/user/INSTALLING.md).

## Running SPRING

Current command-line help:

```text
Allowed options:

* General Options:
  -h [ --help ]                   produce help message
  -V [ --version ]                produce version information
  -o [ --output ] arg             output file name
                                    - if not specified, it uses original input
                                      filenames (swapping extension to .sp during
                                      compression)
                                    - decompress with one -o <prefix>:
                                      paired-end writes <prefix>.R1 / .R2;
                                      grouped also adds optional .R3 / .I1 / .I2
  -t [ --threads ] arg            number of threads (default:
                                  min(max(1, hw_threads - 1), 16))
  -m [ --memory ] arg (=0)        approximate memory budget in GB; reduces
                                  effective thread count using about 1 GB per
                                  worker thread (0 disables)
  -v [ --verbose ]                enable extensive logging (default: progress bar)
--------------------------------------------------------------------------------
* Compression Options:
  -c [ --compress ]               compress
  -R1 [ --R1 ] arg                input read-1 file (required)
  -R2 [ --R2 ] arg                input read-2 file (optional; enables paired-end mode)
  -R3 [ --R3 ] arg                input read-3 file (optional; requires --R2)
  -I1 [ --I1 ] arg                input index-read-1 file (optional; requires --R2)
  -I2 [ --I2 ] arg                input index-read-2 file (optional; requires --I1)
  -l [ --level ] arg (=6)         compression level (1-9) to use for output
                                  (.gz) formatting (passed to gzip unchanged
                                  and scaled to Zstd 1-22 internally)
  -s [ --strip ] arg              discard data: i (ids), o (order), q (quality)
                                  Example: --strip io to drop ids and order.
  -q [ --qmod ] arg               quality mode: possible modes are
                                    1. -q lossless (default)
                                    2. -q ill_bin (Illumina 8-level binning)
                                    3. -q binary thr high low (binary (2-level)
                                      thresholding, quality binned to high if >=
                                      thr and to low if < thr)
  -n [ --note ] arg               add a custom note to the archive
  -y [ --assay ] arg (=auto)      specify assay type. Valid choices:
                                  auto, dna, rna, atac, bisulfite,
                                  sc-rna, sc-atac, sc-bisulfite
  -a [ --audit ]                  enable post-operation integrity verification
--------------------------------------------------------------------------------
* Decompression Options:
  -d [ --decompress ]             decompress
  -i [ --input ] arg              input archive file (.sp)
  -u [ --unzip ]                  during decompression, force output to be
                                  uncompressed (even if original was .gz)

Decompression with a single `-o <prefix>` expands to `prefix.R1` / `prefix.R2`
for paired-end archives. Grouped archives (`--R3` and/or `--I1/--I2`) also add
optional `.R3`, `.I1`, and `.I2` for present lanes.
```

## Previewing Archives

You can use `spring2 --preview` to quickly inspect archive metadata, original filenames, and custom notes. It also provides a high-speed **Audit** mode to verify archive integrity without full decompression:

```bash
# Display metadata preview
spring2 --preview archive.sp

# Perform a high-speed integrity audit (dry-run)
spring2 --preview --audit archive.sp
```

Example:

```text
SPRING2 Archive Metadata Preview:
--------------------------------
Archive Version:   1.1.0
Original Input 1:  file.fastq.gz
Note:              My Note
Assay Type:        sc-rna
Mode:              Single-end
Total Read Records:    321055
Compression Ratio: 5.81x (2312 / 398 MB)
Max Read Length:   301 (using short-read encoder)
Preserve Order:    Yes
Preserve IDs:      Yes
Preserve Quality:  Yes
Quality Mode:      Lossless
Compression Level: 6
Use CRLF:          No
--------------------------------
file.fastq.gz Original Compression:
  Profile:         BGZF (Default)
  Format:          BGZF (Block Gzip)
  Block Size:      2185
  Uncompressed Name: file.fastq
  Gzip Header:     FLG=0x4, MTIME=0, OS=255
  Member Count:    102860
  Original Ratio:  5.53x (6382 / 1154 MB)
  Likely Origin:   htslib/samtools/clib
```

For a comprehensive list of all options, quality modes, and multi-threaded examples, see the **[Usage Guide](docs/user/USAGE.md)** and **[CLI Reference](docs/user/CLI_REFERENCE.md)**.

## Related

- [Bioinformatics publication](https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty1015/5232998?guestAccessKey=266a1378-4684-4f04-bb99-6febdf9d1fb9)
- Specialized tool for nanopore long reads: <https://github.com/qm2/NanoSpring>

## Reporting Bugs and Errors

If you encounter a bug, crash, or unexpected behavior, please open an issue at:

<https://github.com/thisisamirv/SPRING2/issues>

To help us reproduce and diagnose problems quickly, include:

- SPRING2 version (`spring2 --version`)
- Platform details (OS, architecture, shell/environment)
- Exact command you ran

For diagnostics, please also provide logs from debug verbosity. Capture both stdout and stderr into a file:

```bash
spring2 ... --verbose debug > spring2-debug.log 2>&1
```

PowerShell equivalent:

```powershell
spring2 ... --verbose debug *> spring2-debug.log
```

Please upload `spring2-debug.log` to the issue so we can diagnose the problem quickly.

If your command includes sensitive paths or data, redact private information before posting logs.
