# Windows Build Guide

NInfer builds on Windows 11 with MSVC + CUDA + Ninja. Dependencies (FFmpeg, libcurl)
are managed with vcpkg in manifest mode. The build produces three executables:
`ninfer` (CLI), `ninfer-serve` (HTTP server), and `ninfer-perplexity` (evaluation).

## Prerequisites

| Component | Requirement | Notes |
|---|---|---|
| Windows | 11 (x64) | |
| Visual Studio 2022 | Community or higher | Desktop development with C++ workload, MSVC v144 |
| CUDA toolkit | 13.1+ (validated) | Must support `sm_120a`; RTX 5090 target |
| Ninja | 1.13+ | Available via `winget install Ninja` |
| CMake | 3.28+ | Available via `winget install CMake` |
| Git | Any | |
| vcpkg | 2026-07-27+ | See step 2 |
| Python 3 | 3.11+ (for upgrade tool only) | For `tools/upgrade_ninfer_v2_to_v3.py` |

## Step 1 — Visual Studio Developer Prompt

The build requires MSVC environment variables (`INCLUDE`, `LIB`, `PATH`).
Always run from a Developer Command Prompt:

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
```

Or for BuildTools:

```
"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

**Important**: If you are running CMake from a generic terminal (PowerShell, VS Code
integrated terminal), prefix every build command with the vcvars call:

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake ...'
```

## Step 2 — vcpkg (dependencies)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
```

## Step 3 — Configure

```powershell
cd C:\AI\ninfer
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_CUDA_ARCHITECTURES=120a -DCMAKE_BUILD_TYPE=Release
```

First configuration: vcpkg compiles FFmpeg and curl from source (~15 minutes).
Subsequent configurations restore them from the local binary cache in seconds.

## Step 4 — Build

```powershell
cmake --build build-win -j
```

## Step 5 — Run

```powershell
# CLI one-shot
.\build-win\apps\ninfer.exe <model.ninfer> --prompt "Hello" --max-new 64

# Server
.\build-win\apps\ninfer-serve.exe <model.ninfer> --host 127.0.0.1 --port 8080
```

Then call the OpenAI-compatible API at `http://127.0.0.1:8080/v1/chat/completions`.

## Upgrading a v2 artifact

NInfer v3 requires v3 `.ninfer` artifacts. To upgrade a v2 artifact locally:

```powershell
python tools/upgrade_ninfer_v2_to_v3.py input_v2.ninfer output_v3.ninfer
```

## Project structure

```
ninfer/
├── build-win/              # Build tree (generated)
│   ├── apps/               # Executables + DLLs
│   │   ├── ninfer.exe
│   │   ├── ninfer-serve.exe
│   │   └── ninfer-perplexity.exe
│   └── vcpkg_installed/    # vcpkg dependencies
├── src/                    # C++/CUDA source
├── apps/                   # Entry points
├── cmake/                  # CMake modules
├── docs/                   # Documentation
├── vcpkg.json              # Dependency manifest
└── CMakePresets.json       # Build presets
```

## Troubleshooting

| Problem | Solution |
|---|---|
| `fatal error C1083: Cannot open include file: 'chrono'` | Terminal is not a VS Developer prompt. Run `vcvarsall.bat x64` first. |
| `Could NOT find PkgConfig` | Windows doesn't have pkg-config. Use the vcpkg toolchain (step 3). |
| `no version database entry for curl` | vcpkg baseline is stale. Run `git -C C:\vcpkg fetch origin master` and update `builtin-baseline` in `vcpkg.json`. |
| `NOMINMAX` / `min/max` errors | Ensure `NOMINMAX` compile definition is set (handled by `CMakeLists.txt` on Windows). |
| `__int128` not defined | Handled by `src/core/math_util.h` which uses `_umul128` on MSVC. |
| UTF-8 console output garbled | The `windows-utf8.manifest` and `ConsoleUtf8Setup` handle this automatically. |

## Performance notes

Published measurements use an RTX 5090. The build targets `sm_120a` and rejects
other CUDA architectures. CUDA 13.1 is the validated toolkit; CMake does not
impose a version floor.
