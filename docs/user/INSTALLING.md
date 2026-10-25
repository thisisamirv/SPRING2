# Installing SPRING2

This page covers installing SPRING2 from prebuilt release artifacts. If you want to compile from source instead, see [Building SPRING2](BUILDING.md).

## Download Releases

Download platform artifacts from the GitHub releases page:

- <https://github.com/thisisamirv/SPRING2/releases>

Available release artifacts currently include:

- Linux AppImages for `x86_64` and `ARM64`
- macOS `.app` bundles packaged as zip archives for `arm64` and `x86_64`
- Windows standalone executables for `x86_64` and `ARM64`

## Conda (bioconda)

If you are using conda environments, install SPRING2 with:

```bash
conda install -c bioconda spring2
```

Optional: create a dedicated environment first.

```bash
conda create -n spring2-env -c bioconda spring2
conda activate spring2-env
spring2 --version
```

## Linux

Linux releases provide portable AppImages.

### AppImage

1. Download the AppImage matching your architecture.
2. Mark it executable.
3. Run it directly.

Example:

```bash
chmod +x spring2-linux-x86_64.AppImage
./spring2-linux-x86_64.AppImage --version
```

If you prefer, you can keep the AppImage in a tools directory and invoke it from there.

## macOS

macOS releases provide `.app` bundles packaged as zip archives per architecture.

- Download the zip for your architecture: - `spring2-macos-arm64.app.zip` - `spring2-macos-x86_64.app.zip`
- Extract the zip file.
- Run SPRING2 from Terminal:

    ```bash ./SPRING2.app/Contents/MacOS/spring2 --version ```

If you prefer, move `SPRING2.app` to `/Applications` and invoke it from there.

## Windows

Windows releases provide standalone executables per architecture.

1. Download the executable for your architecture: - `spring2-windows-x86_64.exe` - `spring2-windows-arm64.exe`

2. Rename to `spring2.exe` (optional) and place it in a folder on your `PATH`.

3. Open a new terminal and run:

    ```powershell spring2 --version ```
