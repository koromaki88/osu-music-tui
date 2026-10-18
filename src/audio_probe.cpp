#include "osu_player/audio_probe.hpp"

#include <memory>
#include <stdexcept>
#include <string>

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
    AVFormatContext* raw_context = nullptr;
    const std::string path_text = path.string();

    const int open_result = avformat_open_input(&raw_context, path_text.c_str(), nullptr, nullptr);
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
