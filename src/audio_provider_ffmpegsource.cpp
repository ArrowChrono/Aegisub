// Copyright (c) 2008-2009, Karl Blomster
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file audio_provider_ffmpegsource.cpp
/// @brief ffms2-based audio provider
/// @ingroup audio_input ffms
///

#ifdef WITH_FFMS2
#include <libaegisub/audio/provider.h>

#include "ffmpegsource_common.h"
#include "audio_provider_timeline.h"
#include "mkv_wrap.h"
#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <map>

namespace {
bool GetConfiguredFFmpegSourceIndexAllTracks() {
	return config::GetBoolOptionOrDefault("Provider/FFmpegSource/Index All Tracks", false);
}

bool GetConfiguredFFmpegSourceAudioDownmix() {
	return config::GetBoolOptionOrDefault("Provider/Audio/FFmpegSource/Downmix", true);
}

class FFmpegSourceAudioProvider final : public agi::AudioProvider, FFmpegSourceProvider {
	/// audio source object
	agi::scoped_holder<FFMS_AudioSource*, void (FFMS_CC *)(FFMS_AudioSource*)> AudioSource;

	mutable char FFMSErrMsg[1024];			///< FFMS error message
	mutable FFMS_ErrorInfo ErrInfo;			///< FFMS error codes/messages

	std::optional<MkvAudioTimeline> timeline;
	void LoadAudio(agi::fs::path const& filename);
	void FillBuffer(void *Buf, int64_t Start, int64_t Count) const override {
		if (ffms::GetAudio(AudioSource, Buf, Start, Count, &ErrInfo))
			throw agi::AudioDecodeError(std::string("Failed to get audio samples: ") + ErrInfo.Buffer);
	}

public:
	FFmpegSourceAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);

	std::optional<MkvAudioTimeline> const& GetTimeline() const { return timeline; }
	bool NeedsCache() const override { return true; }
	agi::AudioProviderMemoryStats GetMemoryStats() const override { return BuildMemoryStats("FFmpegSource"); }
};

/// @brief Constructor
/// @param filename The filename to open
FFmpegSourceAudioProvider::FFmpegSourceAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) try
: FFmpegSourceProvider(br, std::move(choice_sink))
, AudioSource(nullptr, ffms::DestroyAudioSource)
{
	ErrInfo.Buffer		= FFMSErrMsg;
	ErrInfo.BufferSize	= sizeof(FFMSErrMsg);
	ErrInfo.ErrorType	= FFMS_ERROR_SUCCESS;
	ErrInfo.SubType		= FFMS_ERROR_SUCCESS;
	SetLogLevel();

	LoadAudio(filename);
}
catch (agi::EnvironmentError const& err) {
	throw agi::AudioProviderError(err.GetMessage());
}

void FFmpegSourceAudioProvider::LoadAudio(agi::fs::path const& filename) {
	auto const filename_utf8 = agi::fs::PathToString(filename);
	FFMS_Indexer *Indexer = ffms::CreateIndexer(filename_utf8.c_str(), &ErrInfo);
	if (!Indexer) {
		if (ErrInfo.SubType == FFMS_ERROR_FILE_READ)
			throw agi::fs::FileNotFound(std::string(ErrInfo.Buffer));
		else
			throw agi::AudioDataNotFound(ErrInfo.Buffer);
	}

	auto cancel_indexer = agi::make_scope_exit([&] { ffms::CancelIndexing(Indexer); });
	auto TrackList = GetTracksOfType(filename, Indexer, FFMS_TYPE_AUDIO);

	// initialize the track number to an invalid value so we can detect later on
	// whether the user actually had to choose a track or not
	int TrackNumber = -1;
	if (TrackList.size() > 1) {
		auto Selection = AskForTrackSelection(TrackList, FFMS_TYPE_AUDIO);
		if (Selection == TrackSelection::None)
			throw agi::UserCancelException("audio loading canceled by user");
		TrackNumber = static_cast<int>(Selection);
	}
	else if (TrackList.size() == 1)
		TrackNumber = TrackList.front().ffms_track_index;
	else
		throw agi::AudioDataNotFound("no audio tracks found");

	// The UI list omits streams with unavailable codecs; count the raw audio
	// streams instead so a skipped choice cannot change the Matroska ordinal.
	int audio_ordinal = 0;
	int audio_count = 0;
	for (int i = 0; i < ffms::GetNumTracksI(Indexer); ++i) {
		if (ffms::GetTrackTypeI(Indexer, i) == FFMS_TYPE_AUDIO) {
			if (i < TrackNumber)
				++audio_ordinal;
			++audio_count;
		}
	}
	timeline = MatroskaWrapper::GetOpusAudioTimeline(filename, audio_ordinal, audio_count);

	// generate a name for the cache file
	agi::fs::path CacheName = GetCacheFilename(filename);

	// try to read index
	auto const cache_name_utf8 = agi::fs::PathToString(CacheName);
	agi::scoped_holder<FFMS_Index*, void (FFMS_CC*)(FFMS_Index*)>
		Index(ffms::ReadIndex(cache_name_utf8.c_str(), &ErrInfo), ffms::DestroyIndex);

	if (Index && ffms::IndexBelongsToFile(Index, filename_utf8.c_str(), &ErrInfo))
		Index = nullptr;

	if (Index) {
		// we already have an index, but the desired track may not have been
		// indexed, and if it wasn't we need to reindex
		FFMS_Track *TempTrackData = ffms::GetTrackFromIndex(Index, TrackNumber);
		if (ffms::GetNumFrames(TempTrackData) <= 0)
			Index = nullptr;
	}

	// reindex if the error handling mode has changed
	FFMS_IndexErrorHandling ErrorHandling = GetErrorHandlingMode();
#if FFMS_VERSION >= ((2 << 24) | (17 << 16) | (2 << 8) | 0)
	if (Index && ffms::GetErrorHandling(Index) != ErrorHandling)
		Index = nullptr;
#endif

	// moment of truth
	if (!Index) {
		TrackSelection TrackMask = static_cast<TrackSelection>(TrackNumber);
		if (GetConfiguredFFmpegSourceIndexAllTracks())
			TrackMask = TrackSelection::All;
		cancel_indexer.release(); // DoIndexing consumes the FFMS indexer.
		Index = DoIndexing(Indexer, CacheName, TrackMask, ErrorHandling);
	}
	else {
		ffms::CancelIndexing(Indexer);
		cancel_indexer.release();
	}

	// update access time of index file so it won't get cleaned away
	agi::fs::Touch(CacheName);

	AudioSource = ffms::CreateAudioSource(filename_utf8.c_str(), TrackNumber, Index, timeline ? FFMS_DELAY_NO_SHIFT : FFMS_DELAY_FIRST_VIDEO_TRACK, &ErrInfo);
	if (!AudioSource)
		throw agi::AudioProviderError(std::string("Failed to open audio track: ") + ErrInfo.Buffer);

	const FFMS_AudioProperties AudioInfo = *ffms::GetAudioProperties(AudioSource);

	channels	= AudioInfo.Channels;
	sample_rate	= AudioInfo.SampleRate;
	num_samples = AudioInfo.NumSamples;
	decoded_samples = AudioInfo.NumSamples;
	if (channels <= 0 || sample_rate <= 0 || num_samples <= 0)
		throw agi::AudioProviderError("sanity check failed, consult your local psychiatrist");

	switch (AudioInfo.SampleFormat) {
		case FFMS_FMT_U8:  bytes_per_sample = 1; float_samples = false; break;
		case FFMS_FMT_S16: bytes_per_sample = 2; float_samples = false; break;
		case FFMS_FMT_S32: bytes_per_sample = 4; float_samples = false; break;
		case FFMS_FMT_FLT: bytes_per_sample = 4; float_samples = true; break;
		case FFMS_FMT_DBL: bytes_per_sample = 8; float_samples = true; break;
		default:
			throw agi::AudioProviderError("unknown or unsupported sample format");
	}

#if FFMS_VERSION >= ((2 << 24) | (17 << 16) | (4 << 8) | 0)
	if (GetConfiguredFFmpegSourceAudioDownmix()) {
		if (channels > 1 || bytes_per_sample != 2 || float_samples) {
			std::unique_ptr<FFMS_ResampleOptions, decltype(ffms::DestroyResampleOptions)>
				opt(ffms::CreateResampleOptions(AudioSource), ffms::DestroyResampleOptions);
			opt->ChannelLayout = FFMS_CH_FRONT_CENTER;
			opt->SampleFormat = FFMS_FMT_S16;

			// Might fail if FFMS2 wasn't built with libavresample
			if (!ffms::SetOutputFormatA(AudioSource, opt.get(), nullptr)) {
				channels = 1;
				bytes_per_sample = 2;
				float_samples = false;
			}
		}
	}
#endif
}

}

std::unique_ptr<agi::AudioProvider> CreateFFmpegSourceAudioProvider(agi::fs::path const& file, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto provider = agi::make_unique<FFmpegSourceAudioProvider>(file, br, std::move(choice_sink));
	auto const timeline = provider->GetTimeline();
	return ApplyMatroskaAudioTimeline(std::move(provider), timeline);
}

#endif /* WITH_FFMS2 */
