#include "product/media_pipeline/pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ninfer::product::media_pipeline {
namespace {

using Clock = std::chrono::steady_clock;

std::string av_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

std::string hms(double t) {
    t              = std::max(0.0, t);
    const int h    = static_cast<int>(t / 3600.0);
    const int m    = static_cast<int>((t - h * 3600.0) / 60.0);
    const double s = t - h * 3600.0 - m * 60.0;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%04.1f", h, m, s);
    return buffer;
}

std::string fixed(double v, int precision = 1) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return buffer;
}

// ------------------------------------------------------------------ input

struct MemoryCursor {
    const std::uint8_t* data = nullptr;
    std::size_t size         = 0;
    std::size_t offset       = 0;
};

int memory_read(void* opaque, std::uint8_t* output, int size) {
    auto& cursor = *static_cast<MemoryCursor*>(opaque);
    if (cursor.offset >= cursor.size) { return AVERROR_EOF; }
    const std::size_t amount =
        std::min<std::size_t>(static_cast<std::size_t>(size), cursor.size - cursor.offset);
    std::memcpy(output, cursor.data + cursor.offset, amount);
    cursor.offset += amount;
    return static_cast<int>(amount);
}

std::int64_t memory_seek(void* opaque, std::int64_t offset, int whence) {
    auto& cursor = *static_cast<MemoryCursor*>(opaque);
    if (whence == AVSEEK_SIZE) { return static_cast<std::int64_t>(cursor.size); }
    std::int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = static_cast<std::int64_t>(cursor.offset);
        break;
    case SEEK_END:
        base = static_cast<std::int64_t>(cursor.size);
        break;
    default:
        return AVERROR(EINVAL);
    }
    const std::int64_t target = base + offset;
    if (target < 0 || static_cast<std::uint64_t>(target) > cursor.size) { return AVERROR(EINVAL); }
    cursor.offset = static_cast<std::size_t>(target);
    return target;
}

// One opened media source. URL and path sources are read (and seeked) by the demuxer itself, so a
// segment of a long remote video costs only the byte ranges around it.
class Input {
public:
    Input(const media_acquire::Source& source, const media_acquire::Policy& policy)
        : policy_(policy) {
        format_ = avformat_alloc_context();
        if (format_ == nullptr) { throw std::bad_alloc(); }
        format_->interrupt_callback = AVIOInterruptCB{&Input::interrupted, this};
        AVDictionary* options       = nullptr;
        std::string url;
        // Plain containers only: playlist/concat/image-sequence demuxers open further URLs or files
        // named inside the media, which would bypass the URL and media-root checks.
        av_dict_set(&options, "format_whitelist",
                    "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,flv,mpegts,mpeg,asf,ogg,m4v,h264,hevc,"
                    "ivf,obu,wav,w64,mp3,aac,flac,aiff,caf,amr,wv",
                    0);
        try {
            if (source.kind == media_acquire::SourceKind::Url) {
                // redirects are resolved (and checked) up front; the demuxer may not follow more
                url = media_acquire::resolve_remote_url(source.value, policy);
                av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls", 0);
                av_dict_set(&options, "max_redirects", "0", 0);
                av_dict_set(&options, "rw_timeout", "30000000", 0);
                av_dict_set(&options, "reconnect", "1", 0);
            } else if (source.kind == media_acquire::SourceKind::Path) {
                const auto path = media_acquire::resolve_media_path(source, policy).u8string();
                url.assign(reinterpret_cast<const char*>(path.data()), path.size());
                av_dict_set(&options, "protocol_whitelist", "file", 0);
            } else {
                av_dict_set(&options, "protocol_whitelist", "none", 0);
                bytes_  = media_acquire::acquire_bytes(source, policy);
                cursor_ = MemoryCursor{bytes_.data(), bytes_.size(), 0};
                auto* buffer = static_cast<std::uint8_t*>(av_malloc(1 << 16));
                if (buffer == nullptr) { throw std::bad_alloc(); }
                io_ = avio_alloc_context(buffer, 1 << 16, 0, &cursor_, memory_read, nullptr,
                                         memory_seek);
                if (io_ == nullptr) {
                    av_free(buffer);
                    throw std::bad_alloc();
                }
                format_->pb = io_;
                format_->flags |= AVFMT_FLAG_CUSTOM_IO;
            }
            AVFormatContext* raw = format_;
            int rc = avformat_open_input(&raw, url.empty() ? nullptr : url.c_str(), nullptr, &options);
            format_ = raw;
            av_dict_free(&options);
            if (rc < 0) { throw std::invalid_argument("cannot open media: " + av_error(rc)); }
            if ((rc = avformat_find_stream_info(format_, nullptr)) < 0) {
                throw std::invalid_argument("cannot read media streams: " + av_error(rc));
            }
            video_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            audio_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        } catch (...) {
            av_dict_free(&options);
            close();
            throw;
        }
    }

    ~Input() { close(); }
    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    [[nodiscard]] AVFormatContext* format() const noexcept { return format_; }
    [[nodiscard]] AVStream* video() const noexcept {
        return video_index_ >= 0 ? format_->streams[video_index_] : nullptr;
    }
    [[nodiscard]] AVStream* audio() const noexcept {
        return audio_index_ >= 0 ? format_->streams[audio_index_] : nullptr;
    }

    [[nodiscard]] double duration() const noexcept {
        if (format_->duration > 0) { return static_cast<double>(format_->duration) / AV_TIME_BASE; }
        const AVStream* s = video() != nullptr ? video() : audio();
        if (s != nullptr && s->duration > 0) {
            return static_cast<double>(s->duration) * av_q2d(s->time_base);
        }
        return 0.0;
    }

    // Stream time of media position 0, so segment times count from the start of the media.
    [[nodiscard]] static double origin(const AVStream* s) noexcept {
        return s->start_time != AV_NOPTS_VALUE ? static_cast<double>(s->start_time) * av_q2d(s->time_base)
                                               : 0.0;
    }

    // Keyframe seek on one stream to just before a media position; the caller drops earlier frames.
    void seek(const AVStream* s, double seconds, AVCodecContext* decoder) {
        if (seconds <= 0.0) {
            if (av_seek_frame(format_, s->index, s->start_time != AV_NOPTS_VALUE ? s->start_time : 0,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                (void)avformat_seek_file(format_, -1, INT64_MIN, 0, 0, 0);
            }
        } else {
            const std::int64_t ts =
                static_cast<std::int64_t>(std::llround((origin(s) + seconds) / av_q2d(s->time_base)));
            const int rc = av_seek_frame(format_, s->index, ts, AVSEEK_FLAG_BACKWARD);
            if (rc < 0) { throw std::invalid_argument("cannot seek in media: " + av_error(rc)); }
        }
        avcodec_flush_buffers(decoder);
    }

private:
    static int interrupted(void* opaque) {
        const auto& self = *static_cast<Input*>(opaque);
        if (self.policy_.is_cancelled && self.policy_.is_cancelled()) { return 1; }
        return self.policy_.deadline != Clock::time_point{} && Clock::now() >= self.policy_.deadline ? 1 : 0;
    }

    void close() noexcept {
        if (format_ != nullptr) {
            if ((format_->flags & AVFMT_FLAG_CUSTOM_IO) != 0) { format_->pb = nullptr; }
            avformat_close_input(&format_);
            avformat_free_context(format_);
            format_ = nullptr;
        }
        if (io_ != nullptr) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
    }

    const media_acquire::Policy& policy_;
    AVFormatContext* format_ = nullptr;
    AVIOContext* io_         = nullptr;
    std::vector<std::uint8_t> bytes_;
    MemoryCursor cursor_;
    int video_index_ = -1;
    int audio_index_ = -1;
};

struct CodecDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
using CodecPtr  = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr  = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

CodecPtr open_decoder(const AVStream* s) {
    const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (codec == nullptr) { throw std::invalid_argument("media codec is not supported"); }
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) { throw std::bad_alloc(); }
    int rc = avcodec_parameters_to_context(ctx.get(), s->codecpar);
    ctx->thread_count = 0;
    if (rc < 0 || (rc = avcodec_open2(ctx.get(), codec, nullptr)) < 0) {
        throw std::invalid_argument("cannot open media decoder: " + av_error(rc));
    }
    return ctx;
}

// Decodes one stream from the current position; callback(frame, seconds) returns false to stop.
template <typename Callback>
void decode_stream(Input& input, const AVStream* s, AVCodecContext* decoder, Callback&& callback) {
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) { throw std::bad_alloc(); }
    const double tb     = av_q2d(s->time_base);
    const double origin = Input::origin(s);
    auto drain          = [&]() {
        while (true) {
            const int rc = avcodec_receive_frame(decoder, frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) { return true; }
            if (rc < 0) { throw std::invalid_argument("cannot decode media: " + av_error(rc)); }
            const std::int64_t pts =
                frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const double seconds = pts != AV_NOPTS_VALUE ? static_cast<double>(pts) * tb - origin : -1.0;
            const bool more      = callback(frame.get(), seconds);
            av_frame_unref(frame.get());
            if (!more) { return false; }
        }
    };
    while (true) {
        const int rc = av_read_frame(input.format(), packet.get());
        if (rc == AVERROR_EOF) { break; }
        if (rc == AVERROR_EXIT) { throw media_acquire::Error(media_acquire::ErrorKind::Cancelled, "media read interrupted"); }
        if (rc < 0) { throw std::invalid_argument("cannot read media: " + av_error(rc)); }
        if (packet->stream_index == s->index) {
            const int send = avcodec_send_packet(decoder, packet.get());
            av_packet_unref(packet.get());
            if (send < 0 && send != AVERROR(EAGAIN) && send != AVERROR_INVALIDDATA) {
                throw std::invalid_argument("cannot decode media: " + av_error(send));
            }
            if (!drain()) { return; }
        } else {
            av_packet_unref(packet.get());
        }
    }
    (void)avcodec_send_packet(decoder, nullptr);
    (void)drain();
}

// ------------------------------------------------------------------ planning

struct Plan {
    std::vector<Segment> segments;
    bool explicit_range = false;
    int width           = 0;
    int height          = 0;
    double fps          = 0.0;
    int display_rotation = 0; // degrees, from the source display matrix
    std::vector<std::uint8_t> display_matrix;
};

int rotation_degrees(const AVStream* s, std::vector<std::uint8_t>& matrix) {
    const AVPacketSideData* side = av_packet_side_data_get(s->codecpar->coded_side_data,
                                                           s->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    if (side == nullptr || side->size < 36) { return 0; }
    matrix.assign(side->data, side->data + 36);
    const double angle = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side->data));
    return std::isnan(angle) ? 0 : static_cast<int>(std::lround(std::fabs(angle))) % 360;
}

std::vector<Segment> resolve_segments(const MediaRequest& request, double duration) {
    std::vector<Segment> out = request.segments;
    if (out.empty()) {
        if (!(duration > 0.0)) {
            throw std::invalid_argument("cannot determine the media duration; give an explicit start/end");
        }
        return {Segment{0.0, duration}};
    }
    for (Segment& s : out) {
        if (s.end < 0.0) {
            if (!(duration > 0.0)) {
                throw std::invalid_argument("segment without end, and the media duration is unknown");
            }
            s.end = duration;
        }
        if (duration > 0.0) {
            if (s.start >= duration) {
                throw std::invalid_argument("segment start " + hms(s.start) +
                                            " is beyond the end of the media (" + hms(duration) + ")");
            }
            s.end = std::min(s.end, duration);
        }
        if (!(s.end > s.start)) {
            throw std::invalid_argument("segment end must be after its start (" + hms(s.start) + ")");
        }
    }
    return out;
}

Plan make_plan(const Input& input, const MediaRequest& request, const Settings& settings) {
    const AVStream* v = input.video();
    if (v == nullptr || v->codecpar->width <= 0 || v->codecpar->height <= 0) {
        throw std::invalid_argument("the media has no video stream");
    }
    Plan plan;
    plan.explicit_range   = !request.segments.empty();
    plan.segments         = resolve_segments(request, input.duration());
    plan.display_rotation = rotation_degrees(v, plan.display_matrix);
    int src_w = v->codecpar->width;
    int src_h = v->codecpar->height;
    if (plan.display_rotation == 90 || plan.display_rotation == 270) { std::swap(src_w, src_h); }

    // aspect preserved, area from the detail level (32x32 px per merged token), never upscaled
    const int tokens        = request.detail_tokens.value_or(settings.detail_tokens);
    const std::string level = request.detail_tokens ? request.detail_name : settings.detail_name;
    const double scale =
        std::min(1.0, std::sqrt(static_cast<double>(tokens) * 1024.0 / (static_cast<double>(src_w) * src_h)));
    plan.width  = std::max(32, static_cast<int>(std::lround(src_w * scale / 32.0)) * 32);
    plan.height = std::max(32, static_cast<int>(std::lround(src_h * scale / 32.0)) * 32);
    // the displayed size is planned; frames are scaled before rotation, so swap back for the coded frame
    if (plan.display_rotation == 90 || plan.display_rotation == 270) { std::swap(plan.width, plan.height); }

    const double frame_tokens = static_cast<double>(plan.width) * plan.height / 2048.0; // two frames per group
    double total              = 0.0;
    for (const Segment& s : plan.segments) { total += s.end - s.start; }
    // a request may lower the server limits, never raise them
    const int max_frames = std::min(request.max_frames.value_or(settings.max_frames), settings.max_frames);
    const int max_tokens = std::min(request.max_tokens.value_or(settings.max_tokens), settings.max_tokens);
    double fps           = std::min(request.fps.value_or(settings.fps), settings.fps);
    fps                  = std::min({fps, max_frames / total, max_tokens / (frame_tokens * total)});
    if (fps < settings.min_fps) {
        const double fits = std::min(max_frames / settings.min_fps, max_tokens / (frame_tokens * settings.min_fps));
        throw std::invalid_argument(
            fixed(total, 0) + " s of video does not fit: at detail \"" + level + "\" (" +
            std::to_string(plan.width) + "x" + std::to_string(plan.height) + ", ~" + fixed(frame_tokens, 0) +
            " tokens per frame) with max_tokens=" + std::to_string(max_tokens) + " and max_frames=" +
            std::to_string(max_frames) + ", even " + fixed(settings.min_fps, 2) + " fps allows at most " +
            fixed(fits, 0) + " s. Use shorter segments (start/end or segments[]), a lower detail, a higher "
            "max_tokens, or split the video into chunks and summarize them one by one.");
    }
    plan.fps = fps;
    return plan;
}

// ------------------------------------------------------------------ video clip

// 64x36 grid of mean luma from the Y plane, to spot frame groups that repeat the last kept one.
std::vector<float> luma_grid(const AVFrame* f) {
    constexpr int kGx = 64;
    constexpr int kGy = 36;
    std::vector<float> sum(kGx * kGy, 0.0F);
    std::vector<int> count(kGx * kGy, 0);
    for (int y = 0; y < f->height; y += 2) {
        const int gy          = y * kGy / f->height;
        const std::uint8_t* row = f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0];
        for (int x = 0; x < f->width; x += 2) {
            const int cell = gy * kGx + x * kGx / f->width;
            sum[cell] += row[x];
            ++count[cell];
        }
    }
    for (std::size_t i = 0; i < sum.size(); ++i) { sum[i] = count[i] != 0 ? sum[i] / count[i] : 0.0F; }
    return sum;
}

class ClipWriter {
public:
    ClipWriter(int width, int height, double fps, const std::vector<std::uint8_t>& display_matrix) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        if (codec == nullptr) { throw std::runtime_error("FFV1 encoder is not available in this FFmpeg"); }
        int rc = avformat_alloc_output_context2(&out_, nullptr, "matroska", nullptr);
        if (rc < 0 || out_ == nullptr) { throw std::runtime_error("cannot create Matroska muxer: " + av_error(rc)); }
        encoder_.reset(avcodec_alloc_context3(codec));
        if (!encoder_) { throw std::bad_alloc(); }
        encoder_->width     = width;
        encoder_->height    = height;
        encoder_->pix_fmt   = AV_PIX_FMT_YUV420P;
        encoder_->time_base = AVRational{1, 1000};
        encoder_->framerate = av_d2q(fps, 100000);
        encoder_->gop_size  = 1;
        encoder_->level     = 3;
        encoder_->thread_count = 0;
        av_opt_set_int(encoder_->priv_data, "slices", 4, 0);
        if ((out_->oformat->flags & AVFMT_GLOBALHEADER) != 0) { encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; }
        if ((rc = avcodec_open2(encoder_.get(), codec, nullptr)) < 0) {
            throw std::runtime_error("cannot open FFV1 encoder: " + av_error(rc));
        }
        stream_ = avformat_new_stream(out_, nullptr);
        if (stream_ == nullptr) { throw std::bad_alloc(); }
        avcodec_parameters_from_context(stream_->codecpar, encoder_.get());
        stream_->time_base      = AVRational{1, 1000};
        stream_->avg_frame_rate = encoder_->framerate;
        if (!display_matrix.empty()) {
            (void)av_packet_side_data_add(&stream_->codecpar->coded_side_data,
                                          &stream_->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX,
                                          av_memdup(display_matrix.data(), display_matrix.size()),
                                          display_matrix.size(), 0);
        }
        if ((rc = avio_open_dyn_buf(&out_->pb)) < 0) { throw std::runtime_error("cannot open clip buffer"); }
        if ((rc = avformat_write_header(out_, nullptr)) < 0) {
            throw std::runtime_error("cannot write clip header: " + av_error(rc));
        }
        packet_.reset(av_packet_alloc());
        if (!packet_) { throw std::bad_alloc(); }
    }

    ~ClipWriter() {
        if (out_ != nullptr) {
            if (out_->pb != nullptr) {
                std::uint8_t* buffer = nullptr;
                (void)avio_close_dyn_buf(out_->pb, &buffer);
                av_free(buffer);
            }
            avformat_free_context(out_);
        }
    }

    ClipWriter(const ClipWriter&)            = delete;
    ClipWriter& operator=(const ClipWriter&) = delete;

    void write(AVFrame* frame, double seconds) {
        frame->pts = std::llround(seconds * 1000.0);
        send(frame);
        ++frames_;
    }

    [[nodiscard]] int frames() const noexcept { return frames_; }

    std::vector<std::uint8_t> finish() {
        send(nullptr);
        int rc = av_write_trailer(out_);
        if (rc < 0) { throw std::runtime_error("cannot finish clip: " + av_error(rc)); }
        std::uint8_t* buffer = nullptr;
        const int size       = avio_close_dyn_buf(out_->pb, &buffer);
        out_->pb             = nullptr;
        std::vector<std::uint8_t> bytes(buffer, buffer + std::max(size, 0));
        av_free(buffer);
        return bytes;
    }

private:
    void send(AVFrame* frame) {
        int rc = avcodec_send_frame(encoder_.get(), frame);
        if (rc < 0 && rc != AVERROR_EOF) { throw std::runtime_error("cannot encode clip frame: " + av_error(rc)); }
        while ((rc = avcodec_receive_packet(encoder_.get(), packet_.get())) >= 0) {
            av_packet_rescale_ts(packet_.get(), encoder_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            rc                    = av_interleaved_write_frame(out_, packet_.get());
            if (rc < 0) { throw std::runtime_error("cannot write clip frame: " + av_error(rc)); }
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
            throw std::runtime_error("cannot encode clip frame: " + av_error(rc));
        }
    }

    AVFormatContext* out_ = nullptr;
    AVStream* stream_     = nullptr;
    CodecPtr encoder_;
    PacketPtr packet_;
    int frames_ = 0;
};

struct ScalerDeleter {
    void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};

// One segment as one or more consecutive clips, each within the model's single-item Vision limit.
std::vector<std::vector<std::uint8_t>> extract_clip(Input& input, const Plan& plan, const Segment& segment,
                                                    const Settings& settings) {
    AVStream* v      = input.video();
    CodecPtr decoder = open_decoder(v);
    input.seek(v, segment.start, decoder.get());

    const double pair_tokens = static_cast<double>(plan.width) * plan.height / 1024.0;
    const int clip_frames    = std::max(2, static_cast<int>(settings.item_max_tokens / pair_tokens) * 2);
    std::vector<std::vector<std::uint8_t>> clips;
    auto writer = std::make_unique<ClipWriter>(plan.width, plan.height, plan.fps, plan.display_matrix);
    std::unique_ptr<SwsContext, ScalerDeleter> scaler;
    FramePtr scaled(av_frame_alloc());
    if (!scaled) { throw std::bad_alloc(); }
    scaled->format = AV_PIX_FMT_YUV420P;
    scaled->width  = plan.width;
    scaled->height = plan.height;
    if (av_frame_get_buffer(scaled.get(), 64) < 0) { throw std::bad_alloc(); }

    const double step = 1.0 / plan.fps;
    double next       = segment.start;
    double last_seen  = -1.0;
    // pairs of sampled frames form one merged temporal patch; dedup decides per pair
    std::vector<std::pair<FramePtr, double>> group;
    std::vector<float> kept_luma;
    double kept_time = -1e30;
    int dropped      = 0;
    auto flush_group = [&](bool final) {
        if (group.empty() || (group.size() < 2 && !final)) { return; }
        bool keep = true;
        if (settings.dedup > 0.0) {
            std::vector<float> luma = luma_grid(group.front().first.get());
            if (!kept_luma.empty() && group.front().second - kept_time < 10.0) {
                float change = 0.0F;
                for (std::size_t i = 0; i < luma.size(); ++i) { change = std::max(change, std::fabs(luma[i] - kept_luma[i])); }
                keep = change >= settings.dedup;
            }
            if (keep) {
                kept_luma = std::move(luma);
                kept_time = group.front().second;
            }
        }
        if (keep) {
            if (writer->frames() + static_cast<int>(group.size()) > clip_frames) {
                clips.push_back(writer->finish());
                writer = std::make_unique<ClipWriter>(plan.width, plan.height, plan.fps, plan.display_matrix);
            }
            for (auto& [frame, seconds] : group) { writer->write(frame.get(), seconds); }
        } else {
            dropped += static_cast<int>(group.size());
        }
        group.clear();
    };

    const double half_frame = v->avg_frame_rate.num > 0 ? 0.5 / av_q2d(v->avg_frame_rate) : 0.0;
    auto take = [&](AVFrame* frame, double at) {
        if (!scaler) {
            scaler.reset(sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                        plan.width, plan.height, AV_PIX_FMT_YUV420P, SWS_LANCZOS, nullptr,
                                        nullptr, nullptr));
            if (!scaler) { throw std::runtime_error("cannot create video scaler"); }
        }
        FramePtr out(av_frame_alloc());
        if (!out) { throw std::bad_alloc(); }
        out->format = AV_PIX_FMT_YUV420P;
        out->width  = plan.width;
        out->height = plan.height;
        if (av_frame_get_buffer(out.get(), 64) < 0) { throw std::bad_alloc(); }
        sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, out->data, out->linesize);
        group.emplace_back(std::move(out), at);
        flush_group(false);
    };

    if (step >= 4.0) {
        // sparse sampling (long overviews): seek to every sample instead of decoding everything between
        for (; next < segment.end; next += step) {
            input.seek(v, next, decoder.get());
            decode_stream(input, v, decoder.get(), [&](AVFrame* frame, double seconds) {
                if (seconds >= 0.0 && seconds < next - half_frame) { return true; }
                take(frame, next);
                return false;
            });
        }
    } else {
        decode_stream(input, v, decoder.get(), [&](AVFrame* frame, double seconds) {
            if (seconds < 0.0) { seconds = last_seen < 0.0 ? segment.start : last_seen + step; }
            last_seen = seconds;
            if (seconds < segment.start - 1e-3) { return true; }
            if (seconds >= segment.end || next >= segment.end) { return false; }
            while (next <= seconds + half_frame && next < segment.end) {
                take(frame, next);
                next += step;
            }
            return true;
        });
    }
    flush_group(true);
    if (writer->frames() > 0) { clips.push_back(writer->finish()); }
    if (clips.empty()) {
        throw std::invalid_argument("no video frames in segment " + hms(segment.start) + " - " + hms(segment.end));
    }
    (void)dropped;
    return clips;
}

// ------------------------------------------------------------------ audio + transcript

std::vector<std::int16_t> extract_audio(Input& input, const Segment& segment) {
    AVStream* a      = input.audio();
    CodecPtr decoder = open_decoder(a);
    input.seek(a, segment.start, decoder.get());
    SwrContext* raw = nullptr;
    AVChannelLayout mono{};
    av_channel_layout_default(&mono, 1);
    if (swr_alloc_set_opts2(&raw, &mono, AV_SAMPLE_FMT_S16, 16000, &decoder->ch_layout, decoder->sample_fmt,
                            decoder->sample_rate, 0, nullptr) < 0 ||
        swr_init(raw) < 0) {
        swr_free(&raw);
        throw std::runtime_error("cannot create audio resampler");
    }
    std::unique_ptr<SwrContext, void (*)(SwrContext*)> resampler(raw, [](SwrContext* s) { swr_free(&s); });
    std::vector<std::int16_t> pcm;
    double first  = -1.0;
    const auto wanted = static_cast<std::size_t>(std::llround((segment.end - segment.start) * 16000.0));
    decode_stream(input, a, decoder.get(), [&](AVFrame* frame, double seconds) {
        if (seconds >= segment.end) { return false; }
        if (seconds >= 0.0 && seconds + static_cast<double>(frame->nb_samples) / frame->sample_rate < segment.start) {
            return true;
        }
        if (first < 0.0) { first = seconds >= 0.0 ? seconds : segment.start; }
        const int capacity = swr_get_out_samples(resampler.get(), frame->nb_samples);
        const std::size_t offset = pcm.size();
        pcm.resize(offset + static_cast<std::size_t>(std::max(capacity, 0)));
        auto* out = reinterpret_cast<std::uint8_t*>(pcm.data() + offset);
        const int produced = swr_convert(resampler.get(), &out, capacity,
                                         const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        pcm.resize(offset + static_cast<std::size_t>(std::max(produced, 0)));
        return pcm.size() < wanted + 16000;
    });
    // align the start to the segment and cut the tail
    if (first >= 0.0 && first < segment.start) {
        const auto drop = std::min(pcm.size(), static_cast<std::size_t>(std::llround((segment.start - first) * 16000.0)));
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    if (pcm.size() > wanted) { pcm.resize(wanted); }
    return pcm;
}

std::vector<std::uint8_t> wav_bytes(const std::vector<std::int16_t>& pcm) {
    const auto data_size = static_cast<std::uint32_t>(pcm.size() * 2);
    std::vector<std::uint8_t> out(44 + data_size);
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(out.data() + at, &v, 2); };
    std::memcpy(out.data(), "RIFF", 4);
    put32(4, 36 + data_size);
    std::memcpy(out.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, 16000);
    put32(28, 32000);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(out.data() + 36, "data", 4);
    put32(40, data_size);
    if (!pcm.empty()) { std::memcpy(out.data() + 44, pcm.data(), data_size); }
    return out;
}

std::size_t collect(char* data, std::size_t size, std::size_t count, void* opaque) {
    static_cast<std::string*>(opaque)->append(data, size * count);
    return size * count;
}

struct Spoken {
    double start = 0.0;
    double end   = 0.0;
    std::string text;
};

std::vector<Spoken> transcribe(const std::vector<std::uint8_t>& wav, const std::string& language,
                               const Settings& settings) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::string endpoint = settings.asr_url;
    while (!endpoint.empty() && endpoint.back() == '/') { endpoint.pop_back(); }
    const std::size_t path = endpoint.find('/', endpoint.find("://") + 3);
    if (path == std::string::npos) {
        endpoint += "/v1/audio/transcriptions";
    } else if (endpoint.ends_with("/v1")) {
        endpoint += "/audio/transcriptions";
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) { throw std::runtime_error("cannot create HTTP client"); }
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(curl_mime_init(curl.get()), curl_mime_free);
    curl_mimepart* file = curl_mime_addpart(mime.get());
    curl_mime_name(file, "file");
    curl_mime_data(file, reinterpret_cast<const char*>(wav.data()), wav.size());
    curl_mime_filename(file, "audio.wav");
    curl_mime_type(file, "audio/wav");
    auto field = [&](const char* name, const std::string& value) {
        if (value.empty()) { return; }
        curl_mimepart* part = curl_mime_addpart(mime.get());
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    };
    field("response_format", "verbose_json");
    field("model", settings.asr_model);
    field("language", language);
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10'000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 3'600'000L); // an hour of audio takes a minute or two
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        throw std::runtime_error("speech-to-text server " + settings.asr_url + " is not reachable: " +
                                 curl_easy_strerror(rc));
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        throw std::runtime_error("speech-to-text server returned HTTP " + std::to_string(status) + ": " +
                                 body.substr(0, 300));
    }
    const nlohmann::json parsed = nlohmann::json::parse(body);
    std::vector<Spoken> out;
    for (const auto& s : parsed.value("segments", nlohmann::json::array())) {
        Spoken spoken{s.value("start", 0.0), s.value("end", 0.0), s.value("text", std::string())};
        const auto b = spoken.text.find_first_not_of(" \t\r\n");
        const auto e = spoken.text.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) { continue; }
        spoken.text = spoken.text.substr(b, e - b + 1);
        out.push_back(std::move(spoken));
    }
    return out;
}

std::string transcript_text(const std::vector<Spoken>& spoken, double offset, const std::string& title) {
    std::string out = title + " (speech-to-text; times are seconds in the source):\n";
    if (spoken.empty()) { return out + "(no speech detected)"; }
    for (const Spoken& s : spoken) {
        out += "<" + fixed(offset + s.start) + " - " + fixed(offset + s.end) + " seconds> " + s.text + "\n";
    }
    out.pop_back();
    return out;
}

bool wants_transcript(const std::string& mode, const Settings& settings, const char* what) {
    if (mode == "none") { return false; }
    if (mode != "auto" && mode != "transcript") {
        throw std::invalid_argument(std::string(what) + ".audio must be auto, transcript or none");
    }
    if (settings.asr_url.empty()) {
        if (mode == "transcript") {
            throw std::invalid_argument("transcripts need a speech-to-text server; start ninfer-serve with --asr-url");
        }
        return false;
    }
    return true;
}

} // namespace

int detail_level_tokens(const std::string& name) {
    if (name == "low") { return 256; }
    if (name == "standard" || name == "auto" || name == "medium") { return 576; }
    if (name == "high") { return 1024; }
    if (name == "max") { return 2048; }
    throw std::invalid_argument("detail must be low, standard, high or max (or a number of tokens per frame)");
}

double parse_time(const std::string& text) {
    std::string value = text;
    if (!value.empty() && (value.back() == 's' || value.back() == 'S')) { value.pop_back(); }
    double total      = 0.0;
    std::size_t start = 0;
    int fields        = 0;
    while (true) {
        const std::size_t colon = value.find(':', start);
        const std::string part  = value.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        char* end               = nullptr;
        const double x          = std::strtod(part.c_str(), &end);
        if (part.empty() || end == part.c_str() || *end != '\0' || x < 0.0 || ++fields > 3) {
            throw std::invalid_argument("not a valid time: \"" + text + "\"");
        }
        total = total * 60.0 + x;
        if (colon == std::string::npos) { break; }
        start = colon + 1;
    }
    return total;
}

std::vector<Piece> prepare_video(const media_acquire::Source& source, const MediaRequest& request,
                                 const Settings& settings, const media_acquire::Policy& policy) {
    Input input(source, policy);
    const Plan plan       = make_plan(input, request, settings);
    const bool transcript = input.audio() != nullptr && wants_transcript(request.audio, settings, "video");
    const std::string language = request.language.empty() ? settings.asr_language : request.language;
    std::vector<Piece> out;
    for (std::size_t i = 0; i < plan.segments.size(); ++i) {
        const Segment& segment = plan.segments[i];
        if (plan.explicit_range) {
            const std::string head = plan.segments.size() > 1
                                         ? "Video segment " + std::to_string(i + 1) + " of " +
                                               std::to_string(plan.segments.size())
                                         : std::string("Video segment");
            out.push_back(Piece{false, head + ", source time " + hms(segment.start) + " to " + hms(segment.end) + ":", {}});
        }
        for (auto& clip : extract_clip(input, plan, segment, settings)) {
            out.push_back(Piece{true, {}, std::move(clip)});
        }
        if (transcript) {
            const auto spoken = transcribe(wav_bytes(extract_audio(input, segment)), language, settings);
            out.push_back(Piece{false, transcript_text(spoken, segment.start, "Audio transcript of this video segment"), {}});
        } else if (input.audio() == nullptr && request.audio == "transcript") {
            out.push_back(Piece{false, "(the video has no audio track)", {}});
        }
    }
    return out;
}

std::vector<Piece> prepare_audio(const media_acquire::Source& source, const MediaRequest& request,
                                 const Settings& settings, const media_acquire::Policy& policy) {
    if (!wants_transcript(request.audio == "none" ? "none" : "transcript", settings, "audio")) {
        return {};
    }
    Input input(source, policy);
    if (input.audio() == nullptr) { throw std::invalid_argument("the media has no audio stream"); }
    const std::vector<Segment> segments = resolve_segments(request, input.duration());
    const std::string language          = request.language.empty() ? settings.asr_language : request.language;
    std::vector<Piece> out;
    for (const Segment& segment : segments) {
        const auto spoken = transcribe(wav_bytes(extract_audio(input, segment)), language, settings);
        const std::string title = request.segments.empty()
                                      ? std::string("Audio transcript")
                                      : "Audio transcript, source time " + hms(segment.start) + " to " + hms(segment.end);
        out.push_back(Piece{false, transcript_text(spoken, segment.start, title), {}});
    }
    return out;
}

} // namespace ninfer::product::media_pipeline
