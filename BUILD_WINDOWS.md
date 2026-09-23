# Windows Build Guide

NInfer builds on Windows 11 with MSVC + CUDA + Ninja. Dependencies (FFmpeg, libcurl)
are managed with vcpkg in manifest mode through the repository's `vcpkg.json`. The build
produces three executables in `build-win\apps\`: `ninfer` (CLI), `ninfer-serve` (HTTP
server), and `ninfer-perplexity` (evaluation). vcpkg copies the FFmpeg and curl DLLs next
to them, so run the executables from that directory tree.

## Prerequisites

| Component | Requirement | Notes |
|---|---|---|
| Windows | 11 (x64) | |
| Visual Studio 2022 | Community or higher | Desktop development with C++ workload (MSVC v143 toolset, 14.4x) |
| CUDA toolkit | 13.1+ (13.1 validated; 13.3 builds) | Must support `sm_120a`; RTX 5090 target |
| Ninja | 1.13+ | `winget install Ninja-build.Ninja` |
| CMake | 3.28+ | `winget install Kitware.CMake` |
| Git | Any | |
| vcpkg | 2026-07-27+ | See step 2 |
| Python 3 | 3.11+ (for the upgrade tool only) | For `tools/upgrade_ninfer_v2_to_v3.py` |

## Step 1 — Visual Studio developer environment

Every CMake command below (configure **and** build) needs the MSVC environment variables
(`INCLUDE`, `LIB`, `PATH`). Run them from an "x64 Native Tools Command Prompt for VS 2022", or
call `vcvars64.bat` first:

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

For a Build Tools installation, replace `Community` with `BuildTools`. From PowerShell or the
VS Code terminal, prefix each command with the vcvars call:

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build build-win -j'
```

## Step 2 — vcpkg (dependencies)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
```

`vcpkg.json` requests FFmpeg with `avcodec`, `avformat`, `swscale` and `zlib`. The `zlib`
feature is required for PNG decoding; without it PNG image inputs fail with
`invalid_media: media codec is not supported` while JPEG, WebP and video still work.

## Step 3 — Configure

```powershell
cd C:\AI\ninfer
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_CUDA_ARCHITECTURES=120a -DCMAKE_BUILD_TYPE=Release
```

or, with `VCPKG_ROOT` set, the equivalent preset:

```powershell
cmake --preset windows-vcpkg
```

The first configuration compiles FFmpeg and curl from source (about 15 minutes). Later
configurations restore them from the local vcpkg binary cache in seconds. Changing the features in
`vcpkg.json` triggers a rebuild of the affected package on the next configure.

Keep `BUILD_TESTING` at its default `OFF`. Several tests use POSIX-only headers (`unistd.h`) and do
not compile with MSVC, so a tree configured with `-DBUILD_TESTING=ON` fails a plain
`cmake --build`. To build selected tests in such a tree, name them with `--target`.

## Step 4 — Build

```powershell
cmake --build build-win -j
```

## Step 5 — Run

```powershell
# CLI one-shot
.\build-win\apps\ninfer.exe <model.ninfer> --prompt "Hello" --max-new 64

# Server (text only)
.\build-win\apps\ninfer-serve.exe <model.ninfer> --host 127.0.0.1 --port 8080

# Server accepting image and video input
.\build-win\apps\ninfer-serve.exe <model.ninfer> --host 127.0.0.1 --port 8080 --vision
```

Then call the OpenAI-compatible API at `http://127.0.0.1:8080/v1/chat/completions`. Without
`--vision`, image and video requests return HTTP 400 `vision_disabled`. The request `model` must
match the id reported by `GET /v1/models`; use `--model-id` to serve under another name, for example
the alias a client already uses for llama.cpp. See [HTTP serving](docs/serving.md) for the full
protocol.

## Upgrading a v2 artifact

NInfer v3 requires v3 `.ninfer` artifacts. To upgrade one of the official v2 artifacts locally:

```powershell
python tools/upgrade_ninfer_v2_to_v3.py input_v2.ninfer output_v3.ninfer
```

## Troubleshooting

| Problem | Solution |
|---|---|
| `fatal error C1083: Cannot open include file: 'chrono'` | The terminal lacks the MSVC environment. Run `vcvars64.bat` first (step 1). |
| `fatal error C1083: Cannot open include file: 'unistd.h'` in `tests\` | The tree was configured with `BUILD_TESTING=ON`. Reconfigure with `-DBUILD_TESTING=OFF`, or build with `--target ninfer ninfer-serve ninfer-perplexity`. |
| `Could NOT find PkgConfig` | Windows has no pkg-config. Configure with the vcpkg toolchain (step 3). |
| `Could NOT find FFMPEG` / missing `vcpkg.json` | Configure from the repository root, where `vcpkg.json` lives, with the vcpkg toolchain file. |
| `no version database entry for curl` | The vcpkg baseline is stale. Run `git -C C:\vcpkg fetch origin master` and update `builtin-baseline` in `vcpkg.json`. |
| PNG input returns `media codec is not supported` | FFmpeg was built without `zlib`. Check `vcpkg.json` and reconfigure (step 3). |
| `NOMINMAX` / `min`/`max` errors | `CMakeLists.txt` defines `NOMINMAX` on Windows; make sure you configure from the repository root. |
| UTF-8 console output garbled | The embedded `windows-utf8.manifest` and console setup in each app handle this automatically. |

## Performance notes

Published measurements use an RTX 5090. The build targets `sm_120a` and rejects other CUDA
architectures. CUDA 13.1 is the validated toolkit; CMake does not impose a version floor.
