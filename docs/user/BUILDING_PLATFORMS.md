<!-- markdownlint-disable MD033 -->

# SPRING2 Platform Build Commands

This page contains the concrete build commands for each supported host environment. For requirements and build strategy, start with [BUILDING.md](BUILDING.md).

## Linux

Install build requirements:

```bash
sudo apt-get update
sudo apt-get install -y build-essential libomp-dev python3 python3-pip
python3 -m pip install --upgrade --user pip
python3 -m pip install --user "cmake==4.2.0"
```

If the Python user binary directory is not on `PATH`, add it before invoking CMake:

```bash
export PATH="$(python3 -m site --user-base)/bin:$PATH"
```

Configure, build, and install:

```bash
cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix out
```

## macOS

Install Xcode Command Line Tools if needed:

```bash
xcode-select --install
```

Install dependencies:

```bash
brew update
brew install libomp python llvm cppcheck
python3 -m pip install --upgrade --user pip
python3 -m pip install --user "cmake==4.2.0"
```

If the Python user binary directory is not on `PATH`, add it before invoking CMake:

```bash
export PATH="$(python3 -m site --user-base)/bin:$PATH"
```

Build with Apple Clang and Homebrew OpenMP:

```bash
CC=clang CXX=clang++ cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix out
```

## Windows (native MinGW-w64)

Install a native UCRT toolchain and CMake:

```powershell
winget install --id BrechtSanders.WinLibs.MCF.UCRT -e
winget install --id Kitware.CMake -e
winget install --id Git.Git -e
```

Set compiler variables in PowerShell:

```powershell
$env:CC = "gcc"
$env:CXX = "g++"
```

Configure, build, and install:

```powershell
cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix out
```

The installed binary and runtime DLLs land in `out/bin/`.

## Windows (Visual Studio / MSVC)

Configure with the Visual Studio generator:

```powershell
cmake -S . -B out/build-msvc -G "Visual Studio 18 2026" -A x64
cmake --build out/build-msvc --config Release --parallel
cmake --install out/build-msvc --config Release --prefix out
```

## Windows (ClangCL)

Configure ClangCL on top of the Visual Studio generator:

```powershell
cmake -S . -B out/build-clangcl -G "Visual Studio 18 2026" -A x64 -T ClangCL
cmake --build out/build-clangcl --config Release --parallel
cmake --install out/build-clangcl --config Release --prefix out
```

## Notes

- CMake 4.2 or newer is required.
- A C++20-capable compiler is required.
- OpenMP is required.
- Vendored Ninja and NASM are used automatically from `tools/host/` when available on supported hosts.
- For a machine-tuned build, pass `-Dspring_optimize_for_native=ON -Dspring_optimize_for_portability=OFF`.
