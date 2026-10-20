#include "osu_player/audio_probe.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace osu_player {
namespace {

[[nodiscard]] std::string ffmpeg_error(const int error_code)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error_code, message, sizeof(message));
    return message;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept
    {
        avformat_close_input(&context);
    }
};

using FormatContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

}  // namespace

AudioMetadata probe_audio(const std::filesystem::path& path)
{
    return probe_audio(path, {});
}

AudioMetadata probe_audio(const std::filesystem::path& path, const std::stop_token stop)
{
    // Anchor a regular local file; reject FIFOs/devices and protocol URLs before
    // FFmpeg can block opening them or interpret them as a remote resource.
    struct File { int fd; ~File() { if (fd >= 0) { close(fd); } } };
    File file{open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    struct stat info{};
    if (file.fd < 0 || fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        throw std::runtime_error{"audio probe requires a readable regular local file"};
    }
    struct Interrupt {
        std::stop_token stop;
        std::chrono::steady_clock::time_point deadline;
    } interrupt{stop, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
    AVFormatContext* raw_context = avformat_alloc_context();
    if (!raw_context) { throw std::bad_alloc{}; }
    raw_context->interrupt_callback = {[](void* opaque) {
        const auto& state = *static_cast<Interrupt*>(opaque);
        return state.stop.stop_requested() || std::chrono::steady_clock::now() >= state.deadline ? 1 : 0;
    }, &interrupt};
    const std::string path_text = "/proc/self/fd/" + std::to_string(file.fd);
    AVDictionary* options{};
    av_dict_set(&options, "protocol_whitelist", "file", 0);

    const int open_result = avformat_open_input(&raw_context, path_text.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (open_result < 0) {
        throw std::runtime_error{"could not open audio: " + ffmpeg_error(open_result)};
    }

    FormatContext context{raw_context};
    const int stream_info_result = avformat_find_stream_info(context.get(), nullptr);
    if (stream_info_result < 0) {
        throw std::runtime_error{"could not read audio stream information: " +
                                 ffmpeg_error(stream_info_result)};
    }

    const int stream_index = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        throw std::runtime_error{"file contains no playable audio stream: " +
                                 ffmpeg_error(stream_index)};
    }

    const AVStream* const stream = context->streams[stream_index];
    const AVCodecParameters* const parameters = stream->codecpar;
    const AVCodecDescriptor* const codec = avcodec_descriptor_get(parameters->codec_id);

    AudioMetadata result;
    result.container = context->iformat != nullptr && context->iformat->name != nullptr
                           ? context->iformat->name
                           : "unknown";
    result.codec = codec != nullptr && codec->name != nullptr ? codec->name : "unknown";
    result.sample_rate = parameters->sample_rate;
    result.channels = parameters->ch_layout.nb_channels;

    if (context->duration != AV_NOPTS_VALUE && context->duration > 0) {
        result.duration_ms = context->duration / (AV_TIME_BASE / 1000);
    }

    return result;
}

}  // namespace osu_player
