// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "libaegisub/audio/provider.h"

#include "libaegisub/make_unique.h"

#include <array>
#include <boost/container/stable_vector.hpp>
#include <mutex>
#include <thread>

namespace {
using namespace agi;

#define CacheBits 22
#define CacheBlockSize (1 << CacheBits)

std::string FormatWrappedProviderName(char const* wrapper_name, AudioProvider const* source) {
	std::string name = wrapper_name;
	if (!source)
		return name;

	auto const source_name = source->GetMemoryStats().provider_name;
	if (source_name.empty())
		return name;

	name += " (";
	name += source_name;
	name += ")";
	return name;
}

class RAMAudioProvider final : public AudioProviderWrapper {
	const int64_t samples_per_block;
	/* clang-cl rejects the array-type pseudo-destructor expression in
	** boost::container::stable_vector's node destructor. */
#if defined(_MSC_VER) && !defined(__clang__)
	mutable boost::container::stable_vector<char[CacheBlockSize]> blockcache;
#else
	mutable boost::container::stable_vector<std::array<char, CacheBlockSize>> blockcache;
#endif
	std::unique_ptr<std::atomic<bool>[]> failed_blocks;
	mutable std::mutex source_mutex;
	std::atomic<bool> cancelled = {false};
	std::thread decoder;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override;

public:
	RAMAudioProvider(std::unique_ptr<AudioProvider> src)
	: AudioProviderWrapper(std::move(src))
	, samples_per_block(CacheBlockSize / bytes_per_sample / channels)
	{
		decoded_samples = 0;

		try {
			blockcache.resize(static_cast<size_t>(num_samples / samples_per_block + (num_samples % samples_per_block != 0)));
			failed_blocks = std::make_unique<std::atomic<bool>[]>(blockcache.size());
		}
		catch (std::bad_alloc const&) {
			throw AudioProviderError("Not enough memory available to cache in RAM");
		}

		decoder = std::thread([&] {
			for (size_t i = 0; i < blockcache.size(); i++) {
				if (cancelled) break;
				auto const start = static_cast<int64_t>(i) * samples_per_block;
				auto const actual_read = std::min(samples_per_block, num_samples - start);
				{
					std::scoped_lock lock(source_mutex);
					try {
						source->GetAudioChecked(&blockcache[i][0], start, actual_read);
					}
					catch (...) {
						failed_blocks[i].store(true, std::memory_order_release);
					}
				}
				// Publish the processed frontier even on failure. Reads of that
				// block must recover it instead of treating partial data as silence.
				decoded_samples += actual_read;
			}
		});
	}

	~RAMAudioProvider() {
		cancelled = true;
		decoder.join();
	}

	AudioProviderMemoryStats GetMemoryStats() const override {
		return BuildMemoryStats(
			FormatWrappedProviderName("RAM", source.get()),
			"memory",
			blockcache.size() * static_cast<size_t>(CacheBlockSize));
	}
};

void RAMAudioProvider::FillBuffer(void *buf, int64_t start, int64_t count) const {
	auto charbuf = static_cast<char *>(buf);
	for (int64_t bytes_remaining = count * bytes_per_sample * channels; bytes_remaining; ) {
		if (start >= decoded_samples)
			throw AudioDecodeError("RAM audio cache has not decoded the requested samples yet");

		const size_t i = start / samples_per_block;
		const int start_offset = (start % samples_per_block) * bytes_per_sample * channels;
		const int read_size = std::min<int>(bytes_remaining, samples_per_block * bytes_per_sample * channels - start_offset);
		if (failed_blocks[i].load(std::memory_order_acquire)) {
			std::scoped_lock lock(source_mutex);
			if (failed_blocks[i].load(std::memory_order_relaxed)) {
				auto const block_start = static_cast<int64_t>(i) * samples_per_block;
				auto const block_samples = std::min(samples_per_block, num_samples - block_start);
				// A failed read may have written any prefix of the block. Replace
				// all of it, and leave it failed if this single recovery read throws.
				source->GetAudioChecked(&blockcache[i][0], block_start, block_samples);
				failed_blocks[i].store(false, std::memory_order_release);
			}
		}

		memcpy(charbuf, &blockcache[i][start_offset], read_size);
		charbuf += read_size;
		bytes_remaining -= read_size;
		start += read_size / bytes_per_sample / channels;
	}
}
}

namespace agi {
std::unique_ptr<AudioProvider> CreateRAMAudioProvider(std::unique_ptr<AudioProvider> src) {
	return agi::make_unique<RAMAudioProvider>(std::move(src));
}
}
