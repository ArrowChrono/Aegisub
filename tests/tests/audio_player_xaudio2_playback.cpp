#include <main.h>

#include "../../src/audio_player_xaudio2_playback.h"

#include <chrono>

namespace {
using namespace std::chrono_literals;
using aegisub::xaudio2::ConsumedFrame;
using aegisub::xaudio2::DecideEndUpdate;
using aegisub::xaudio2::EndUpdate;
using aegisub::xaudio2::PlaybackClock;
auto const origin = PlaybackClock::Clock::time_point{};
}

TEST(xaudio2_playback, end_inside_unplayed_queue_rebuilds_instead_of_stopping) {
	EXPECT_EQ(EndUpdate::Rebuild, DecideEndUpdate(100, 500, 1000, 300));
	EXPECT_EQ(EndUpdate::Rebuild, DecideEndUpdate(100, 500, 1000, 500));
	EXPECT_EQ(EndUpdate::Rebuild, DecideEndUpdate(100, 500, 1000, 101));
}

TEST(xaudio2_playback, end_at_or_before_consumed_position_stops) {
	EXPECT_EQ(EndUpdate::Stop, DecideEndUpdate(100, 500, 1000, 100));
	EXPECT_EQ(EndUpdate::Stop, DecideEndUpdate(100, 500, 1000, 99));
}

TEST(xaudio2_playback, extension_replaces_an_already_queued_eos_marker) {
	EXPECT_EQ(EndUpdate::Rebuild, DecideEndUpdate(100, 300, 300, 600));
	EXPECT_EQ(EndUpdate::Rebuild, DecideEndUpdate(300, 300, 300, 600));
	EXPECT_EQ(EndUpdate::Unchanged, DecideEndUpdate(100, 300, 300, 300));
}

TEST(xaudio2_playback, end_changes_beyond_queue_preserve_existing_buffers) {
	EXPECT_EQ(EndUpdate::Refill, DecideEndUpdate(100, 500, 1000, 501));
	EXPECT_EQ(EndUpdate::Refill, DecideEndUpdate(100, 500, 1000, 1200));
	EXPECT_EQ(EndUpdate::Unchanged, DecideEndUpdate(100, 500, 1000, 1000));
}

TEST(xaudio2_playback, consumed_frames_use_the_stream_epoch_and_submitted_limit) {
	EXPECT_EQ(1025, ConsumedFrame(1000, 1500, 400, 425, false));
	EXPECT_EQ(1000, ConsumedFrame(1000, 1500, 400, 0, false));
	EXPECT_EQ(1500, ConsumedFrame(1000, 1500, 400, 2000, false));
	EXPECT_EQ(1500, ConsumedFrame(1000, 1500, 400, 0, true));
}

TEST(xaudio2_playback, clock_interpolates_below_the_device_processing_quantum) {
	PlaybackClock clock;
	clock.Reset(1000, origin);
	clock.Publish(1000, 5800, origin, true);
	EXPECT_EQ(1048, clock.Position(origin + 1ms, 48000));
	EXPECT_EQ(1120, clock.Position(origin + 2500us, 48000));
	EXPECT_EQ(1240, clock.Position(origin + 5ms, 48000));
}

TEST(xaudio2_playback, clock_is_monotonic_across_quantized_device_observations) {
	PlaybackClock clock;
	clock.Reset(0, origin);
	clock.Publish(0, 4800, origin, true);
	EXPECT_EQ(240, clock.Position(origin + 5ms, 48000));
	clock.Publish(0, 4800, origin + 5ms, true);
	EXPECT_EQ(240, clock.Position(origin + 6ms, 48000));
	clock.Publish(480, 4800, origin + 10ms, true);
	EXPECT_EQ(528, clock.Position(origin + 11ms, 48000));
}

TEST(xaudio2_playback, blocked_worker_cannot_advance_clock_past_submitted_audio) {
	PlaybackClock clock;
	clock.Reset(2000, origin);
	clock.Publish(2000, 6800, origin, true);
	EXPECT_EQ(4400, clock.Position(origin + 50ms, 48000));
	EXPECT_EQ(6800, clock.Position(origin + 100ms, 48000));
	// No worker observation arrives while the provider is blocked for seconds.
	EXPECT_EQ(6800, clock.Position(origin + 5s, 48000));
	EXPECT_EQ(6800, clock.Position(origin + 10s, 48000));
}

TEST(xaudio2_playback, recovery_reanchors_without_catching_up_starvation_time) {
	PlaybackClock clock;
	clock.Reset(0, origin);
	clock.Publish(0, 4800, origin, true);
	EXPECT_EQ(4800, clock.Position(origin + 2s, 48000));
	clock.Publish(4800, 9600, origin + 2s, true);
	EXPECT_EQ(4800, clock.Position(origin + 2s, 48000));
	EXPECT_EQ(5280, clock.Position(origin + 2010ms, 48000));
	EXPECT_EQ(9600, clock.Position(origin + 3s, 48000));
}

TEST(xaudio2_playback, internal_queue_rebuild_freezes_without_rewinding_cursor) {
	PlaybackClock clock;
	clock.Reset(1000, origin);
	clock.Publish(1000, 5800, origin, true);
	EXPECT_EQ(2440, clock.Position(origin + 30ms, 48000));
	clock.Publish(2200, 4000, origin + 30ms, false);
	EXPECT_EQ(2440, clock.Position(origin + 1s, 48000));
	clock.Publish(2200, 4000, origin + 1s, true);
	EXPECT_EQ(2440, clock.Position(origin + 1001ms, 48000));
	EXPECT_EQ(2680, clock.Position(origin + 1010ms, 48000));
	EXPECT_EQ(4000, clock.Position(origin + 2s, 48000));
}

TEST(xaudio2_playback, explicit_seek_resets_old_monotonic_position_and_eof_stays_clamped) {
	PlaybackClock clock;
	clock.Reset(5000, origin);
	clock.Publish(5000, 9800, origin, true);
	EXPECT_EQ(9800, clock.Position(origin + 1s, 48000));
	clock.Reset(100, origin + 1s);
	EXPECT_EQ(100, clock.Position(origin + 2s, 48000));
	clock.Publish(300, 300, origin + 2s, false);
	EXPECT_EQ(300, clock.Position(origin + 10s, 48000));
}
