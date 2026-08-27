<!-- markdownlint-disable MD024 -->

# Changelog

## [Unreleased]

### Changed

- Changed CI to trigger only on `pull_request` (plus `merge_group` and manual `workflow_dispatch`) instead of also on every `push`, eliminating duplicate runs on the same commit.
- Changed `comparison.yml` to run only via manual `workflow_dispatch` instead of automatically after every CI run.
- Fixed the `macOS x64 Clang` CI leg to use `os: macos-15-intel` instead of `macos-latest` (which is Apple Silicon), so it actually tests x64.
- Added `-Wall -Wextra -Werror` (GCC/Clang) and `/W4 /WX` (MSVC/ClangCL) to first-party build targets, and `-warnings-as-errors=*` to the `clang-tidy` invocation. Neither had previously caused CI to fail on warnings.
- Marked pthash's own include directory `SYSTEM` in `vendor/pthash/CMakeLists.txt` so its warnings aren't attributed to first-party code under the new `-Werror`/`/WX` enforcement.
- Hardened benchmark dataset downloads (atomic temp-file rename, `https://` instead of `ftp://`) to avoid truncated-download and blocked-FTP failures on CI runners.
- Suppressed a known GCC false positive (`-Warray-bounds` misfiring on `std::vector<std::string>` reallocation at `-O1`) in the `sanitizers` job.

### Fixed

- Fixed dozens of pre-existing warnings surfaced by the new `-Werror`/`/WX` enforcement across GCC, Clang, MSVC, and ClangCL (Linux/macOS/Windows, including ARM64): narrowing conversions, uninitialized-variable false positives, variable shadowing, unused parameters, and a few small dead-code removals. Where a parameter was genuinely unused, it was removed (along with all call sites) rather than suppressed with `[[maybe_unused]]`/`(void)`-casts; a handful of parameters that mimic POSIX/Windows API contracts or are consumed only inside `#pragma omp` clauses were intentionally left in place.
- Removed several fully dead functions with no remaining callers (`spill_reordered_stream_artifact`, `compress_id_block`/`decompress_id_block` file-path wrappers, an unused test helper).
- Simplified the public `decompress()` API and several internal decompression helpers by dropping the `num_thr` and `compression_level` parameters that were threaded through many layers but never actually used for anything.
- Fixed a crash (segfault/abort) instead of a clean error when decompressing a corrupted or truncated archive. `decompress_archive_bsc_member`'s `allow_raw_fallback` option silently substituted the raw (still-corrupted) compressed bytes whenever BSC decompression failed, instead of propagating the error; those bytes were then misinterpreted as valid decoded data (e.g. read lengths), leading to memory corruption downstream. The two call sites that used this fallback (short-read unaligned reads, long-read lengths) always store genuinely BSC-compressed data, so the fallback was never legitimate; it has been removed entirely, and decompression failures now surface as a normal caught error.
- Fixed a heap over-read in `reference_sequence_store` (used to reconstruct short-read reference sequences during decompression): a chunk's declared length, taken from archive metadata, was never checked against the chunk's actually-decoded byte count, and `read()` never bounds-checked `chunk_index` against the chunk list. A corrupted or truncated archive could therefore cause an out-of-bounds heap read instead of a clean error. Both are now validated and throw on mismatch.
- Fixed a SIGSEGV (instead of a clean caught error) when decoding a deliberately corrupted archive on macOS x64. A debug build confirmed the crash was inside `libunwind.dylib`'s compact-unwind "frameless" stepper while walking the stack to propagate a thrown `std::runtime_error` — not in any SPRING2 code. Root cause: `-fomit-frame-pointer` was applied unconditionally, and Apple's compact-unwind info is unreliable for frame-pointer-less code in some stack shapes. `-fomit-frame-pointer` is no longer applied on Apple platforms.
- Organized `CMakeLists.txt` into clearly labeled sections (project metadata, toolchain detection, build options, compiler flags, OpenMP, vendor directories, targets, installation, tests) with no functional changes.
- Removed dead code with no remaining callers across `src/common`: the `bgzf_ostream` class (`bgzf.h`/`.cpp`, superseded by `bgzf_compress_buffer`), `OmpLockGuard` and `MmapView` (`raii.h`), `safe_bsc_decompress`/`safe_bsc_str_array_decompress`/`write_var_int64`/`read_var_int64` and their now-orphaned `zigzag_encode64`/`zigzag_decode64` helpers (`io_utils.cpp`), `safe_remove_file` (`fs_utils.cpp`), the four bit-serialization helpers `write_dna_in_bits`/`read_dna_from_bits`/`write_dnaN_in_bits`/`read_dnaN_from_bits` (`dna_utils.cpp`), and an unused `#include <atomic>` (`progress.h`). Also removed `detect_max_read_length`/`detect_max_read_length_in_file` (`input_preparation.cpp`, superseded by `detect_input_properties`), the dead `write_key_chunks`/`keys_bin_path`/`hash_bin_path` helpers and inert `bbhashdict::freeze()`/`is_frozen()` (`bitset_dictionary.h`), the unused `basedir`/`outfile_*` members of `encoder_global` and `reorder_global<N>::basedir` (both were vestiges of a removed disk-based encoding path), and a leftover empty anonymous namespace in `archive_record_reconstruction.cpp`.
- Consolidated three byte-for-byte identical `detect_sc_*_layout` functions (`assay_sc_rna.cpp`, `assay_sc_atac.cpp`, `assay_sc_bisulfite.cpp`) into a single shared `detect_single_cell_layout` helper in `assay_detector.cpp`, removing ~120 duplicated lines and the now-unnecessary per-assay dispatch in `AssayDetector::evaluate_stages` (used by `analyze_startup_sample`).
- Removed the unused `write_gzip_from_input` helper from `tests/bench/bench_common.py` (its `_bytes` sibling is the one actually used).
- Removed the `LIBRAPIDARCHIVE_WITH_RPMALLOC` and `LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB` CMake options from `vendor/indexed_bzip2/CMakeLists.txt` (declared but never consulted by any `if()`/`target_compile_definitions()` in that file) and the corresponding dead `LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB`/`LIBRAPIDARCHIVE_USE_ZLIB_NG`/`LIBRAPIDARCHIVE_WITH_RPMALLOC` cache-variable overrides in the root `CMakeLists.txt` (`LIBRAPIDARCHIVE_USE_ZLIB_NG` wasn't even declared anywhere). `LIBRAPIDARCHIVE_WITH_ISAL`, which is genuinely used, is unaffected.

## V1.3.4

### Changed

- Implemented chunked reorder for large datasets: read sets exceeding 50 M reads are now split into sequential chunks of up to 50 M reads each, with each chunk's bitset working set sized at ~2 GB. This keeps MPHF lookups and Hamming-distance comparisons within the L3 cache / DRAM bandwidth budget, replacing a single monolithic pass that would exceed available RAM. Reads receive globally-contiguous IDs before merge so downstream quality and ID reordering sees an unbroken ID sequence. The threshold can be overridden via `SPRING2_REORDER_CHUNK_SIZE` for testing.
- For reads longer than 100 bp, `maxshift` is now capped at 50 (was `readlen/2`) and `shift_step` is set to 2. Together these reduce shift iterations from up to 75 per read (for 151 bp) to 26, cutting reorder time roughly in half for long-read datasets. Reads ≤ 100 bp use the original `maxshift = readlen/2` and `shift_step = 1`.
- When `stop_searching` fires in the reorder inner loop, threads now immediately exit the per-read matching loop and perform a contiguous forward sweep over their own exclusive slice of `remaining_reads`. The previous stride-10 backward scan kept all threads within 9 bytes of each other on the same 64-byte cache lines, causing constant cross-core invalidation. The contiguous sweep eliminates false sharing, is prefetcher-friendly, and also removes two `updaterefcount` calls per unmatched singleton (pure overhead on this path). Any reads contested by the non-blocking `omp_test_lock` check are recovered by the existing post-loop safety scan.
- Fixed the `stop_searching` fast-path never firing. `unmatched_reads_in_window` is reset to 0 every 100K iterations and can increment at most once per iteration (max value ≈ 100K per window), but the trigger condition was `> STOP_CRITERIA_REORDER * 1000000 = 500000` — a threshold that could never be reached. Changed the multiplier from `1000000` to `100000` so the threshold becomes `50000` (50% of the window). For sc-ATAC datasets with ~96% singleton rate the condition fires in the very first 100K-iteration window, reducing per-chunk inner-loop iterations from ~6.6M (full backward scan of all reads) to ~100K active iterations before the fast-path sweep takes over. Without this fix the fast-path was dead code and the chunked reorder still ran 52 MPHF lookups per singleton, explaining the observed ~944 s/chunk for R1/R2 and ~1882 s/chunk for R3.
- Added 60-second progress messages (thread 0, seeds claimed and elapsed time) inside the reorder OMP loop so long-running cluster jobs produce visible log output.
- Added timing and count logging around the `stage_archive_members` call so previously invisible preprocessing staging time is observable in the log.

### Fixed

- Fixed OOM kill at reorder chunk 9 in disk_path mode for large sc-ATAC datasets. `clean_read_streams` (~29 GB for 726 M reads) and per-chunk `singleton_read_bytes` (~7.4 GB × up to 15 chunks) were both held in RAM simultaneously. The chunk loop now spills `clean_read_streams` to disk at chunk start and flushes `singleton_read_bytes` per chunk to the reorder artifact directory rather than accumulating them. The spilled artifact layout matches `spill_reorder_encoder_artifact` so the encoder stage loads it unchanged. Peak reorder RAM is now ~15–20 GB regardless of total read count.
- Fixed OOM kill at "Merging encoder metadata to disk..." in disk_path mode for datasets with many unmatched singletons (e.g. sc-ATAC). After the main encoding loop, `write_unaligned_range` accumulated all remaining unaligned reads into an in-memory buffer before flushing; for ~657 M unmatched singletons at ~155 bytes each this reached ~102 GB on top of the existing ~33 GB singleton bitset pool. A new `write_unaligned_range_streaming` variant writes each read's order, length, and sequence directly to the three output file streams, eliminating the peak buffer entirely.

## V1.3.3

### Fixed

- Fixed `--preview` consuming memory proportional to archive size for grouped archives. Previously the grouped archive path in `preview()` called `read_files_from_tar_memory` to buffer all member archives into RAM, then called `read_all_files_from_tar_bytes` to decompress each member fully, storing every file of every member in memory at once. The new `read_files_from_nested_tars` helper (added to `fs_utils`) streams the outer archive directly from disk via `archive_read_open_filename` and opens each nested member on-the-fly using a libarchive read callback — no full buffer for the outer entry; the inner reader pulls data 64 KB at a time. Only `cp.bin` (a few KB) is extracted from each member; all other member files are skipped. For single (non-grouped) archives, `preview_single` now uses `read_files_from_tar_memory` to load only `cp.bin` instead of reading the entire archive into memory.

- Fixed `--preview` crashing or reporting "Can't read compression parameters" on legacy spring v1 archives when only `cp.bin` is loaded. `looks_like_legacy_spring_archive` previously required `read_1.0` or equivalent large encoded-read files to be present in the loaded artifact map. It now also returns true when the artifact contains only `cp.bin` of exactly 64 bytes — the fixed size of the legacy spring compression-parameter block — so the legacy parsing fallback fires correctly without loading any large data files.

- Fixed a TOCTOU code-analysis finding in vendored libarchive (`archive_write_disk_posix.c`). Where `HAVE_UNLINKAT` is defined but the full `openat`/`fstatat`/`unlinkat` triple is not, `unlinkat(AT_FDCWD, path, 0)` is now used instead of `unlink()`. The remaining two call sites that genuinely cannot use a file-descriptor-relative form are annotated `// codeql[cpp/toctou-race-condition]` (updated from the retired `// lgtm[...]` syntax). The `.github/codeql/codeql-config.yml` vendor-exclusion file is removed as it is no longer needed.

## V1.3.2

### Fixed

- Fixed a segfault in the low-diversity read reorder fast path: `artifact.aligned_shards` is now initialized before the parallel encoding phase so the fast-path (used when average reads per dictionary key is extremely high) no longer dereferences an uninitialized vector. This resolves a crash seen on very low-diversity index-only datasets (e.g. sc-ATAC I1 barcode lanes) while preserving identical output and performance benefits.
- Fixed a potential use-after-free in the test `String` helper to ensure safe buffer swaps in `tests/support/doctest.h`.
- Widened loop and counter types and adjusted shift operands in `igzip` inflate and Huffman packing code to avoid narrow-vs-wide comparison warnings in `vendor/indexed_bzip2/isa-l/igzip/igzip_inflate.c` and `vendor/indexed_bzip2/isa-l/igzip/huff_codes.c.
- Cast multiplication operands to larger types in zlib/igzip utility code to avoid integer-promotion hazards (vendor `cloudflare_zlib` and `indexed_bzip2` patches).
- Reduced TOCTOU race windows in the vendored `libarchive` POSIX disk writer by attempting directory creation before unlinking and using safer unlink variants where available.
- Replaced plain `unlink()` calls with `unlinkat()` where available in the vendored `libarchive` POSIX writer to reduce TOCTOU race windows and simplified conditional fallbacks for portability.

## V1.3.1

### Changed

- Added a low-diversity fast path to the read reorder stage. When the average number of reads per dictionary key exceeds 10,000 (indicating extremely low sequence diversity, e.g. sc-ATAC I1 barcode reads with only a handful of distinct 8 bp sequences among hundreds of millions of reads), the greedy chain-matching algorithm is bypassed entirely. Instead, reads already grouped in the dictionary's `read_id` array are emitted directly in bucket order — each bucket becomes one chain (seed + aligned reads at shift 0), producing identically structured output to the normal path. For a 528 M × 8 bp barcode archive with 16 distinct sequences this reduces reorder time from several hours (O(N × MAX_SEARCH_REORDER) with heavy lock contention on 16 buckets across 10 threads) to a few seconds (O(N)), with no change to compression ratio or archive format.

## V1.3.0

### Removed

- Removed QVZ lossy quality compression entirely due to license incompability. The `-q qvz` CLI option, the `quantize_quality_qvz` function, the `qvz_flag`/`qvz_ratio` fields from `QualityConfig`, and the vendored `vendor/qvz` library have all been deleted. Passing `-q qvz` now throws a runtime error directing users to `ill_bin` or `binary` instead. The binary serialization layout is preserved for backward compatibility when reading older archives that carry QVZ-mode flags, but those flags are now treated as reserved and the quantization step is no longer executed. Decompression of archives that were compressed with QVZ is unaffected because QVZ only transformed quality values during compression; the stored values are already quantized and are read back without any QVZ library involvement.

## V1.2.1

### Changed

- Changed disk-path stream reordering (the "Reordering and compressing streams" stage) to buffer scatter records in RAM instead of reopening a per-block scratch file for every read. `partition_alignment_stream_records` previously called `append_record_to_file` once per read, performing an `open(append)`/write/`close` cycle per record; for a ~1 B-read dataset this issued ~1 billion file-open round-trips, which stalled for days on NFS-backed work directories. Records now accumulate in in-RAM per-block buffers and are flushed to their scratch files only in large batched appends when the aggregate buffer size crosses a cap (`0.4 × memory budget`, or a 4 GiB default when no budget is supplied), collapsing file opens from ~1 per read to at most one per block per flush.
- Added an in-RAM fast path to disk-path stream reordering: when the scatter buffers never exceed the cap they are handed directly to the block rebuild, skipping the scratch write and read-back entirely (eliminating ~80 GB of intermediate NFS traffic for a 1 B × 50 bp dataset). The block parser (`rebuild_stream_blocks`) now consumes either the in-RAM buffer or a spilled file through a shared zero-copy `span_streambuf`, and `reorder_compress_streams` selects the in-RAM or spill path based on whether the cap was crossed. `staged_stream_record_header` was packed (`#pragma pack(1)`, 24 → 18 bytes) to shrink both the in-RAM buffers and the spilled scratch payload.
- Changed the stream-reordering memory budget to flow from the resolved `-m` cap through `reorder_compress_streams`, including all grouped-bundle members (read, read-3, and index sub-archives), so grouped assays such as sc-ATAC size their scatter buffers against the user's actual memory budget instead of the conservative default cap.
- Changed disk-path quality/ID reordering to use cap-aware in-RAM batch buffers with conditional spill, mirroring the stream-reordering strategy. The file-mode path now partitions into in-RAM per-batch buffers first and spills only when buffered bytes cross a cap (`0.4 × memory budget`, or a 4 GiB default when no budget is supplied), flushing each non-empty batch via large append writes instead of constant scratch-file churn. When the cap is never crossed, batch rebuild parses directly from in-memory buffers and skips scratch-file write/read-back entirely.
- Changed disk-path reorder MPHF selection to prefer in-memory construction when the resolved memory budget can safely absorb it, instead of always preferring external-memory MPHF when temp-disk space is available. The workflow now estimates MPHF in-core demand from total clean reads and keeps a headroom guard; when memory is sufficient it forces internal MPHF to avoid temp-file round-trips on high-latency work directories (for example, NFS), and falls back to external-memory MPHF only when RAM headroom is insufficient.
- Added fine-grained reorder-stage timing diagnostics in `call_reorder` and dictionary construction logs so long-running datasets can attribute time to specific subphases: per-dictionary `MPHF build time`, per-dictionary `Hash pass time`, plus top-level `Dictionary stage time`, `Reorder pass time`, and `Reorder write time`. This makes it straightforward to distinguish MPHF bottlenecks from reorder matching or output-materialization bottlenecks during production runs.

## V1.2.0

### Changed

- Changed disk-path encoding to stream aligned shard files directly from disk instead of loading all shard byte buffers into RAM before encoding. `load_reorder_encoder_artifact` now records file paths for each shard's `flag_bytes.bin`, `read_bytes.bin`, `orientation_bytes.bin`, `position_bytes.bin`, `order_bytes.bin`, and `read_length_bytes.bin` when `stream_from_disk=true`; the encoder's OpenMP thread loop opens its private `std::ifstream` handles for the shard on disk rather than indexing into in-memory strings. For a 1 B × 50 bp paired-end dataset with N encoding threads this eliminates a ~25 GB shard-buffer peak (500 M aligned reads × ~50 bytes each) that previously coexisted with the bitset array and MPHF during encoding.
- Changed disk-path reordering to free the raw clean-read byte streams (`clean_read_streams`) immediately after `readDnaFile` decodes them into the bitset array, instead of holding them live for the entire reorder stage. `reorder_main` now takes `reorder_input_artifact` by value; after decoding, both streams are swapped with empty strings and the `n_read_bytes`/`n_read_order_bytes` fields are moved into the output artifact. For 1 B × 50 bp paired-end reads this eliminates a ~58 GB peak that previously coexisted with the bitset array throughout MPHF construction and read reordering.
- Changed disk-path encoding to stream singleton reads directly from disk instead of loading the full raw singleton byte buffers into RAM before decoding. `load_reorder_encoder_artifact` now accepts a `stream_from_disk` flag that records file paths for `singleton_read_bytes.bin`, `singleton_order_bytes.bin`, `n_read_bytes.bin`, and `n_read_order_bytes.bin` instead of reading them into memory; `readsingletons` dispatches to `std::ifstream`-based readers when those paths are set. For datasets with many singletons (e.g. ~548 M singletons × 50 bp), this eliminates a ~27+ GB raw-bytes peak that previously coexisted with the bitset array during encoding, resolving OOM failures on constrained disk-path runs.
- Changed disk-path encoding to write per-thread encoder metadata (positions, orientations, read lengths, read orders, noise) directly to per-thread binary files on disk during the OpenMP encoding loop, instead of accumulating them in `reordered_stream_artifact` RAM buffers and spilling after encoding completes. `EncodingConfig` and `encoder_global` gain an `encoder_metadata_spill_dir` field; when non-empty, each encoder thread opens binary output files under `metadata_spill_dir/threads/<id>/` and flushes accumulated vectors every 500 000 reads; after the OMP loop, `encoder_main` stream-merges the per-thread files into the final `read_order_entries.bin`, `orientation_entries.bin`, `position_entries.bin`, `read_length_entries.bin`, `noise_serialized.bin`, and `noise_positions.bin` artifacts, appends unaligned-read entries, and returns an empty artifact so the downstream `spill_reordered_stream_artifact` call is skipped. For a 1 B × 50 bp dataset this eliminates the ~15 GB `reordered_stream_artifact` peak that previously built up while all encoding threads ran concurrently.

## V1.1.0

### Added

- Added disk-path memory reduction via external-memory MPHF construction: when disk-backed compression is selected and the work directory has sufficient free space (estimated as 40 bytes × total clean reads), SPRING2 now calls pthash's `build_in_external_memory` instead of `build_in_internal_memory`, offloading the key-sort/search structures to temp files and reducing peak RAM during MPHF construction by ~4–8 GB.
- Added disk-path memory reduction via thread-count capping: when disk-backed compression is selected but the work directory lacks sufficient space for external MPHF temp files, SPRING2 automatically reduces the encoding thread count to `floor(available_memory / 2 / 4 GiB)`, trimming per-thread encoder buffer overhead to help stay within the user's memory budget.

### Removed

- Removed `tools/dev/docker/` (per-platform Docker dev-environment templates).

### Fixed

- Fixed an MSVC-specific private-member access error in the vendored pthash `mm_file.hpp`: `file_source::open()` accessed `base::m_data` and `base::m_size` directly, which GCC/Clang allow through a base-class qualifier but MSVC rejects. Changed both accesses to use the protected accessor methods `base::data()` and `base::bytes()`.
- Fixed missing comparator overload in the vendored pthash `essentials.hpp` `boost::range` compatibility shim: the `sort` wrapper only provided the single-argument form, causing a compile error when `external_memory_builder_single_phf` called `boost::range::sort(range, comparator)`. Added `template <typename Range, typename Compare> void sort(Range &r, Compare comp)`.

## V1.0.2

### Added

- Added explicit redistribution permission notice.

## V1.0.1

### Changed

- Moved the in-repo conda recipe from `conda/recipe/` to `tools/conda/recipe/` to consolidate all developer tooling under `tools/`.
- Converted the conda recipe from the legacy conda-build format (`meta.yaml`) to the rattler-build v1 format (`recipe.yaml`), aligning the in-repo recipe with the conda-forge staged-recipes submission.
- Replaced `conda-build` with `rattler-build` in the `conda-build-and-test` CI job. The CI now patches the recipe's `source` block at build time to use a local path (`../../..`) instead of the tagged tarball URL, so every push is tested against the current checkout rather than the last release. The built package is installed from a local `file://` channel and tested with `conda run`.
- Renamed `conda/recipe/bld.bat` to `build.bat` (rattler-build looks for `build.bat`, not `bld.bat`, on Windows).

### Fixed

- Fixed Windows conda package test failure (exit code 9009 — "command not found") caused by `spring2.exe` being installed to `%PREFIX%\bin`, which is not on PATH in conda's Windows test environment. The `install(TARGETS)` call used a hardcoded `DESTINATION bin` that silently ignored the `CMAKE_INSTALL_BINDIR=Library\bin` passed by the conda build script. Added `include(GNUInstallDirs)` and replaced all hardcoded `bin`/`lib` install destinations with `${CMAKE_INSTALL_BINDIR}`/`${CMAKE_INSTALL_LIBDIR}` so the override takes effect.
- Fixed GitHub Actions CI failures on Linux caused by Microsoft APT repositories (`azure-cli`, `microsoft-prod`) serving invalid InRelease files ("NOSPLIT" error). Both the `ci` and `sanitizers` jobs now remove the broken Microsoft source files before running `apt-get update`.

## V1.0.0

### Added

- Added first-class conda packaging support with an in-repo recipe (`conda/recipe/meta.yaml`) and platform build scripts (`conda/recipe/build.sh`, `conda/recipe/bld.bat`) so SPRING2 can be built as a conda package on Linux, macOS, and Windows.
- Added conda reliability checks to CI (`.github/workflows/ci.yml`) that build the conda recipe and run smoke tests (`spring2 --version`, `spring2 --help`) from a locally installed conda artifact, without publishing.
- Added conda publishing documentation (`docs/dev/CONDA_PUBLISHING.md`) covering local build/test, Anaconda channel upload, and conda-forge feedstock flow.
- Added tests to all CI runners to ensure full platform and compiler compability.

### Changed

- Removed the README license-restriction notice after receiving redistribution permission; SPRING2 is now published under the same license as upstream SPRING.
- Dropped Windows installers to reduce release complexity and confusion; releases now ship Windows standalone binaries, Linux AppImages, and macOS `.app` bundles.
- Downgraded minimum CMake dependency version to 3.31+ to support more platforms.
- Replaced the native C++ `embed-reference` codegen executable with an equivalent Python script (`tools/codegen/embed_reference.py`). The C++ binary was blocked at build time by Windows Device Guard policy on hardened runners and developer machines. The Python interpreter is signed and trusted by Device Guard. Python's `zlib.compress()` produces standard RFC 1950 zlib output, which is fully compatible with the `libdeflate_zlib_decompress` runtime path. CMake now locates the interpreter via `find_package(Python3 REQUIRED COMPONENTS Interpreter)` instead of building and invoking a custom executable.

### Fixed

- Fixed an MSVC-specific crash (exit code 3) during MPHF construction in the vendored pthash library. `buckets_iterator_t::skip_empty_buckets()` and `operator++` decremented `m_buffers_it` past `begin()` once `m_bucket_size` reached zero. Fixed by guarding both decrement sites on `m_bucket_size != 0`.
- Fixed an MSVC-specific access violation in `populate_bucket_read_ids` during dictionary construction. MSVC does not inline `std::unique_ptr::operator[]` in Debug builds, so `#pragma omp atomic capture` received a non-addressable expression. Fixed by extracting raw pointers from the `unique_ptr` members before the atomic region.
- Fixed integration tests flooding CI logs with megabytes of genomic data on assertion failure. Added a `check_bytes_equal` helper that reports only sizes on mismatch instead of stringifying full file contents via doctest's expression decomposer.
- Set `DOCTEST_CONFIG_USE_STD_HEADERS` for all test targets to suppress MSVC C5285 warnings from doctest forward declarations of standard library templates.
- Fixed SPRING1 archive decompression producing truncated output on Windows ARM64. With OpenMP disabled on MSVC ARM64, `omp_get_thread_num()` always returned 0 inside `decompress_short()`/`decompress_long()`, so only the first block per step was processed. Replaced the `#pragma omp parallel` + `omp_get_thread_num()` pattern with `#pragma omp parallel for` so all blocks are processed even without OpenMP.
- Fixed gzipped input processing crashes (0xC0000005) on Windows by compiling the vendored `cloudflare_zlib` with `/Od` (MSVC/ClangCL) or `-O1` (Clang) to avoid miscompilation in the zlib inflate/gzread path under aggressive optimization.
- Fixed gzipped input processing crashes (0xC0000005) on Windows ARM64 MSVC. MSVC ARM64 defines `__aarch64__`, activating unsafe CRC32 intrinsic and NEON SIMD code paths in `cloudflare_zlib` that contain unaligned pointer casts. Fixed by passing `/U__aarch64__` to force the generic safe C fallbacks.
- Fixed compression crashes (0xC0000005) on Windows ClangCL x64 caused by a file-scope `thread_local std::string` in `io_utils.cpp`. ClangCL's dynamic TLS initialization wrapper is not triggered for threads created by LLVM libomp's raw `CreateThread()`, so the first access in a worker thread crashed. Fixed by replacing the `thread_local` with a plain local `std::string` declared inside each parallel block.
- Fixed all remaining compression and decompression crashes (0xC0000005) on Windows ClangCL x64. Clang's OMP outlined-function closure mishandles shared `std::vector<T>` variables — only thread 0 receives a valid `data()` pointer, causing an access violation in `std::bitset<256>::operator&` in `reorder()`, truncated gzip output from `write_fastq_block`, and missing `readlength_1.N.bsc` archive members from the long-read preprocessing loop. Fixed by extracting raw data pointers before the parallel region in `reorder()` and converting the affected bare `#pragma omp parallel` blocks in `io_utils.cpp` and `input_preparation.cpp` to `#pragma omp parallel for`.
- Fixed `valid_bucket_range` in `bitset_dictionary.h` accepting empty ranges `[N, N)`. An empty range causes the tail sentinel `MAX_NUM_READS` to index `reads[]`, producing an out-of-bounds heap access. Fixed by changing the guard to `dictidx[1] <= dictidx[0]`.
- Fixed a thread-safety data race on `bbhashdict::empty_bin`: `std::vector<bool>` packs bits into shared words, making adjacent-element writes from different threads unsafe. Changed the type to `std::vector<uint8_t>` so each element has its own byte.
- Fixed compression crashes (0xC0000005) on Windows ClangCL x64 caused by LLVM libomp creating worker threads via `CreateThread()` instead of `_beginthreadex()`. With the static CRT (`/MT`), per-thread CRT state is never initialized for such threads, crashing as soon as any non-trivial C++ scope runs on a worker. Fixed by switching ClangCL to the dynamic CRT (`/MD`) so `vcruntime140.dll` initializes per-thread state for all threads regardless of creation API.
- Fixed integration test failures on Windows (`tar: _repack_tmp.tar: Can't add archive to itself`) caused by bsdtar including its own output file in a `*` glob. Added `--exclude _repack_tmp.tar` to all `tar -cf` invocations.

## V1.0.0-rc.2

### Added

- Added memory-aware compression-path planning helpers that detect available system memory, estimate total input size including `.gz` inputs by inferred uncompressed size, and honor the user-provided `-m` memory cap when choosing between in-memory and disk-backed compression.
- Added version-aware decompression support for backward compability in SPRING2 future versions. Current archives keep their exact stored creator version, headered metadata remains version-aware, and older headerless archives are pinned to `1.0.0-rc.1` for preview and decompression compatibility.
- Added support for legacy SPRING archives, including preview, decompression, and `SpringReader` compatibility for checked-in legacy `*.spring` sample fixtures from SPRING1.
- Added Windows release packaging with architecture-specific Inno Setup installers, embedded executable and installer version metadata, publisher details, release icon assets, and optional code-signing hooks for GitHub Actions.
- Added Linux release packaging metadata with repo-owned AppImage desktop and AppStream files so release builds ship cleaner, more standards-compliant Linux application metadata.
- Added macOS release packaging assets, DMG staging, and a native `.pkg` installer so universal macOS releases now include install guidance, a helper install script, and a no-terminal installer path alongside the `spring2` binary.
- Added release-engineering documentation covering optional Windows code-signing setup and the available Windows installer tasks.
- Added a regression test covering compression storage planning so the memory-path versus disk-path decision now stays pinned to the required peak-memory threshold instead of only raw input bytes.
- Added installation guides to the docs.

### Changed

- Changed compression to support a real disk-backed fallback path for memory-constrained runs: SPRING2 now selects the in-memory path only when available RAM covers estimated input bytes plus an explicit peak-intermediate-memory estimate and additional safety margin; standard archives stage work under `<archive>.work-tmp`, grouped bundle members under `<archive>.grouped-tmp`, short-read disk-path compression spills reorder, encoder, and post-encode side-stream artifacts to disk between stages, and final archive assembly plus alignment, quality, and ID stream rebuilding can consume staged files directly from disk instead of rebuilding large in-memory payloads; paired-end order rewrites now persist updated `read_order_entries` into the spilled encoder artifact before downstream quality/ID and final stream reordering consume that metadata.
- Replaced the Python-based embedded-reference generator with a native C++ codegen helper under `tools/codegen`, backed by `libdeflate`, so the build no longer depends on a Python interpreter to produce `reference_data.cpp`.
- Replaced the Bash and PowerShell helper wrappers for linting, cppcheck, smoke/valgrind runs, and benchmark workflows with cross-platform Python tooling, and aligned the test helper naming around `unit-tests` and `smoke-tests`.
- Reorganized the repository layout so developer tooling now lives under `tools/dev`, release packaging assets under `tools/release`, assay reference assets under `tools/assay-reference`, checked-in test fixtures under `tests/data`, and default installed binaries under `out/bin` instead of `dist/bin`.
- Filtered clang-tidy’s internal “Processing file” chatter and replaced the per-file banner spam with a single cppcheck-style progress line per completed file.
- Updated `mkdocs-material` and `pymdown-extensions` to the latest versions.
- Removed `IntelLLVM` compiler support from the CMake build, user-facing build documentation, and GitHub Actions compiler-compatibility workflow due to untraceable comipiling issues.

### Fixed

- Fixed preprocessing progress reporting for gzipped input files by tracking gzip stream progress from the compressed byte offset instead of using invalid `tellg()` positions on the zlib-backed input stream; progress updates are now also clamped to valid bounds before rendering.

## V1.0.0-rc.1

### Added

- Added comprehensive docs and examples and launched dedicated documentation website.
- Added support for Windows ARM64 devices.
- Added grouped lane support for read-3 and index reads with `-R3/--R3`, `-I1/--I1`, and `-I2/--I2`: SPRING2 can now compress `R1/R2` together with a third read lane and/or index lanes and restore grouped outputs on decompression (`.R1`, `.R2`, `.R3`, `.I1`, `.I2`).
- Added automatic non-ACGTN sequence detection during input pre-scan: when extended IUPAC/RNA symbols are present, SPRING2 now switches to long-read mode to preserve sequence alphabet losslessly instead of using the short-read 3-bit path.
- Added the `-y/--assay` flag to specify and store the sequencing assay type in the archive metadata.
- Implemented **scoring-based assay detection system** (`--assay auto`) that combines evidence from multiple detection methods to classify sequencing chemistry and layout (RNA, ATAC, bisulfite/methylation, DNA, and single-cell variations). The detector samples the first 10,000 reads and aggregates confidence scores from: (1) base composition analysis with relaxed bisulfite thresholds (C/(C+T) and G/(G+A) ratios ≤0.10 for high confidence, ≤0.15 for moderate) to catch real-world conversion efficiencies, (2) FASTQ sequence signatures (Tn5 adapters for ATAC, poly-A/T tails for RNA with bisulfite false-positive suppression), and (3) reference k-mer alignment sketches to RNA exons, ATAC promoters, and genomic backbone regions. Single-cell layout detection uses multiple independent indicators including explicit index lanes (R3/I1/I2), CB:Z: and UMI tags in FASTQ headers, and read-length asymmetry patterns (short R1 with long R2). The final classification selects the assay type with the highest cumulative evidence score and provides detailed confidence reporting showing which signals contributed to the decision.
- Implemented **bisulfite-aware overlap-based compression for methylation assays**: 2-bit encoding and bitwise masking in the reordering dictionary allow the overlap-based encoder to match reads regardless of bisulfite conversion status (C/T and G/A), maximizing consensus contig length and compression ratios for methylated data while ensuring bit-perfect reconstruction.
- Added a small checked-in assay reference (`ref_hg38_gencode49.fa`) and corresponding metadata for detecting assay type from input FASTQ files.
- Implemented **cell barcode prefix extraction for sc-RNA compression**: For single-cell RNA assays in preserve-order mode without external index lanes (I1/I2), the first `--cb-len` bases (default 16 bp) are extracted from R1 reads during preprocessing and stored in a separate compressed stream (`cb_prefix.dna`), allowing the overlap-based encoder to operate on the UMI-only portion of R1. This reduces R1 read length from 28bp (CB+UMI) to 12bp (UMI only), enabling more effective compression since the highly repetitive CB sequences are encoded separately from the random UMI data. CB prefixes are transparently restored during decompression. The optimization is lossless and automatically enabled for `--assay sc-rna` only when the cell barcode is embedded in R1 rather than provided through external index lanes. Grouped sc-RNA inputs that already carry CB data in I1/I2, such as `test_5`, do not use this path and therefore should not be expected to show a compression gain from CB-prefix extraction. Note: sc-bisulfite protocols vary in read structure (some place genomic sequence in R1, others use CB+UMI), so SPRING2 currently leaves CB extraction disabled for sc-bisulfite.
- Implemented **lossless terminal adapter stripping for ATAC / sc-ATAC compression**: For ATAC-family assays, SPRING2 now detects terminal Tn5/Nextera read-through sequence at the end of genomic reads, strips the matched suffix during preprocessing, and stores the removed bases in a synchronized side stream for exact restoration during decompression. The transform is enabled only when the detected adapter burden is high enough to be net beneficial, skips grouped index/read-3 lanes, and preserves bit-perfect reconstruction even for mixed-content datasets containing `N` reads.
- Implemented **index-aware grouped sc-RNA ID compression**: For grouped single-cell RNA archives with external `I1`/`I2` lanes, SPRING2 now detects the common Illumina-style layout where the trailing FASTQ identifier token already contains the index reads (`...:I1+I2`). In that case, the grouped index subarchive stores only the identifier prefix and reconstructs the trailing `I1`/`I2` token from the decoded index reads during decompression. This avoids compressing the same barcode token twice, remains fully lossless, and improves grouped sc-RNA compression when cell barcode / sample index bases are provided as explicit index lanes.
- Added SPRING2 version to the metadata of compressed files.
- Implemented **lossless poly-A/T tail stripping and restoration for RNA-seq assays**: Strips long terminal poly-A/T runs (>20bp) during preprocessing to improve compression ratios for transcriptomics data. The optimization is strictly lossless, storing stripped sequence bases and quality scores in a synchronized metadata stream that is reordered in tandem with reads. Restoration during decompression ensures identical reconstruction and passes all integrity audits. Activated automatically for high-confidence RNA assays or via `--assay rna`.
- Added regressions covering the shared 10,000-fragment startup sampler, late overlength escalation from sampled short-read input into long mode, and late CRLF metadata discovery during preprocessing retry.
- Added integration regressions covering grouped preview/audit corruption detection, grouped decompression output naming and explicit output lists, grouped `SpringReader` streaming, aliased grouped `R3` output formatting, mixed paired FASTQ plus-line and LF/CRLF round-trips, per-stream gzip reconstruction behavior, archive extraction path containment, preview and `SpringReader` rejection of truncated metadata, output-path collision rejection, grouped manifest validation, `SpringReader` digest retrieval, and graceful failure for corrupted long-read archives.
- Expanded compiler support.

### Changed

- Refactored source code into a more modular structure.
- Changed the compression CLI to require `-R1/--R1` for read 1 and accept optional `-R2/--R2` for paired-end mode; compression no longer uses `-i/--input`.
- Removed the obsolete working-directory CLI flag `-w/--tmp-dir` now that runtime working-directory use has been fully eliminated.
- Changed reordering algorithm to use deterministic matching.
- Replaced archived vendors with direct dependencies.
- Pruned vendored libs even further.
- Had vendored libs flattened for easier maintenance. This allows easier future pruning and specialized modifications.
- Merged preview into the main binary. spring2 now accepts `-p` or `--preview` and runs the old `spring2-preview` behavior, including `-a/--audit` in preview mode.
- Changed the compression, decompression, preview, audit, grouped-archive, and `SpringReader` pipelines to operate fully in memory, completely eliminating working-directory use and temporary working files across the runtime data path.
- Changed compression startup to reuse one shared 10,000-fragment sample for assay detection plus initial read-length, newline, and non-ACGTN analysis instead of running separate startup passes; sampled-long inputs still take the existing full prescan, while sampled-short inputs defer full validation to preprocessing.
- Changed sampled short-read startup detection so preprocessing now validates sampled max-read-length, CRLF, and non-ACGTN assumptions while streaming input, retries once from a clean restart when later data changes those properties, and escalates to the long-read path with a full prescan when late reads prove the sample was not representative.
- Vendored NASM and Ninja for easier development.

### Fixed

- Fixed decompression thread count mismatch bug in `src/decompress.cpp` where archives compressed with N threads could only be decompressed successfully with exactly N threads. The decompressor was incorrectly using the user-specified `-t` flag instead of the archive's encoded thread count metadata, causing incomplete block processing when fewer threads were used. Decompression now always uses the archive's encoding thread count internally (via `archive_encoding_thread_count`) regardless of the user's `-t` flag, ensuring archives can be decompressed on any hardware configuration.
- Fixed grouped preview, grouped `spring2 -p -a/--audit`, and grouped decompression handling so grouped bundles now execute the real audit path, preserve archive notes in preview output, and accept all five explicit output paths (`R1`, `R2`, `R3`, `I1`, `I2`).
- Fixed truncated-metadata handling by rejecting archives whose `cp.bin` parameter stream cannot be fully read, instead of continuing with partially parsed compression parameters.
- Fixed long-read decompression error handling in `src/decompress.cpp` by capturing exceptions inside the OpenMP worker region and rethrowing them on the main thread, preventing corrupted long-read blocks from terminating the whole process instead of returning a normal runtime error.
- Fixed archive assembly in `src/fs_utils.cpp` to fail loudly on `stat`, header, open, read, write, and finalize errors instead of silently producing truncated or incomplete tar archives.
- Fixed lossless paired FASTQ round-tripping for mixed formatting by storing quality-header style (`+` versus `+READ_ID`) and newline convention (LF versus CRLF) per stream in archive metadata, rather than collapsing both mates to one archive-wide setting; backward compatibility with older archives is preserved.
- Fixed grouped decompression when `R3` is stored as an alias of `R1` or `R2`: aliased outputs are now materialized through the normal decompression path so the requested target format is preserved instead of inheriting the source mate's on-disk representation.
- Fixed `SpringReader` so it can stream grouped bundle archives by resolving `bundle.meta`, loading the primary read member archive, and reading `cp.bin` plus decode streams from that nested archive instead of assuming a flat top-level layout.
- Fixed paired gzip output reconstruction so each mate preserves its own compression behavior during decompression; SPRING2 no longer collapses both output streams to one archive-wide gzip level.
- Fixed sorted-output quality and ID reordering after the in-memory reorder-to-encoder refactor by making downstream order-map generation stop once `read_order.bin` already covers all output positions, preventing stale singleton or N-order side files from corrupting reordered streams.
- Fixed archive extraction in `src/fs_utils.cpp` to reject absolute or escaping tar entry paths, preventing crafted archives from writing outside the requested extraction directory during preview, audit, decompression, or `SpringReader` setup.
- Fixed decompression output validation so SPRING2 now rejects colliding output paths and refuses to overwrite the input archive when reconstructing reads.
- Fixed grouped bundle decompression defaults so duplicate original lane basenames are deterministically disambiguated with role-based suffixes (`.R1`, `.R2`, `.R3`, `.I1`, `.I2`) instead of still colliding after a single `.index` suffix pass.
- Fixed grouped bundle manifest parsing to reject invalid `read3_alias_source` values and conflicting read-3 archive/alias declarations, preventing silent reconstruction of the wrong `R3` lane from malformed metadata.
- Fixed grouped bundle metadata validation drift by centralizing `bundle.meta` parsing and invariants in a shared helper used by compression, decompression, preview, and `SpringReader`; malformed grouped manifests and truncated nested member metadata now fail consistently.
- Fixed preview read-count reporting so archive metadata now labels the aggregate paired-end count as `Total Read Records` and reports per-input clean reads with explicit non-clean read counts, instead of presenting incompatible totals under `Reads Processed`.
- Fixed compression output validation so archive creation now refuses output paths that would overwrite any input FASTQ/FASTA file, preventing destructive in-place compression mistakes.
- Fixed `SpringReader` lifecycle handling by allowing the background producer to shut down cleanly when the reader is destroyed before the archive is fully consumed, avoiding a queue-backpressure deadlock on early exit.
- Fixed `SpringReader::get_digests()` so library callers can retrieve the actual computed sequence, quality, and ID CRCs after fully consuming an archive, instead of always receiving zeroed outputs.
- Fixed paired-end preprocess cleanup so merged mate-side `input_N.dna.2`, `read_order_N.bin.2`, and redundant mate-ID intermediates are closed before deletion and removed with the safe filesystem helper; this prevents stale raw temporary files from being packaged into `.sp` archives on Windows and reduces final archive size.
- Fixed release/install packaging drift so fresh self-contained builds no longer install vendored dependency artifacts or a standalone `rapidgzip` tool; Windows now defaults to static runtime linking, and a clean install produces a single `spring2.exe` instead of extra dependency binaries and headers.

## V1.0.0-beta

### Added

- Implemented **Archive Integrity Auditing**: Added record-lditing).
- Added `tests/integrity_test.cpp` to validate end-to-end data fidelity and corruption detection.
- Expanded the `SpringReader` API with `get_digests()` to enable programmatic integrity verification for library consumers.
- Implemented a public library-style **Streaming Decompression API** (`SpringReader`) for SPRING2 archives, enabling external tools to consume genomic records programmatically without intermediate file I/O.
- Added the `DecompressionSink` abstract interface, allowing the decompression engine to push reconstructed records to arbitrary consumers (files, memory buffers, or network streams).
- Integrated a high-performance **Asynchronous Producer-Consumer model** within `SpringReader` that leverages background pre-fetching to maintain maximum throughout during streaming.
- Added robust stream error checking in `src/decompress.cpp` to handle corrupt or truncated archives during position and orientation decoding.
- Replaced system `tar` subprocess calls with the native `libarchive` library, removing a major runtime dependency and improving cross-platform compatibility.
- Added `src/scoped_temp_file.h` providing `ScopedTempFile` RAII helper to safely remove temporary files in a noexcept destructor; replaces ad-hoc temp-file handling to reduce races and ensure deterministic cleanup.
- Added the `-V, --version` flag to both `spring2` and `spring2-preview` tools for reporting the application version.
- Integrated the **doctest** unit testing framework for granular logic verification.
- Added a suite of unit tests in `tests/unit/unit_tests.cpp` covering core utility functions (sequence manipulation, parsing, string helpers).
- Integrated both unit and smoke tests into the unified `ctest` workflow.
- Implemented a native Windows backend for `MmapView` in `src/raii.h`, enabling high-performance memory-mapped I/O on Windows.
- Added automatic compiler-cache launcher support in CMake (`SPRING_ENABLE_COMPILER_CACHE`, default ON).
- Added automatic fast-linker selection in CMake (`SPRING_ENABLE_FAST_LINKER`, default ON), preferring `mold` on Linux and falling back to `lld` where supported.
- Added precompiled header support in CMake (`SPRING_ENABLE_PRECOMPILED_HEADERS`, default ON) to accelerate incremental rebuilds by caching stable headers (`src/pch.h`).
- Added upfront I/O parameter validation in `src/main.cpp` via `validate_io_parameters()` to verify input files exist, output directories are accessible, and paired-end compression requirements are met before entering the main compression/decompression pipeline; this ensures any runtime error is genuinely a compression/decompression issue, not a parameter error.

### Changed

- Optimized `reference_sequence_store::find_chunk_index` in `src/decompress.cpp` by replacing the linear scan with a binary search on chunk start offsets.
- **Improved compression parallelism** by restoring parallel sequence chunk packing in `src/encoder.cpp`, removing OpenMP critical sections from error logging in `src/encoder_impl.h` and `src/reorder_impl.h` (replaced with thread-local error aggregation), removing redundant startup barrier in `src/reorder_impl.h`, and unlocking MPHF dictionary thread count in `src/bitset_dictionary.h` (was hardcoded to 1).
- Increased and batched buffered I/O in the compression tail by expanding archive creation and per-thread encoder merge copy buffers to reduce serialized filesystem overhead.
- **Parallelized preprocessing N-read classification** in `src/preprocess.cpp` by adding `#pragma omp parallel for` loop for N-read detection to reduce preprocessing bottleneck on large inputs.
- Reduced preprocessing pass overhead by extending FASTQ/FASTA block parsing in `src/io_utils.cpp` to emit per-read metadata (length and N-presence), perform optional quality-length validation, and accumulate record CRCs while parsing; `src/preprocess.cpp` now consumes these parser outputs directly instead of re-scanning full record arrays.
- Reduced short-read preprocessing write overhead in `src/preprocess.cpp` by batching per-thread encoded clean/N-read payloads and flushing them once per block in deterministic order, and by batching quality/ID text stream writes into larger contiguous chunks.
- Reduced preprocessing allocation churn by conditionally allocating stream-specific string working sets and reusing per-step staging buffers in `src/preprocess.cpp`.
- Reduced sequential bottlenecks in `src/reordered_streams.cpp` by introducing safe bulk metadata loading (orientation/position/noise/read-length/order streams) with deterministic in-memory parsing, and by replacing per-read unaligned bit unpacking with boundary-validated parallel decode into disjoint output ranges.
- Reduced reorder lock contention in `src/reorder_impl.h` by shortening dictionary lock hold windows in `search_match`: candidate read IDs are snapshotted under lock and validated after unlock, reducing shard lock occupancy during Hamming/read-lock checks.
- Reduced shared-state contention in `src/reorder_impl.h` by splitting the unmatched-read fallback scan into per-thread striped seed buckets, so threads probe disjoint read windows with fewer lock collisions.
- Reduced synchronization and sorting overhead in the dictionary and reorder paths: `src/bitset_dictionary.h` now uses parallel chunk sorts with deterministic merges before the existing dedup pass, and `src/reorder_impl.h` assigns seed reads with OpenMP atomic capture instead of the initial cross-thread critical section.
- Optimized singleton DNA+N decoding in `src/encoder_impl.h` by replacing per-read `read_dnaN_from_bits()` calls with an inlined decoder loop and larger buffered stream I/O in the hot path.
- Optimized parallel gzip FASTQ writing in `src/util.cpp` by implementing thread-local reusable buffers and a persistent `libdeflate_compressor` cache to reduce heap allocator pressure.
- Optimized `merge_paired_n_reads` in `src/preprocess.cpp` to use buffered I/O, reducing the number of disk operations when merging N-read positions.
- Modernized the monolithic `compression_params` struct by decomposing it into nested, cohesive component structures (`EncodingConfig`, `QualityConfig`, `GzipMetadata`, `ReadMetadata`). This improves type safety and clarifies field ownership across the compression and decompression pipelines.
- Refactored `src/encoder.h` into a thin interface header, moving template implementations to `src/encoder_impl.h` to improve compilation performance and modularity.
- Added `src/raii.h` with RAII helpers (`OmpLock`, `OmpLockGuard`, `MmapView`); migrated `src/decompress.cpp`, `src/reorder_impl.h`, and `src/encoder_impl.h` to use RAII for `mmap` and OpenMP locks, and adopted RAII containers (`std::vector`, `std::unique_ptr`, `std::array`) across preprocessing and hot-path modules (e.g., `src/preprocess.cpp`, buffer and stream ownership in `src/decompress.cpp`) to replace manual `new[]/delete[]`, manage gzip streams, and improve resource safety.
- Hardened filesystem cleanup and temporary-file handling: added non-throwing `safe_remove_file()` and `safe_rename_file()` in `src/util.h`/`src/util.cpp` and replaced unchecked `remove()`/`rename()` call sites across `src/` with these helpers; added `src/scoped_temp_file.h` for noexcept temporary-file cleanup and made mapped-file RAII (`MmapView`) destructor `noexcept` to ensure safe unmapping during stack unwinding.
- Replaced hot-path raw arrays with `std::vector` or `std::unique_ptr<T[]>` starting in `src/reorder_impl.h` and `src/encoder_impl.h` to improve memory safety and eliminate manual `new[]/delete[]` usage.
- Replaced manual `char**` decoded noise table in `src/decompress.cpp` with `std::array<std::array<char,128>,128>`, removing manual `new[]/delete[]` and ensuring RAII-managed lifetime.
- Refactored `src/reorder.h` and the corresponding reordering implementation in `src/reorder_impl.h` to reduce template bloat and improve compilation speed.
- Modernized memory management in `bbhashdict` (the core read dictionary) by replacing manual `new[]/delete[]` with `std::unique_ptr` and `std::make_unique`.
- Refactored the `main.cpp` entry point to use a centralized RAII `SpringContext` class for managing temporary directories and resource cleanup, significantly reducing global state and improving signal-safety.
- Improved development documentation in `DEVELOPMENT.md` with a new section on unit testing.
- Refactored the monolithic `util.h` and `util.cpp` into a modular, domain-specific architecture:
  - `src/io_utils.h/cpp`: Specialized for C-SiT ID compression and robust binary I/O.
  - `src/dna_utils.h/cpp`: Centralized DNA sequence manipulation (reverse complement, bit-packing, N-read encoding).
  - `src/parse_utils.h/cpp`: Handle ID pattern matching, FASTQ block parsing, and numeric string conversions.
  - `src/fs_utils.h/cpp`: Granular filesystem helpers and RAII file management.
  - `src/params.h/cpp`: Serialization of compression parameters and metadata structures to resolve circular dependencies.
- Refactored the core decompression engine in `src/decompress.cpp` and `src/decompress.h` into a stateful, sink-based architecture to support the new streaming reader while maintaining bit-perfect CLI backward compatibility.
- Upgraded the CLI verbosity system from a binary toggle to explicit log levels: default quiet mode keeps the progress bar, `--verbose`/`--verbose info` enables informational logs, and `--verbose debug` enables both informational and detailed debug diagnostics.
- Pruned indexed_bzip2 even more.
- Made windows build process guide easier and more straightforward.
- Expanded CMake runtime portability packaging: improved Windows GCC/OpenMP runtime DLL resolution and bundling for `spring2`, `spring2-preview`, and `rapidgzip`; added install-time RPATH configuration and OpenMP runtime library bundling support for macOS (`@loader_path/../lib`) and Linux (`$ORIGIN/../lib`).
- Consolidated all third-party dependency licenses into the central root `LICENSE` file for improved legal compliance and audit-readiness.

### Fixed

- Removed debug `[GZIP-DIAG]` logs from the compression pipeline that were firing even in non-verbose mode.
- Consolidated `parse_int_or_throw`, `parse_double_or_throw`, and `parse_uint64_or_throw` into `src/util.h` and `src/util.cpp` to remove duplication and potential ODR hazards.
- Consolidated `has_suffix` into `src/util.h` and `src/util.cpp`, removing duplicate definitions in `src/spring.cpp` and `src/decompress.cpp`.
- Removed a redundant duplicate call to `generatemasks` in the encoder initialization.
- Fixed an issue in `src/spring.cpp` where missing archive metadata could lead to silent decompression failures by adding validation for inferred output paths.
- Removed redundant dead code in decompression IO resolution logic.
- Fixed confusing error output in `src/main.cpp` by separating error handling for parameter validation errors (show help) from true runtime errors (show error message only); this ensures runtime failures are not obscured by help text.
- Hardened temporary directory cleanup in `SpringContext::cleanup()` by adding path canonicalization and boundary validation to prevent accidental recursive deletion if internal state is corrupted, and improved diagnostic logging for success/failure conditions.
- Eliminated dictionary lock contention during reorder search by implementing lock-free read-only access in `src/bitset_dictionary.h` and `src/reorder_impl.h`: after dictionary construction completes, `freeze()` marks the dictionary immutable, allowing all search operations to proceed without acquiring dictionary locks; this reduces lock wait time from ~168s to ~0, expected to improve reorder stage wall-clock by 18-25%.
- Fixed benchmark scripts (`tests/bench/small_bench.ps1` and `tests/bench/small_bench.sh`) to properly handle FASTQ files with IDs on the "+" quality separator line: FASTQ spec allows both "+ID" and "+" formats; SPRING2 normalizes to "+" (the canonical form) during compression. Benchmark scripts now normalize both original and restored files before comparison to ensure bit-perfect verification succeeds regardless of input "+" line format.

## V1.0.0-alpha

### Added

- Formally rebranded the software to **SPRING2**.
- Renamed the project and executable binary to `spring2`.
- Added a robust CMake `install` target to create a clean, portable distribution footprint (e.g., in a `dist/` directory) containing only final binaries and required runtime libraries.
- Added benchmark scripts under `benchmark/` for lossless round-trip runs, comparison runs, and larger manual benchmarking workflows.
- Added round-trip integrity verification to the lossless benchmark flow, including checksum reporting when hashing tools are available.
- Added automatic support for gzipped compression inputs by staging `.gz` inputs into the temporary working directory before normal compression.
- Added automatic short-read versus long-read mode detection by pre-scanning input sequence lengths before compression.
- Added the `--memory` (`-m`) CLI option to conservatively reduce effective worker count on memory-constrained systems.
- Added a unified `-s, --strip [ioq]` CLI flag to discard identifiers (`i`), order (`o`), and quality scores (`q`), replacing independent flags.
- Added vendored `libdeflate` for fast whole-buffer DEFLATE, zlib, and gzip workloads used by the current build.
- Added vendored `rapidgzip` support for gzipped compression inputs through the pruned `indexed_bzip2` payload.
- Added a dedicated developer-tooling area for repository maintenance, including linting, cppcheck, Docker workflows, and related validation helpers.
- Added configure-time `.clangd` generation so editor diagnostics inherit the active compiler's include paths and OpenMP configuration.
- Added extensive documentations.
- Added storage of the original input filenames and detailed gzip metadata (BGZF block size, header flags, MTIME, OS, and internal filename) within the compressed archive metadata.
- Added the `spring2-preview` utility for inspecting archive metadata, read counts, settings, and detailed compression ratios without full decompression.
- Added the `-n, --note` flag to store custom text notes within the archive.
- Added the `-u, --unzip` flag to force uncompressed output during decompression, even if the original input was gzipped.
- Added a specialized `-v, --verbose` flag to toggle between a real-time progress bar (default) and detailed informational logging.

### Changed

- Replaced the unmaintained legacy `id_comp` module with a natively integrated **Columnar Specialized Identifier Coder (C-SiT)** backed by Zstd (Level 22 max-compression). C-SiT dynamically parses FASTQ machine headers into dedicated columnar streams. For tile coordinates, it leverages an advanced auto-detecting Byte-Shuffled Delta Encoder that shrinks numeric identifiers into overlapping low-entropy sequences.
- Improved benchmark reporting so compression and decompression runs report elapsed time, CPU time, average core usage, and peak RSS when supported by the host environment.
- Changed the default thread selection logic to `min(max(1, hw_threads - 1), 16)`.
- Changed decompression output handling so output paths ending in `.gz` automatically produce gzipped FASTQ output.
- Replaced the `--gzip-level` option with a unified `-l, --level` flag (range 1–9, default 6). Values are passed to gzip for compressed output and scaled to Zstd (1–22) for internal streams.
- Renamed several core CLI flags for clarity and standard usage: `--num-threads` to `--threads` (`-t`), `--input-file` to `--input` (`-i`), `--output-file` to `--output` (`-o`), `--working-dir` to `--tmp-dir` (`-w`), and `--quality-opts` to `--qmod` (`-q`).
- Transitioned the recommended archive file extension from `.spring` to `.sp`.
- Removed the obsolete `--gzipped-fastq`, `--fasta-input`, and manual `-l` (long-read) flags.
- Repackaged `indexed_bzip2` into a smaller SPRING-specific archive payload that retains only the pieces needed for the current gzip workflow.
- Removed the final Boost dependency from the build and runtime path by replacing the remaining Boost-based gzip and mapped-file usage with local implementations.
- Upgraded the project toolchain baseline to C++20 and CMake 4.2.
- Refreshed the vendored dependency set used by the current tree, including `libbsc`, Cloudflare zlib, `libdeflate`, and the pruned `indexed_bzip2` payload.
- Replaced the unmaintained BBHash with a patched version of PTHash (making it more compatible with windows) for the hash table implementation.
- Made vendor extraction idempotent so repeated configure runs only re-extract archives when their content hash changes.
- Standardized formatting with the repository `.clang-format` configuration.
- Renamed several core source files to clearer role-based names, including `bitset_dictionary`, `template_dispatch`, `paired_end_order`, `reordered_quality_id`, and `reordered_streams`.
- Reorganized the internal implementation to reduce duplicated scaffolding across compression, decompression, preprocessing, template dispatch, and other core subsystems while preserving behavior.

### Fixed

- Removed the previous Unix-only build assumption and enabled the modernized build and CI flow on Windows through a native MinGW-w64 toolchain.
- Fixed empty-reference-chunk decompression failures by switching the decoded chunk path to the local mapping wrapper.
- Reduced decompression peak memory usage by avoiding reconstruction of the full decoded reference in one large in-memory string.
- Reduced decompression write overhead by switching packed-sequence decode output to buffered block writes.
- Reduced compression-side sequence-packing overhead by eliminating the extra prepass and moving to buffered reads and writes.
- Reduced quality and identifier staging overhead by partitioning in a single pass and reloading batches more efficiently.
- Improved compiler portability by adding missing standard-library includes, replacing non-standard integer aliases with standard types, and fixing newer GCC compatibility issues in both first-party and vendored code.
- Fixed thread-count validation and related allocation hazards in CLI and internal helper paths.
- Fixed binary quality range validation for `-q binary` to avoid out-of-range table access and associated compiler warnings.
- Cleaned up lint and cppcheck findings across the current codebase, including missing initialization, binary I/O casts, signed-shift portability, and exception-safety issues.
- Tightened targeted lint-path handling and Python-file validation in the developer tooling.
- Stabilized the Valgrind smoke workflow by focusing failures on actionable leak classes and suppressing known OpenMP and libc startup noise.
