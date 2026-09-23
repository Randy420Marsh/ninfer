#pragma once

#include "product/media_acquire/acquire.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Video and audio preparation for serving, done entirely in memory.
//
// A video part is cut into its requested segments: each segment is seeked in the source (HTTP
// range requests for URLs, file seeks, or an in-memory buffer), sampled at the planned frame rate,
// scaled with preserved aspect to the requested detail, stripped of frame groups that repeat the
// previous one, and written as a lossless FFV1/Matroska clip whose timestamps are source-media
// seconds. The processor then sees an ordinary short video whose frame timestamps are absolute.
// The segment's audio track is decoded to 16 kHz mono and transcribed by an OpenAI-compatible
// speech-to-text server, because the served models have no audio encoder.
namespace ninfer::product::media_pipeline {

struct Segment {
    double start = 0.0;
    double end   = -1.0; // negative: to the end of the media
};

// Per-part options from the wire; unset values take the server Settings.
struct MediaRequest {
    std::vector<Segment> segments; // empty: the whole media
    std::optional<double> fps;
    std::optional<int> detail_tokens;
    std::string detail_name;
    std::optional<int> max_tokens;
    std::optional<int> max_frames;
    std::string audio = "auto"; // auto | transcript | none
    std::string language;
};

struct Settings {
    double fps              = 2.0; // default and maximum sampling rate
    double min_fps          = 0.05;
    int detail_tokens       = 576;
    std::string detail_name = "standard";
    int max_tokens          = 24'576; // per video part, merged Vision tokens
    int max_frames          = 768;
    // largest single Vision item the model accepts; longer segments become consecutive clips
    int item_max_tokens     = 16'384;
    double dedup            = 4.0; // luma change (0-255) a region must show to keep a group; 0 = off
    std::string asr_url;
    std::string asr_model;
    std::string asr_language;
};

// One replacement content part: text, or a prepared video clip.
struct Piece {
    bool is_video = false;
    std::string text;
    std::vector<std::uint8_t> clip; // FFV1 in Matroska; frame timestamps are source seconds
};

// Merged Vision tokens per two-frame group for a named detail level; throws on unknown names.
// Calibrated on the served Qwen models: 20 px text on a 1080p frame stays readable from
// "standard" (1024x576) up; small UI text needs "max" (1920x1088).
int detail_level_tokens(const std::string& name);

// Seconds from "hh:mm:ss(.f)", "mm:ss(.f)" or "ss(.f)" with an optional trailing "s".
double parse_time(const std::string& text);

std::vector<Piece> prepare_video(const media_acquire::Source& source, const MediaRequest& request,
                                 const Settings& settings, const media_acquire::Policy& policy);

// Audio parts become a transcript; the served models have no audio input.
std::vector<Piece> prepare_audio(const media_acquire::Source& source, const MediaRequest& request,
                                 const Settings& settings, const media_acquire::Policy& policy);

} // namespace ninfer::product::media_pipeline
