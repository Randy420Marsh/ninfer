# Video and audio in ninfer-serve

`ninfer-serve` in this fork takes video and audio in OpenAI Chat Completions requests: from a URL, a
local file or inline base64. Before the Vision encoder sees a video, the server cuts it to the requested
time ranges, samples frames and scales them. Long recordings can then be queried by time without filling
the context. Speech becomes a timestamped transcript.

- [1. Starting the server](#1-starting-the-server)
- [2. Endpoints](#2-endpoints)
- [3. Using it from the OpenAI endpoint](#3-using-it-from-the-openai-endpoint)
- [4. How to ask](#4-how-to-ask)
- [5. Budget, detail and readability](#5-budget-detail-and-readability)
- [6. Timestamps and dedup](#6-timestamps-and-dedup)
- [7. Audio transcripts](#7-audio-transcripts)
- [8. Remote URLs and local files](#8-remote-urls-and-local-files)
- [9. Tested and verified](#9-tested-and-verified)
- [10. Errors and limits](#10-errors-and-limits)

## 1. Starting the server

Video needs `--vision`. Transcripts need `--asr-url`; audio-only requests work without `--vision`.
Local files need `--media-path`:

```powershell
ninfer-serve E:\models\qwen3_8_27b_huihui_abliterated_nvfp4_v3b.ninfer `
  --host 127.0.0.1 --port 8081 --max-context 160000 --vision `
  --media-path C:\media --asr-url http://127.0.0.1:8178
```

### Flags

| Flag | Meaning | Default |
|---|---|---|
| `--vision` | enable image/video input (loads the Vision allocations) | off |
| `--video-fps F` | sampling rate; also the most a request may ask for | `2` |
| `--video-min-fps F` | lowest rate a long range may be thinned to; longer ranges are rejected | `0.05` |
| `--video-detail low\|standard\|high\|max` | default frame detail | `standard` |
| `--video-max-tokens N` | Vision-token budget per video part, `64..32768` (requests may only lower it) | `24576` |
| `--video-max-frames N` | frame budget per video part, `4..768` (requests may only lower it) | `768` |
| `--video-dedup F` | a frame pair is kept only if some screen region changed by at least F luma levels (0–255); `0` = off | `4` |
| `--media-path DIR` | allow `file://` media under DIR | disabled |
| `--media-allow-private-urls` | let media URLs reach loopback / LAN addresses | public only |
| `--asr-url URL` | OpenAI-compatible `/v1/audio/transcriptions` server | unset |
| `--asr-model NAME` | `model` field sent to it | server default |
| `--asr-language LANG` | default transcript language (`en`, `fi`, `zh`, ...) | auto-detect |

The default budget is 24,576 rather than llama.cpp's 32,768. ninfer allows at most
`min(--max-context, 32768)` Vision tokens per **prompt**, and the lower default leaves room for an image
or a second part.

## 2. Endpoints

| Endpoint | Video | Audio |
|---|---|---|
| `POST /v1/chat/completions` | `video_url`, `input_video` with all options below | `input_audio`, `audio_url` → transcript |
| `POST /v1/responses` | `input_video` with an HTTP(S) or data-URI `video_url`; server defaults (whole video, no per-part options) | rejected (`audio_inputs_not_supported`) |
| `POST /v1/messages` (Anthropic) | not available (images only) | not available |

ninfer has no web UI. Use any OpenAI-compatible client, or the API directly.

## 3. Using it from the OpenAI endpoint

### Part types (Chat Completions, user messages)

| Part | Value | Notes |
|---|---|---|
| `{"type": "video_url", "video_url": {"url": ...}}` | HTTP(S) URL, `file://path`, data URI | also a plain string `"video_url": "https://..."` |
| `{"type": "input_video", "input_video": {"data": ...}}` | HTTP(S) URL, `file://path`, data URI, bare base64 | llama.cpp / web UI form; `url` works in place of `data` |
| `{"type": "input_audio", "input_audio": {"data": ..., "format": "wav"}}` | same as `input_video` | OpenAI form; the container is probed, `format` is not needed |
| `{"type": "audio_url", "audio_url": {"url": ...}}` | HTTP(S) URL, `file://path`, data URI | |

Formats: anything FFmpeg reads in a plain container (mp4/mov, mkv/webm, avi, flv, ts, ogg, wav, mp3,
flac, aac, ...). Playlists (m3u8), concat lists and image sequences are refused.

### Options (inside the part object)

| Field | Values | Default |
|---|---|---|
| `start`, `end` | seconds (`3600`, `"3600s"`) or `"[hh:]mm:ss[.f]"` (`"01:00:00"`) | whole file |
| `duration` | instead of `end`; same formats | |
| `segments` | 1–256 ranges, each `{start, end}` or `{start, duration}` | |
| `fps` | sampling rate, at most `--video-fps` | `--video-fps` |
| `detail` | `low` (256), `standard` (576), `high` (1024), `max` (2048), or an integer token count per frame pair | `--video-detail` |
| `max_tokens` | budget for this part; can only lower `--video-max-tokens` | server limit |
| `max_frames` | frame budget; can only lower `--video-max-frames` | server limit |
| `audio` | `auto`, `transcript`, `none`, or `true` / `false` | `auto` |
| `language` | transcript language hint, e.g. `"fi"` | `--asr-language` |

### Examples

Two ranges of a 12-hour recording, high detail, with the spoken audio:

```bash
curl http://127.0.0.1:8081/v1/chat/completions -H "Content-Type: application/json" -d '{
  "model": "qwen3.8-27b",
  "messages": [{"role": "user", "content": [
    {"type": "video_url", "video_url": {
      "url": "file://recordings/day.mp4",
      "segments": [{"start": "05:00:00", "end": "05:00:12"}, {"start": "10:30:00", "duration": 12}],
      "detail": "high", "audio": "transcript"}},
    {"type": "text", "text": "For each segment: what does the screen show and what is said, with times?"}
  ]}],
  "max_tokens": 2000
}'
```

A remote video, one range, reading small text:

```json
{"type": "video_url", "video_url": {"url": "https://example.com/lecture.mp4",
  "start": "00:42:00", "end": "00:43:30", "fps": 0.5, "detail": "max"}}
```

An audio file (becomes a transcript):

```json
{"type": "input_audio", "input_audio": {"data": "<base64 wav/mp3/flac>", "format": "wav"}}
```

Python (`openai` package):

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8081/v1", api_key="none")
r = client.chat.completions.create(
    model="qwen3.8-27b",
    messages=[{"role": "user", "content": [
        {"type": "video_url", "video_url": {"url": "file://talk.mp4", "start": "01:00:00", "end": "01:05:00"}},
        {"type": "text", "text": "Summarize this part. Quote what is said about the budget, with times."},
    ]}],
    reasoning_effort="low",            # none, minimal, low, medium, high, xhigh, max
)
print(r.choices[0].message.content)
```

Reasoning works with media in every mode. Use `reasoning_effort` (`none` turns it off) or
`chat_template_kwargs.enable_thinking`. With reasoning on, leave room in `max_tokens` for the thinking.
The model id is the one `GET /v1/models` reports.

## 4. How to ask

The model only sees the ranges you send, and the server does not read your question to choose them. So
put the time range in the request fields, and ask the question in text.

- **Media first, question after.** Put the media parts before the text part in `content`.
- **Ask for times.** Frames and transcript lines carry source time. Questions like "when does X happen",
  "quote what is said with times" or "which clock time is shown" get answers in source seconds
  (for example 18000.8 s for 05:00:00.8).
- **Reading on-screen text:** use `detail: "high"` or `"max"` and a low `fps` (0.2–0.5).
- **Following motion:** use `standard` or `low` detail at 1–2 fps.
- **Speech:** set `audio: "transcript"` (the default for video when `--asr-url` is set) and `language` for
  non-English audio.
- **Several parts in one request** share the 32,768-token prompt limit. Lower each part's `max_tokens`
  so the total fits.
- **Long videos (hours):** split the video into windows, ask each window for a timestamped summary, then
  send one text-only request that combines them (map-reduce):

```python
windows = [(t, min(t + 1200, total)) for t in range(0, total, 1200)]   # 20 min each
notes = []
for a, b in windows:
    r = ask([{"type": "video_url", "video_url": {"url": URL, "start": a, "end": b,
                                                 "fps": 0.2, "detail": "standard"}},
             {"type": "text", "text": "List the events in this part with their times (seconds)."}])
    notes.append(f"[{a}-{b} s]\n{r}")
final = ask([{"type": "text", "text": "Combine these notes into one timeline and summary:\n\n" + "\n\n".join(notes)}])
```

## 5. Budget, detail and readability

Frames keep their aspect ratio. Each frame is scaled so its area is `detail × 1024` pixels (multiples of
32, never upscaled). On Qwen3.x one merged token covers 32×32 px over two frames, so a frame costs
`W × H / 2048` tokens. The detail presets were calibrated with the same model on the llama.cpp fork:

| detail | frame size (16:9) | tokens / frame | Chinese 20 px body text | fine UI text (26 fields) |
|---|---|---|---|---|
| `low` (256) | 672×384 | ~126 | 93 % of characters | 6/26 |
| `standard` (576) | 1024×576 | ~288 | 100 % | 16/26 |
| `high` (1024) | 1344×768 | ~504 | 100 % | 21/26 |
| `max` (2048) | 1920×1088 | ~1020 | 100 % | 25/26 |

The frame rate is the smallest of: the requested `fps`, `max_frames / length` and
`max_tokens / (tokens per frame × length)`. If that falls below `--video-min-fps` the request is
rejected, and the error says which range length would fit.

Seconds that fit in the default 24,576 tokens:

| detail | at 2 fps | at 0.5 fps | at 0.05 fps (floor) |
|---|---|---|---|
| `low` | 97 s | 6.5 min | 65 min |
| `standard` | 43 s | 2.8 min | 28 min |
| `max` | 12 s | 48 s | 8 min |

A segment larger than 16,384 tokens (the Qwen Vision limit for one item) is sent as consecutive clips of
at most that size. Timestamps stay continuous across them. Each request also has to fit
`--max-context`: Vision tokens + transcript + text + output.

## 6. Timestamps and dedup

- **Source time.** Clip frames keep their source timestamps. The Qwen processor reads them from the
  container, so frames 5 h into a file are labelled 18000 s and later. Each frame pair gets the model's
  own `<t seconds>` marker, the mean time of its two frames.
- **Ranges** get a header: `Video segment 2 of 3, source time 10:30:00.0 to 10:30:12.0:`.
- **Dedup.** A frame pair is dropped when no region of a 64×36 grid changed by at least `--video-dedup`
  luma levels since the last kept pair. A single changing clock digit still counts. At least one pair is
  kept every 10 s even when nothing changes. A still video collapses to one frame pair.
- **Sparse sampling.** At 0.25 fps or lower, the server seeks to each sample instead of decoding every
  frame in between, which keeps hour-long ranges fast.

## 7. Audio transcripts

The served models have no audio encoder, so audio always reaches the model as text:

| `audio` | Video part | Audio part |
|---|---|---|
| `auto` (default) | transcript when `--asr-url` is set, else frames only | transcript (needs `--asr-url`) |
| `transcript` | transcript; error without `--asr-url` | same |
| `none` / `false` | frames only | part is dropped |

```
Audio transcript of this video segment (speech-to-text; times are seconds in the source):
<18000.8 - 18002.7 seconds> Testing, testing, one, two, three.
<18003.6 - 18006.0 seconds> The quick brown fox jumps over the lazy dog.
```

### Speech-to-text server

Any server that implements OpenAI `POST /v1/audio/transcriptions` and returns `verbose_json` segments
works. The one used for testing (`whisper-asr-server`, faster-whisper) keeps everything in memory:

```powershell
python asr_server.py --port 8178 --model large-v3 --compute-type float16 --preload
# --idle-unload 300 frees the VRAM after 5 idle minutes
```

- Use multilingual `large-v3`. The `distil-*` models are English-only, and `turbo` was less accurate here.
- `float16` is more accurate than `int8_float16`: the int8 run heard "quick brown" as "quick-prone".
- Voice activity detection is automatic. When it removes most of the audio (music, singing), the audio is
  transcribed again without it. Stock hallucinations ("Thanks for watching") are filtered.

## 8. Remote URLs and local files

- **HTTP(S)** is read by the demuxer with range requests: only the index and the requested ranges are
  downloaded (16 MiB for a 10 s range of a 199 MiB, 12 h file).
- **Public addresses only** by default. The URL and every redirect hop must resolve to a public address,
  checked with a one-byte probe. The demuxer then opens the final URL with redirects disabled.
  `--media-allow-private-urls` allows loopback and LAN hosts (a NAS, a local media server). The check
  does not pin DNS, so a hostname whose answer changes between the check and the open is not caught. Use
  the flag only on trusted networks.
- **`file://`** needs `--media-path DIR`. Relative paths (`file://talks/day1.mp4`) resolve under DIR.
  Absolute paths (`file:///C:/media/x.mp4`) must lie inside it.
- **Plain containers only.** Playlist, concat and image-sequence inputs are refused, because they could
  open other files or hosts named inside the media.

## 9. Tested and verified

Model: **Qwen3.8-27B huihui abliterated NVFP4** (`qwen3_8_27b_huihui_abliterated_nvfp4_v3b.ninfer`),
`--max-context 160000 --vision`, RTX 5090, 2026-09-23. The Qwen3.x artifacts are the architecture
`ninfer-serve` runs. Other model families are not served.

**Capability matrix: 32/32.** Text, image, video and audio each ran under 8 reasoning modes: off,
default, minimal, low, medium, high, xhigh and max. Requests were streamed. A run passes when the answer
has the known facts of the test media, reasoning appears only in `reasoning_content` (and only when
on), no think tags leak into the answer, and it finishes normally.

**Pipeline cases: 6/6.**

| Case | Result |
|---|---|
| 8 s clip with speech, data URI | direction of motion and both spoken sentences with times; 2.6k tokens |
| 1080p 60 s, data URI | described, all four spoken sentences with times; 25.3k tokens (was 247k) |
| 12 h video via `file://`, two 12 s ranges at 05:00:00 and 10:30:00 | correct clock readings, SECTION numbers and speech at 18000.8 s / 37800.8 s |
| 12 h video over HTTP (with `--media-allow-private-urls`), 10 s range at `detail: max` | correct start/end clock; 16 MiB of 199 MiB downloaded |
| `input_audio` wav | exact transcript with times |
| whole 12 h video in one part | clear 400 error: "43200 s of video does not fit ... at most 1707 s" |

The same requests pass on the llama.cpp fork, so clients can switch between the two servers.

## 10. Errors and limits

| Error | Meaning |
|---|---|
| 400 `invalid_media`: `N s of video does not fit ... allows at most M s` | the ranges need more than the budget even at `--video-min-fps`: send shorter ranges, lower `detail`, or split into chunks |
| 400 `invalid_media`: `file:// media needs ninfer-serve --media-path` | start with `--media-path` |
| 400 `invalid_media`: `media path is outside configured media root` | the path escapes `--media-path` |
| 400 `invalid_media`: `media URL resolves only to disallowed network addresses` | private/loopback URL without `--media-allow-private-urls` |
| 400 `invalid_media`: `transcripts need a speech-to-text server; start ninfer-serve with --asr-url` | `audio: "transcript"` or an audio part without `--asr-url` |
| 400 `vision_disabled` | video without `--vision` |
| 400 `media_budget_exceeded` | a prepared item or the whole prompt exceeds the Vision limits (32,768 tokens per prompt) |
| 502 `media_processing_failed` | the speech-to-text server failed or is unreachable |
| 502 / 504 `media_fetch_failed` / `media_fetch_timeout` | the remote URL failed or timed out |

Limits:

- Per-part options exist on Chat Completions only. Responses `input_video` uses server defaults, and
  Responses rejects `input_audio`.
- No native audio. The served models have no audio encoder.
- Requests are prepared in memory. Base64 uploads count against `--max-request-mib` (default 384 MiB)
  and are about 1.33× the file size.
