# NInfer fork: video and audio in ninfer-serve

This fork (`Randy420Marsh/ninfer`) tracks upstream `Neroued/ninfer`. It adds long-video and audio input to
`ninfer-serve`. The design and request format match the llama.cpp fork (`Randy420Marsh/llama.cpp`), so a
client written for one works with the other.

## Pages

- [Video and audio](Video-and-Audio): user guide covering the endpoints, request fields, server flags,
  budgets, seeking, transcripts, the tested model and errors.
- [Media pipeline internals](Media-Pipeline-Internals): how a video part becomes Vision input, the design
  choices behind it, and the changed files.

## What the fork adds

| Area | Addition |
|---|---|
| Requests | Chat Completions `video_url` / `input_video` parts take time ranges (`start`, `end`, `duration`, `segments`), `fps`, `detail`, a per-part budget, an `audio` mode and a `language` hint. `input_audio` / `audio_url` parts become transcripts |
| Sources | HTTP(S) URLs are streamed with range requests: a 10 s range 10.5 h into a 199 MiB file downloaded 16 MiB. `file://` paths under `--media-path` and inline base64 also work |
| Budget | The frame rate drops to fit `--video-max-tokens` / `--video-max-frames`, and resolution stays at the chosen detail. The 1080p 60 s test video costs 25k tokens instead of 247k |
| Long segments | A segment larger than the model's 16,384-token single-item limit is sent as consecutive clips |
| Readability | Same detail presets as the llama.cpp fork (`low` / `standard` / `high` / `max`), calibrated so on-screen text stays readable |
| Dedup | Frame pairs in which no screen region changed are dropped (`--video-dedup`) |
| Timestamps | Frames and transcript lines carry source time. The Qwen processor now takes frame times from the container |
| Audio | Timestamped transcripts from an OpenAI-compatible speech-to-text server (`--asr-url`) |
| Safety | Remote URLs must resolve to public addresses, with every redirect checked, unless `--media-allow-private-urls` is set. Only plain container formats are opened |
| Build | [Windows build guide](https://github.com/Randy420Marsh/ninfer/blob/master/BUILD_WINDOWS.md) moved to the repo root and corrected. New `vcpkg.json` manifest for the Windows build, with FFmpeg's zlib feature so PNG images decode |

Everything runs in-process with libav. Clips are cut into in-memory FFV1/Matroska buffers, audio is
resampled in memory, and nothing is written to disk.

## Quick start

```powershell
ninfer-serve model.ninfer --max-context 160000 --vision `
  --media-path C:\media --asr-url http://127.0.0.1:8178
```

```json
{"role": "user", "content": [
  {"type": "video_url", "video_url": {"url": "file://talk.mp4", "start": "01:00:00", "end": "01:05:00"}},
  {"type": "text", "text": "Summarize this part and quote what is said about the budget."}
]}
```

`ninfer-serve --help` and [docs/serving.md](https://github.com/Randy420Marsh/ninfer/blob/master/docs/serving.md)
give the full option contract.
