#include "include/aegisub/audio_player.h"
#include "options.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

std::unique_ptr<AudioPlayer> CreateXAudio2Player(agi::AudioProvider *, AudioPlayerHost const&);

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr int rate = 48000;

void Require(bool condition, char const *message) {
	if (!condition)
		throw std::runtime_error(message);
}

template <typename Predicate>
void WaitFor(Predicate predicate, std::chrono::milliseconds timeout, char const *message) {
	auto const deadline = Clock::now() + timeout;
	while (!predicate()) {
		Require(Clock::now() < deadline, message);
		std::this_thread::sleep_for(2ms);
	}
}

// Real XAudio2 output remains silent. One controlled decode stall exercises
// the clock while the worker cannot poll GetState or service commands.
class SilentProvider final : public agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable released;
	mutable bool allow_read = false;
	mutable std::atomic<bool> block_next{false};
	mutable std::atomic<int64_t> block_before{std::numeric_limits<int64_t>::max()};
	mutable std::atomic<int64_t> blocked_at{-1};
	mutable std::atomic<int> completed_reads{0};
	mutable std::atomic<int64_t> last_end{0};
	mutable std::atomic<bool> timed_out{false};

	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		if (start < block_before.load() && block_next.exchange(false)) {
			std::unique_lock lock(mutex);
			blocked_at.store(start);
			if (!released.wait_for(lock, 2s, [&] { return allow_read; })) {
				timed_out.store(true);
				throw agi::AudioDecodeError("Timed out releasing the controlled XAudio2 smoke read.");
			}
		}
		std::fill_n(static_cast<int16_t *>(buffer), count, 0);
		last_end.store(start + count);
		completed_reads.fetch_add(1);
	}

	public:
	SilentProvider() {
		channels = 1;
		sample_rate = rate;
		bytes_per_sample = 2;
		num_samples = decoded_samples = rate * 10;
	}
	void BlockNextRead(int64_t before = std::numeric_limits<int64_t>::max()) {
		std::scoped_lock lock(mutex);
		allow_read = false;
		blocked_at.store(-1);
		block_before.store(before);
		block_next.store(true);
	}
	void ReleaseRead() {
		std::scoped_lock lock(mutex);
		allow_read = true;
		released.notify_all();
	}
	[[nodiscard]] int ReadCount() const { return completed_reads.load(); }
	[[nodiscard]] int64_t LastEnd() const { return last_end.load(); }
	[[nodiscard]] int64_t BlockedAt() const { return blocked_at.load(); }
	[[nodiscard]] bool TimedOut() const { return timed_out.load(); }
};

void CheckEndpointChanges() {
	SilentProvider provider;
	auto player = CreateXAudio2Player(&provider, {});
	auto release = agi::make_scope_exit([&] { provider.ReleaseRead(); });
	player->Play(0, rate * 2);
	Require(player->IsPlaying(), "XAudio2 did not start playback");
	WaitFor([&] { return player->GetCurrentPosition() >= rate / 20; }, 500ms, "initial playback did not consume audio");
	auto reads = provider.ReadCount();
	auto const shortened_end = rate / 4;
	provider.BlockNextRead(shortened_end);
	player->SetEndPosition(shortened_end);
	WaitFor([&] { return provider.BlockedAt() >= 0; }, 500ms, "shortening queued audio stopped instead of rebuilding it");
	Require(provider.BlockedAt() >= rate / 50, "endpoint rebuild replayed already consumed audio from the origin");
	std::cout << "XAudio2 endpoint resume_sample=" << provider.BlockedAt() << " end_sample=" << shortened_end << '\n';
	Require(player->IsPlaying(), "internal endpoint rebuild published a stopped transport while refilling");
	auto const frozen = player->GetCurrentPosition();
	std::this_thread::sleep_for(60ms);
	Require(player->IsPlaying(), "endpoint rebuild did not preserve transport during a blocked refill");
	Require(player->GetCurrentPosition() == frozen, "endpoint rebuild advanced its clock while refilling");
	provider.ReleaseRead();
	WaitFor([&] { return provider.ReadCount() > reads && provider.LastEnd() == shortened_end; }, 500ms,
			"rebuilt queue did not reach the exact shortened endpoint");
	Require(player->IsPlaying(), "internal endpoint rebuild published a stopped transport");
	Require(provider.LastEnd() == shortened_end, "rebuilt queue was not clipped to the new endpoint");
	WaitFor([&] { return !player->IsPlaying(); }, 1s, "shortened playback did not finish");

	player->Play(rate, rate / 5);
	WaitFor([&] { return player->GetCurrentPosition() >= rate + rate / 20; }, 500ms, "nonzero-origin playback did not consume audio");
	reads = provider.ReadCount();
	auto const extended_end = rate + rate * 3 / 5;
	player->SetEndPosition(extended_end);
	WaitFor([&] { return provider.ReadCount() > reads; }, 500ms, "extending a queued EOS did not refill playback");
	auto const deadline = Clock::now() + 2s;
	int64_t previous = rate;
	bool passed_old_end = false;
	while (player->IsPlaying()) {
		Require(Clock::now() < deadline, "extended playback did not finish");
		auto const position = player->GetCurrentPosition();
		if (player->IsPlaying()) {
			Require(position >= previous, "extended playback cursor moved backwards at the old EOS");
			Require(position <= extended_end, "extended playback cursor exceeded its endpoint");
			passed_old_end = passed_old_end || position > rate + rate / 5;
			previous = position;
		}
		std::this_thread::sleep_for(2ms);
	}
	Require(passed_old_end, "playback did not pass the replaced EOS marker");
	for (int seek = 0; seek < 6; ++seek) {
		auto const start = rate + seek * 480;
		player->Play(start, rate / 2);
		Require(player->IsPlaying(), "rapid restart lost the playing state");
		Require(player->GetCurrentPosition() >= start, "rapid restart reused the previous counter epoch");
	}
	player->Stop();
	WaitFor([&] { return !player->IsPlaying(); }, 500ms, "rapid-restarted playback did not stop");
	std::cout << "PASS XAudio2 queued endpoint shrink and EOS extension\n";
}

void CheckBlockedWorkerClock() {
	SilentProvider provider;
	auto player = CreateXAudio2Player(&provider, {});
	auto release = agi::make_scope_exit([&] { provider.ReleaseRead(); });
	player->Play(0, rate * 2);
	provider.BlockNextRead();
	WaitFor([&] { return provider.BlockedAt() >= 0; }, 1s, "worker did not enter the controlled decode stall");
	auto const submitted_end = provider.BlockedAt();
	std::this_thread::sleep_for(500ms);
	Require(player->IsPlaying(), "starvation incorrectly completed playback");
	Require(player->GetCurrentPosition() == submitted_end, "cursor advanced past submitted audio while worker was blocked");
	std::this_thread::sleep_for(60ms);
	Require(player->GetCurrentPosition() == submitted_end, "cursor did not stay frozen at the dry queue boundary");
	auto const reads = provider.ReadCount();
	provider.ReleaseRead();
	WaitFor([&] { return provider.ReadCount() > reads; }, 500ms, "worker did not recover after the decode stall");
	WaitFor([&] { return player->GetCurrentPosition() > submitted_end; }, 500ms, "clock did not resume after the decode stall");
	auto const position = player->GetCurrentPosition();
	Require(position >= submitted_end && position < submitted_end + rate / 5,
			"recovery caught up wall-clock starvation time instead of resuming at consumed audio");
	Require(!provider.TimedOut(), "controlled provider stall exceeded its finite timeout");
	player->Stop();
	WaitFor([&] { return !player->IsPlaying(); }, 500ms, "XAudio2 did not stop after recovery");
	std::cout << "PASS XAudio2 worker-blocked starvation and recovery clock\n";
}
}

int main() {
	agi::dispatch::Init([](agi::dispatch::Thunk const& task) { task(); });
	auto shutdown = agi::make_scope_exit([] { agi::dispatch::Shutdown(); });
	agi::log::LogSink log;
	agi::log::log = &log;
	auto reset_log = agi::make_scope_exit([] { agi::log::log = nullptr; });
	agi::Options options("", R"({"Player":{"Audio":{"DirectSound":{"Buffer Latency":100,"Buffer Length":4}}}})", agi::Options::FLUSH_SKIP);
	config::opt = &options;
	auto reset_options = agi::make_scope_exit([] { config::opt = nullptr; });
	try {
		CheckEndpointChanges();
		CheckBlockedWorkerClock();
		return 0;
	}
	catch (agi::Exception const& error) {
		std::cerr << error.GetMessage() << '\n';
	}
	catch (std::exception const& error) {
		std::cerr << error.what() << '\n';
	}
	return 1;
}
