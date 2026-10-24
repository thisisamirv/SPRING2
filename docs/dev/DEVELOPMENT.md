# Maintaining and Developing SPRING2

This guide is for developers who want to contribute to SPRING2 or maintain the build system and delivery pipelines.

## Project Structure

- `src/`: Core C++20 source files and headers.
- `cmake/`: Root build modules included by the top-level `CMakeLists.txt`.
- `vendor/`: Bundled third-party dependencies as `.tar.xz` archives.
- `docs/user/`, `docs/dev/`, `docs/assays/`: User, developer, and assay-specific documentation.
- `tools/dev/`: Developer workflow entrypoints for linting and smoke checks.
- `tools/assay-reference/`: Checked-in assay-detection reference assets consumed by code generation and real-data tests.
- `tools/codegen/`: Native build-time generators and other source-producing helpers.
- `tools/host/`: Vendored host tools such as Ninja and NASM.
- `tests/data/`: Checked-in sample inputs and archive fixtures used by unit, integration, smoke, and benchmark coverage.
- `tests/`: Unit tests, integration tests, smoke tests, benchmarks, fixtures, and test support headers.

## Build System Architecture

SPRING2 uses a modular CMake build system. The root `CMakeLists.txt` is a thin orchestrator; project logic lives in `cmake/*.cmake` (toolchain, OpenMP, host tools, vendors, targets, install, tests). Each vendor dependency in the `vendor/` directory is self-contained:

- Dependencies are extracted on-the-fly during CMake configuration.
- Each major dependency (e.g., `libbsc`, `cloudflare_zlib`) has its own internal `CMakeLists.txt` managing its build logic.
- All internal libraries are built with `POSITION_INDEPENDENT_CODE ON` to ensure compatibility with enterprise Linux (PIE) requirements.

## Quality Control

### Linting

We use `clang-format` and several custom lint scripts. The cross-platform lint entrypoint is:

```bash
python tools/dev/lint/lint.py
```

The cross-platform cppcheck entrypoint follows the same pattern:

```bash
python tools/dev/lint/cppcheck.py
```

For Linux-only leak checking, the valgrind smoke entrypoint is:

```bash
python tools/dev/smoke/valgrind_smoke.py
```

Run at least one build before linting so `out/clangd/compile_commands.json` exists, or copy it from the active build tree if you configured without building.

### Compiler Cache (Faster Rebuilds)

For faster incremental development builds, SPRING2 supports compiler cache launchers during CMake configure:

- Windows: `sccache`
- Linux/macOS: `ccache` (falls back to `sccache` if `ccache` is unavailable)

This behavior is enabled by default with `-DSPRING_ENABLE_COMPILER_CACHE=ON`.

Install suggestions:

- Linux (Debian/Ubuntu): `sudo apt-get install -y ccache`
- macOS (Homebrew): `brew install ccache`
- Windows (winget): `winget install --id Mozilla.sccache -e`

Disable compiler cache explicitly (if needed):

```bash
cmake -S . -B out/build -G Ninja -DSPRING_ENABLE_COMPILER_CACHE=OFF
```

Check cache stats:

- `ccache -s` (Linux/macOS)
- `sccache --show-stats` (Windows, or any platform using `sccache`)

### Faster Linker (Not on macos)

SPRING2 can also select a faster linker during CMake configure, enabled by default with `-DSPRING_ENABLE_FAST_LINKER=ON`.

Selection order:

- Linux: tries `mold` first (`-fuse-ld=mold`), then `lld` (`-fuse-ld=lld`)
- Windows (GNU/Clang toolchains): tries `lld` when supported

Install suggestions:

- Linux (Debian/Ubuntu): `sudo apt-get install -y mold lld`
- Windows (MSYS2/MinGW-w64 UCRT): `pacman -S --needed mingw-w64-ucrt-x86_64-lld`

If no compatible fast linker is available, the build falls back to the platform default linker automatically.

Disable this behavior explicitly (if needed):

```bash
cmake -S . -B out/build -G Ninja -DSPRING_ENABLE_FAST_LINKER=OFF
```

### Precompiled Headers (PCH)

SPRING2 uses precompiled headers to accelerate incremental rebuilds, enabled by default with `-DSPRING_ENABLE_PRECOMPILED_HEADERS=ON`.

The precompiled header (`src/pch.h`) includes stable, frequently-used headers:

- Standard library containers (`<vector>`, `<string>`, `<array>`)
- I/O utilities (`<iostream>`, `<fstream>`, `<iomanip>`)
- File system and concurrency (`<filesystem>`, `<mutex>`, `<thread>`, `<atomic>`)
- OpenMP (`<omp.h>`)

When enabled, CMake compiles this header once and caches it, significantly reducing compile time on subsequent rebuilds. The improvement is especially noticeable after touching a few files or after clean configuration, where PCH cache is reused across translation units.

Disable precompiled headers explicitly (if needed):

```bash
cmake -S . -B out/build -G Ninja -DSPRING_ENABLE_PRECOMPILED_HEADERS=OFF
```

### Unit and Integration Tests

We use the **doctest** framework for both granular unit testing and high-level integration testing. You can build and run all tests using standard CMake/CTest workflows:

```bash
cmake -S . -B out/build
cmake --build out/build --parallel --target unit-tests integration-tests smoke-tests
ctest --test-dir out/build -C Debug --output-on-failure
```

If you already built only the main executable, rebuild the test targets before invoking `ctest`:

```bash
cmake --build out/build --parallel --target unit-tests integration-tests smoke-tests
```

That same `out/build` tree is also the default source for `out/clangd/compile_commands.json` and the smoke/bench helper defaults.

- **Unit Tests**: `tests/unit/*.cpp` (Split by low-level helpers and assay detection behavior).
- **Integration Tests**: `tests/integration/*.cpp` (Split by archive integrity, reader behavior, assay workflows, and grouped archive scenarios).
- **Smoke Tests**: `tests/smoke/*.cpp` (Split C++ CLI behavioral validation executed through the `smoke-tests` binary).
- **Determinism**: `tests/smoke/determinism_smoke_tests.cpp` asserts bit-identical dual compress for short-read (`test_3`) and long-read (`sample.fastq`) paths at `-t 1` and multi-thread, and that short-read multi-thread decompresses to the same mates as `-t 1`.

### Archive Metadata Version Compatibility

SPRING2 treats archive creator versioning as part of the compatibility contract:

- New archives write a metadata header version and preserve the exact creator version string in `cp.bin`.
- Older archives that predate the metadata header are still supported and default to creator version `1.0.0-rc.1` when no explicit version string is present.
- User-facing preview output should show the effective human-readable archive version and should not expose internal compatibility-routing labels unless you are debugging the implementation.
- Future decompression-path changes should continue to route by parsed archive version, not by a coarse legacy/current split.

There are three layers to keep straight:

1. **`cp.bin` format version** (0 headerless / 1 peek trailer / 2 length-prefixed trailer).
2. **Creator semver** stored in metadata (`1.0`–`1.3` supported; `2.0.0` rejected).
3. **Container side-cars** (`integrity.meta` / `bundle.meta` version strings). Absence of `integrity.meta` warns and skips; unknown side-car versions must fail closed.

**Parser coverage** lives in `tests/unit/archive_metadata_version_unit_tests.cpp`. Update that test first when metadata layout, version parsing, preview version reporting, or decompression routing changes.

**End-to-end decompress coverage** lives in `tests/data/compat/` and
`tests/integration/compat_archive_integration_tests.cpp`. The TSV manifest lists
frozen archives (legacy Spring `*.spring_v1` plus SPRING2 `compat/<version>/*.sp`)
and the checks to run (`decompress`, `preview`, `audit`, `reader`). Mint a new
SPRING2 golden set before tagging a release:

```bash
python tools/dev/mint_compat_fixtures.py --spring2 path/to/spring2
```

Never overwrite an older `compat/<version>/` tree with a newer binary. Commit the
new directory and the updated `manifest.tsv` with the release.

### Benchmark/Test Scripts

The `tests/` directory also includes benchmark and comparison scripts. On GitHub,
the manual **Benchmark** workflow (`.github/workflows/benchmark.yml`,
`workflow_dispatch`) builds SPRING2 and runs `small_bench.py`, `big_bench.py`,
and `comparison_bench.py`.

- `tests/bench/big_bench.py`: End-to-end paired-end benchmark on the SRR8185389 dataset. Pass `--no_debug` to suppress SPRING's `-v debug` output during the run.
- `tests/bench/small_bench.py`: Faster benchmark variant for quick local performance checks and assay-specific benchmark coverage.
- `tests/bench/comparison_bench.py`: Compares SPRING2 against SPRING1 and Python gzip baselines (needs a `spring_v1` conda env with `bioconda::spring`).
- `tests/bench/big_thread_test.py`: Debug-oriented thread-count stress benchmark for CRC/integrity investigations (manual only; not in the Benchmark workflow).
- `tests/smoke/*.cpp`: Lightweight CLI sanity checks split into themed smoke test units.
- `tests/integration/*.cpp`: Archive round-trip and API-level integration coverage split into smaller themed units.
- `tests/unit/*.cpp`: Focused tests for low-level helpers and assay detection behavior.

### Diagnostic Key Legend

Many debug logs use a normalized key schema for fast triage:

- `block_id`: Pipeline stage or thread/block identifier that emitted the log.
- `path`: File path or logical input source being checked.
- `expected_bytes`: Expected size/count/bound for the check.
- `actual_bytes`: Observed size/count/value at runtime.
- `index`: Loop position, record number, or block-local offset where the check failed.

Interpretation tips:

- `expected_bytes=1, actual_bytes=0`: Usually open/availability failure (resource not usable).
- `actual_bytes < expected_bytes`: Typical short read/truncation signal.
- `actual_bytes > expected_bytes`: Often out-of-range metadata/value for bound checks.
- `index` pinpoints the offending element; combine with `block_id` to locate the stage.

## Release Pipeline

The project uses GitHub Actions for automated multi-architecture releases.

- **Runners**: Uses Linux, macOS, and Windows GitHub-hosted runners for `x86_64` and `arm64` targets.
- **Environment**: Linux builds run inside **AlmaLinux 8** Docker containers to pin the GLIBC baseline to **2.28**, ensuring portability across enterprise distributions (RHEL 8+, Ubuntu 20.04+, etc.). macOS and Windows builds run natively on their respective hosted runners.
- **Artifacts**:
  - Linux: AppImage per architecture (`spring2-linux-*.AppImage`)
  - macOS: zipped `.app` bundles per architecture (`spring2-macos-*.app.zip`)
  - Windows: standalone executable per architecture (`spring2-windows-*.exe`)
