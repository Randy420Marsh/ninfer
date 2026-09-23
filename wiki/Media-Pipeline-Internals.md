# Media pipeline internals (ninfer-serve)

How a `video_url` / `input_audio` part becomes Vision input or transcript text, why it is built this way,
and where the code is. For usage see [Video and audio](Video-and-Audio).

## Request flow

```
POST /v1/chat/completions
  openai_chat_request.cpp   parse part -> ContentPart{kind Video|Audio, source, media (MediaRequest)}
  GenerationService::prepare / count_prompt_tokens (generation_service.cpp)
    expand_media_parts()     runs before prompt translation
      Policy: deadline, cancellation, --media-path root, private-URL switch, byte limit
      Video part -> media_pipeline::prepare_video()      (src/product/media_pipeline/pipeline.cpp)
      Audio part -> media_pipeline::prepare_audio()
        Input        URL: resolve_remote_url (redirects checked) -> libav demuxer with range reads
                     Path: resolve_media_path under the root;  Bytes/Data: custom in-memory AVIO
        make_plan    segments, frame size from detail, fps lowered to fit max_tokens / max_frames
        per segment:
          extract_clip   decode (dense, or seek per sample at <= 0.25 fps), scale, dedup per pair,
                         encode FFV1 into Matroska in memory; split at the 16,384-token item limit
          extract_audio  resample to 16 kHz mono s16 -> WAV bytes -> transcribe (curl multipart)
        -> Pieces: header text, clips, transcript text
      each clip becomes a ContentPart{Video, Source{Bytes, "video/x-matroska"}}
  to_prompt_input -> acquire_media (clip bytes) -> Engine
    Qwen processor (src/models/qwen3_5/frontend/processor.cpp)
      decode.cpp: frame count from packets, per-frame container times
      video_timestamps(): <t seconds> per frame pair from the real frame times
```

## Design choices

| Choice | Reason |
|---|---|
| Expand media before prompt translation | the Engine and processor stay unchanged: a prepared clip is just another in-memory video, with the existing caching, budgets and cancellation |
| In-process libav (no ffmpeg binary) | ninfer already links FFmpeg libraries. Seeking, cancellation and deadlines go through the demuxer's interrupt callback |
| FFV1 in Matroska, pts in ms = source seconds | lossless and intra-only, so on-screen text is not blurred twice. The source timestamps reach the processor |
| Split segments at 16,384 tokens | `kMaximumVisionItemTokens` in the Qwen frontend. A 60 s 1080p segment at `standard` is ~24k tokens and failed as one item |
| Frame count from demuxed packets | Matroska stores no frame count, and duration × fps overcounts clips with dropped pairs. Indices past the last real frame could not be decoded |
| `EngineOptions.video_max_pixels / video_max_seconds / video_fps` | the processor's default video pixel budget, 10 min duration cap and 2 fps sampling would re-thin or reject planned clips. The server sets them from its own budget |
| Budget clamp | request `max_tokens` / `max_frames` can only lower the server limits |
| `resolve_remote_url` + `max_redirects=0` | libav's HTTP client follows redirects on its own, which would skip the public-address check. Every hop is checked with a one-byte curl probe first |
| `protocol_whitelist` + `format_whitelist` | HLS/concat/image2 demuxers open further URLs or files named inside the media |
| Frame area = `detail × 1024` px | Qwen3.x: 32×32 px per merged token over two frames. The presets match the llama.cpp fork's readability calibration |
| Dedup on a 64×36 grid, max cell change | a whole-frame mean missed a changing clock digit. One pair is still kept every 10 s |

## Changed files

| File | Change |
|---|---|
| `src/product/media_pipeline/pipeline.h/.cpp` | new: `prepare_video`, `prepare_audio`, `Settings`, `MediaRequest`, `parse_time`, `detail_level_tokens` |
| `src/product/media_acquire/acquire.h/.cpp` | `resolve_remote_url` (redirect-checked), `resolve_media_path`, shared `init_curl` |
| `src/product/CMakeLists.txt`, `src/serve/CMakeLists.txt`, `cmake/Dependencies.cmake` | `ninfer_media_pipeline` library, libswresample |
| `src/serve/request.h` | `ContentKind::Audio`, `ContentPart.media` |
| `src/serve/openai_chat_request.cpp` | `input_video` / `input_audio` / `audio_url`, `file://`, bare base64, per-part options |
| `src/serve/generation_service.cpp` | `expand_media_parts`, engine video overrides, private-URL policy |
| `src/serve/serve_options.h/.cpp` | `--video-*`, `--media-path`, `--media-allow-private-urls`, `--asr-*` |
| `include/ninfer/types.h`, `src/models/qwen3_5/frontend/frontend.h/.cpp`, `src/runtime/engine/model_instance.cpp` | `video_max_pixels`, `video_max_seconds`, `video_fps` overrides |
| `src/media/decode/decode.h/.cpp` | per-frame container times, packet-count frame totals |
| `src/models/qwen3_5/frontend/processor.cpp` | timestamps from frame times |
| `tests/test_openai_schema.cpp` | `input_video` / `input_audio` / `file://` / media option tests |
| `docs/serving.md` | endpoint bullets, "Video and audio" section, flag table |
| `BUILD_WINDOWS.md`, `vcpkg.json`, `README.md`, `docs/README.md` | Windows build guide (moved from `docs/windows-build.md`), vcpkg manifest |

Unit tests: `ninfer_openai_schema_test`, `ninfer_media_decode_test`, `ninfer_serve_options_test`,
`ninfer_openai_responses_test`, `ninfer_anthropic_schema_test` and `ninfer_prompt_input_test` all pass.
On Windows, build them with `--target`; some other tests are POSIX-only.

## Test tools

The test tools live outside the repo, in `C:\AI\video-pipeline-tests`:

| Script | Purpose |
|---|---|
| `server_matrix.py URL --modalities vision,video,audio [--label L] [--json F]` | every modality × every reasoning effort from `/v1/models`, streamed; checks answers, reasoning placement, tag leaks, finish reason |
| `pipeline_test.py URL LABEL [case ...]` | clip, 1080p, `file://` seek, HTTP seek, audio, over-budget error |
| `range_server.py DIR [PORT]` | static server with HTTP Range support that logs the bytes sent |
| `make_test_videos.sh` | builds the test media with known contents |
