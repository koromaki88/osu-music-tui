#include "osu_player/audio_player.hpp"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace osu_player {
namespace {

constexpr int output_rate = 48'000;
constexpr int output_channels = 2;
constexpr std::uint32_t bytes_per_frame = sizeof(float) * output_channels;
constexpr std::uint32_t queue_target = output_rate * bytes_per_frame * 120 / 1000;

[[noreturn]] void fail_ffmpeg(const char* operation, const int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error{std::string{operation} + ": " + message};
}

struct FormatDeleter {
    void operator()(AVFormatContext* value) const noexcept { avformat_close_input(&value); }
};
struct CodecDeleter {
    void operator()(AVCodecContext* value) const noexcept { avcodec_free_context(&value); }
};
struct ResamplerDeleter {
    void operator()(SwrContext* value) const noexcept { swr_free(&value); }
};
struct PacketDeleter {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};
struct FrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

class Media {
public:
    explicit Media(const std::filesystem::path& path)
    {
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error{"audio source is not a regular file: " + path.string()};
        }
        AVFormatContext* raw_format = nullptr;
        int result = avformat_open_input(&raw_format, path.c_str(), nullptr, nullptr);
        if (result < 0) {
            fail_ffmpeg("open audio", result);
        }
        format.reset(raw_format);
        result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0) {
            fail_ffmpeg("read audio streams", result);
        }

        const AVCodec* decoder = nullptr;
        stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (stream_index < 0) {
            fail_ffmpeg("find audio decoder", stream_index);
        }
        codec.reset(avcodec_alloc_context3(decoder));
        if (!codec) {
            throw std::bad_alloc{};
        }
        result = avcodec_parameters_to_context(codec.get(), stream()->codecpar);
        if (result < 0) {
            fail_ffmpeg("initialize decoder parameters", result);
        }
        result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0) {
            fail_ffmpeg("open decoder", result);
        }
        if (codec->sample_rate <= 0 || codec->ch_layout.nb_channels <= 0) {
            throw std::runtime_error{"audio stream has an invalid sample rate or channel layout"};
        }

        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* raw_resampler = nullptr;
        result = swr_alloc_set_opts2(&raw_resampler, &output_layout, AV_SAMPLE_FMT_FLT,
                                    output_rate, &codec->ch_layout, codec->sample_fmt,
                                    codec->sample_rate, 0, nullptr);
        resampler.reset(raw_resampler);
        if (result < 0) {
            fail_ffmpeg("configure audio resampler", result);
        }
        result = swr_init(resampler.get());
        if (result < 0) {
            fail_ffmpeg("initialize audio resampler", result);
        }
        packet.reset(av_packet_alloc());
        frame.reset(av_frame_alloc());
        if (!packet || !frame) {
            throw std::bad_alloc{};
        }

        if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
            duration = AudioPlayer::Milliseconds{format->duration / (AV_TIME_BASE / 1000)};
        }
    }

    [[nodiscard]] AVStream* stream() const { return format->streams[stream_index]; }

    void seek(const AudioPlayer::Milliseconds target)
    {
        const std::int64_t timestamp = av_rescale_q(target.count(), AVRational{1, 1000},
                                                   stream()->time_base) + start_time();
        const int result = avformat_seek_file(format.get(), stream_index,
            std::numeric_limits<std::int64_t>::min(), timestamp, timestamp, 0);
        if (result < 0) {
            fail_ffmpeg("seek audio", result);
        }
        avcodec_flush_buffers(codec.get());
        av_packet_unref(packet.get());
        av_frame_unref(frame.get());
        swr_close(resampler.get());
        const int reset_result = swr_init(resampler.get());
        if (reset_result < 0) {
            fail_ffmpeg("reset resampler", reset_result);
        }
        demux_finished = false;
        decoder_finished = false;
        discard_before_ms = target.count();
    }

    // Decode one frame into interleaved floating-point stereo. Empty output can
    // mean seek preroll; false means the decoder and resampler are both drained.
    bool next(std::vector<float>& samples)
    {
        samples.clear();
        if (decoder_finished) {
            return convert(samples, nullptr, 0) > 0;
        }
        while (true) {
            int result = avcodec_receive_frame(codec.get(), frame.get());
            if (result >= 0) {
                convert(samples, const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
                // Seeking is packet-based; discard decoded preroll before the
                // requested point, rather than playing the earlier packet.
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    const auto frame_ms = av_rescale_q(frame->best_effort_timestamp - start_time(),
                                                       stream()->time_base, AVRational{1, 1000});
                    if (frame_ms < discard_before_ms) {
                        const auto frames_to_skip = std::min<std::int64_t>(
                            (discard_before_ms - frame_ms) * output_rate / 1000,
                            static_cast<std::int64_t>(samples.size() / output_channels));
                        samples.erase(samples.begin(), samples.begin() + frames_to_skip * output_channels);
                    }
                }
                av_frame_unref(frame.get());
                return true;
            }
            if (result == AVERROR_EOF) {
                decoder_finished = true;
                return convert(samples, nullptr, 0) > 0;
            }
            if (result != AVERROR(EAGAIN)) {
                fail_ffmpeg("decode audio frame", result);
            }
            if (demux_finished) {
                throw std::runtime_error{"decoder requested more packets after its end-of-stream signal"};
            }
            do {
                av_packet_unref(packet.get());
                result = av_read_frame(format.get(), packet.get());
            } while (result >= 0 && packet->stream_index != stream_index);

            if (result == AVERROR_EOF) {
                demux_finished = true;
                result = avcodec_send_packet(codec.get(), nullptr);
            } else if (result >= 0) {
                result = avcodec_send_packet(codec.get(), packet.get());
                av_packet_unref(packet.get());
            } else {
                fail_ffmpeg("read audio packet", result);
            }
            if (result < 0) {
                fail_ffmpeg("submit audio packet", result);
            }
        }
    }

    std::optional<AudioPlayer::Milliseconds> duration;

private:
    [[nodiscard]] std::int64_t start_time() const
    {
        return stream()->start_time == AV_NOPTS_VALUE ? 0 : stream()->start_time;
    }

    int convert(std::vector<float>& samples, const std::uint8_t** input, const int count)
    {
        const auto capacity = av_rescale_rnd(swr_get_delay(resampler.get(), codec->sample_rate) + count,
                                             output_rate, codec->sample_rate, AV_ROUND_UP);
        // Bound a malformed stream's requested allocation as well as int casts.
        if (capacity < 0 || capacity > output_rate * 10) {
            throw std::runtime_error{"decoded audio frame exceeds the buffering limit"};
        }
        samples.resize(static_cast<std::size_t>(capacity) * output_channels);
        if (capacity == 0) {
            return 0;
        }
        std::uint8_t* output = reinterpret_cast<std::uint8_t*>(samples.data());
        const int converted = swr_convert(resampler.get(), &output, static_cast<int>(capacity), input, count);
        if (converted < 0) {
            fail_ffmpeg("resample audio", converted);
        }
        samples.resize(static_cast<std::size_t>(converted) * output_channels);
        return converted;
    }

    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    std::unique_ptr<SwrContext, ResamplerDeleter> resampler;
    std::unique_ptr<AVPacket, PacketDeleter> packet;
    std::unique_ptr<AVFrame, FrameDeleter> frame;
    int stream_index{};
    bool demux_finished{};
    bool decoder_finished{};
    std::int64_t discard_before_ms{};
};

class OutputDevice {
public:
    OutputDevice()
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            throw AudioOutputError{"initialize audio output: " + std::string{SDL_GetError()}};
        }
        SDL_AudioSpec desired{};
        desired.freq = output_rate;
        desired.format = AUDIO_F32SYS;
        desired.channels = output_channels;
        desired.samples = 1024;
        // No callback: SDL owns its copied queue; the control thread pumps decoding.
        id = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        if (id == 0) {
            const std::string error = SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            throw AudioOutputError{"open audio output device: " + error};
        }
    }
    ~OutputDevice()
    {
        SDL_CloseAudioDevice(id);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    OutputDevice(const OutputDevice&) = delete;
    OutputDevice& operator=(const OutputDevice&) = delete;

    SDL_AudioDeviceID id{};
};

class StreamingAudioPlayer final : public AudioPlayer {
public:
    void load(const std::filesystem::path& path) override
    {
        // Preserve the old source if opening the replacement fails.
        auto candidate = std::make_unique<Media>(path);
        if (!device_) {
            device_ = std::make_unique<OutputDevice>();
        }
        SDL_PauseAudioDevice(device_->id, 1);
        SDL_ClearQueuedAudio(device_->id);
        media_ = std::move(candidate);
        submitted_frames_ = 0;
        anchor_ = Milliseconds{0};
        drained_ = false;
        state_ = PlaybackState::ready;
    }

    void play() override
    {
        require_media();
        if (state_ == PlaybackState::finished) {
            seek(Milliseconds{0});
        }
        state_ = PlaybackState::playing;
        update();
        SDL_PauseAudioDevice(device_->id, 0);
    }

    void pause() override
    {
        if (state_ == PlaybackState::playing) {
            SDL_PauseAudioDevice(device_->id, 1);
            state_ = PlaybackState::paused;
        }
    }

    void stop() override
    {
        if (media_) {
            pause();
            seek(Milliseconds{0});
            state_ = PlaybackState::ready;
        }
    }

    void seek(Milliseconds target) override
    {
        require_media();
        target = std::max(target, Milliseconds{0});
        if (media_->duration) {
            target = std::min(target, *media_->duration);
        }
        media_->seek(target);
        SDL_ClearQueuedAudio(device_->id);
        anchor_ = target;
        submitted_frames_ = 0;
        drained_ = false;
        if (state_ == PlaybackState::finished) {
            state_ = PlaybackState::paused;
        }
    }

    void set_volume(const float value) override
    {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            throw std::invalid_argument{"volume must be finite and between 0 and 1"};
        }
        volume_ = value;
    }

    void update() override
    {
        if (state_ != PlaybackState::playing) {
            return;
        }
        try {
            while (!drained_ && SDL_GetQueuedAudioSize(device_->id) < queue_target) {
                if (!media_->next(samples_)) {
                    drained_ = true;
                    break;
                }
                if (samples_.empty()) {
                    continue;
                }
                for (float& sample : samples_) {
                    sample *= volume_;
                }
                const auto bytes = static_cast<Uint32>(samples_.size() * sizeof(float));
                if (SDL_QueueAudio(device_->id, samples_.data(), bytes) != 0) {
                    throw AudioOutputError{"queue audio output: " + std::string{SDL_GetError()}};
                }
                submitted_frames_ += samples_.size() / output_channels;
            }
            if (drained_ && SDL_GetQueuedAudioSize(device_->id) == 0) {
                SDL_PauseAudioDevice(device_->id, 1);
                state_ = PlaybackState::finished;
            }
        } catch (...) {
            SDL_PauseAudioDevice(device_->id, 1);
            state_ = PlaybackState::paused;
            throw;
        }
    }

    [[nodiscard]] PlaybackState state() const noexcept override { return state_; }

    [[nodiscard]] Milliseconds position() const noexcept override
    {
        const std::uint64_t queued = device_ ? SDL_GetQueuedAudioSize(device_->id) / bytes_per_frame : 0;
        const std::uint64_t played = submitted_frames_ - std::min(submitted_frames_, queued);
        const auto value = anchor_ + Milliseconds{static_cast<std::int64_t>(played * 1000 / output_rate)};
        return duration() ? std::min(value, *duration()) : value;
    }

    [[nodiscard]] std::optional<Milliseconds> duration() const noexcept override
    {
        return media_ ? media_->duration : std::nullopt;
    }

    [[nodiscard]] float volume() const noexcept override { return volume_; }

private:
    void require_media() const
    {
        if (!media_) {
            throw std::logic_error{"no audio source is loaded"};
        }
    }

    std::unique_ptr<Media> media_;
    std::unique_ptr<OutputDevice> device_;
    std::vector<float> samples_;
    PlaybackState state_{PlaybackState::empty};
    Milliseconds anchor_{};
    std::uint64_t submitted_frames_{};
    float volume_{0.7F};
    bool drained_{};
};

}  // namespace

std::unique_ptr<AudioPlayer> make_audio_player()
{
    return std::make_unique<StreamingAudioPlayer>();
}

}  // namespace osu_player
