// Exercise the application provider boundary, including real decode and random seeks.
#include "audio_provider_factory.h"
#include "include/aegisub/video_provider.h"
#include "lsmas_native_api.h"
#include "video_frame.h"
#include "video_provider_manager.h"
#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: lsmas-provider-smoke <video-with-audio>\n";
        return 2;
    }
    try {
        auto path = agi::fs::PathFromString(argv[1]);
        auto video = VideoProviderFactory::GetProviderWithPreferred(path, {}, "LsmasNative", nullptr, {}, size_t{0});
        if (!video || video->GetDecoderName() != "LsmasNative" || video->GetFrameCount() < 2)
            throw std::runtime_error("LsmasNative video was not selected or has no frames");
        VideoFrame first, last, again;
        video->GetFrame(0, first);
        video->GetFrame(video->GetFrameCount() - 1, last);
        video->GetFrame(0, again);
        if (first.data.empty() || last.data.empty() || first.width != size_t(video->GetWidth()) || first.height != size_t(video->GetHeight()))
            throw std::runtime_error("video decode returned invalid frame storage");
        if (first.data != again.data)
            throw std::runtime_error("random video seek changed decoded frame zero");
        auto audio = GetAudioProviderWithPreferred(path, "LsmasNative", nullptr, {});
        if (!audio || GetLastAudioProviderSelectionReport().selected_provider != "LsmasNative")
            throw std::runtime_error("audio silently selected a different provider");
        if (audio->GetChannels() < 1 || audio->GetSampleRate() < 1 || audio->GetNumSamples() < 4096)
            throw std::runtime_error("audio decode returned invalid properties");
        constexpr int count = 1024;
        std::vector<int16_t> start(count), tail(count), repeat(count);
        audio->GetInt16MonoAudioChecked(start.data(), 0, count);
        audio->GetInt16MonoAudioChecked(tail.data(), audio->GetNumSamples() - count, count);
        audio->GetInt16MonoAudioChecked(repeat.data(), 0, count);
        if (std::all_of(start.begin(), start.end(), [](auto x) {return x == 0;}) || std::all_of(tail.begin(), tail.end(), [](auto x) {return x == 0;}))
            throw std::runtime_error("fixture audio is unexpectedly silent");
        if (start != repeat)
            throw std::runtime_error("random audio seek changed decoded starting samples");
        std::cout << "runtime=" << lsmas::GetLoadedLibrary() << '\n'
                  << "video=" << video->GetWidth() << 'x' << video->GetHeight() << " frames=" << video->GetFrameCount() << '\n'
                  << "audio=" << audio->GetSampleRate() << "Hz channels=" << audio->GetChannels() << " samples=" << audio->GetNumSamples() << '\n'
                  << "decode-and-random-seek=passed\n";
    } catch (std::exception const& error) {
        std::cerr << "lsmas-provider-smoke: " << error.what() << '\n';
        return 1;
    }
}
