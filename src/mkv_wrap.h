// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

/// @file mkv_wrap.h
/// @see mkv_wrap.cpp
/// @ingroup video_input
///

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include "mkv_wrap_common.h"
#include "secondary_subtitle_packet_stream.h"

#include <memory>
#include <string>

DEFINE_EXCEPTION(MatroskaException, agi::Exception);

class AssFile;
namespace agi { class SingleChoiceInteractionSink; }
namespace agi { class BackgroundRunnerFactory; }

class MatroskaWrapper {
public:
	/// Check if the file is a matroska file with at least one subtitle track
	static bool HasSubtitles(agi::fs::path const& filename);
	/// Scan all tracks in a Matroska file and return their metadata
	static MkvTrackScanResult ScanTracks(agi::fs::path const& filename);
	/// Read exact container origins for a selected Opus audio track with video.
#if AEGISUB_MATROSKA_PARSING
	static std::optional<MkvAudioTimeline> GetOpusAudioTimeline(agi::fs::path const& filename, int audio_ordinal, int audio_count = -1);
#else
	// The legacy subtitle parser does not expose exact Opus timing metadata.
	static std::optional<MkvAudioTimeline> GetOpusAudioTimeline(agi::fs::path const&, int, int = -1) { return std::nullopt; }
#endif
	/// Report text and bitmap subtitle availability without importing a track.
	static MkvSubtitleAvailability GetSubtitleAvailability(agi::fs::path const& filename);
	/// Load subtitles from a matroska file
	/// @param secondary_track_choice when true the multi-track choice dialog is
	///        labelled as loading into the secondary subtitle strip.
	/// @param selected_track_label if non-null, receives a human-readable
	///        description of the track that was actually imported (the same
	///        string shown in the track-choice dialog).
	static void GetSubtitles(agi::fs::path const& filename, AssFile *target, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory = {}, bool secondary_track_choice = false, std::string *selected_track_label = nullptr);
	/// Load one already-selected text subtitle track.
	static void GetTextSubtitlesForTrack(agi::fs::path const& filename, uint64_t track_number, AssFile *target, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory = {});
	/// Extract one already-selected bitmap track as normalized packets.
	static SecondarySubtitlePacketStream GetBitmapSubtitlePacketsForTrack(agi::fs::path const& filename, uint64_t track_number, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory = {});
};
