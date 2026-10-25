<!-- markdownlint-disable MD033 -->

# Building SPRING2

This page is the entry point for building SPRING2 from source.

## Requirements

SPRING2 currently requires:

- A C++20-capable compiler
- CMake 4.2 or newer
- OpenMP

Supported compiler frontends include:

- GCC
- Clang / Apple Clang
- MSVC

Current CI validation covers Linux, macOS, and native Windows builds.

## Get the Source

```bash
git clone https://github.com/thisisamirv/SPRING2.git
cd SPRING2
```

## Choose a Build Style

By default, SPRING2 builds for portability:

```text
-Dspring_optimize_for_native=OFF
-Dspring_optimize_for_portability=ON
```

Use that default when you want binaries that run across a broader range of machines. For a machine-specific build, switch those flags:

```text
-Dspring_optimize_for_native=ON
-Dspring_optimize_for_portability=OFF
```

## Output Layout

- Build artifacts go under `out/build/`.
- Installed binaries go under `out/bin/` by default unless you provide a custom prefix.
- On supported hosts, vendored Ninja and NASM under `tools/host/` are used automatically when available.

## Platform Commands

Use the platform-specific command recipes here:

- [Platform Build Commands](BUILDING_PLATFORMS.md)

That guide contains separate sections for:

- Linux
- macOS
- Windows with native MinGW-w64
- Windows with MSVC or ClangCL

## Editor Tooling

If you want a compilation database for editor integration, build once and use `out/build/compile_commands.json`.

## Installation Alternative

If you do not need to build from source, prefer the prebuilt binaries and installers documented in [Installing SPRING2](INSTALLING.md).
