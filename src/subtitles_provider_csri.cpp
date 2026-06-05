// Copyright (c) 2007, Rodrigo Braz Monteiro
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

/// @file subtitles_provider_csri.cpp
/// @brief Wrapper for CSRI-based subtitle renderers
/// @ingroup subtitle_rendering
///

#ifdef WITH_CSRI
#include "subtitles_provider_csri.h"

#include "include/aegisub/subtitles_provider.h"
#include "subtitle_format_ass.h"
#include "transient_font_set.h"
#include "video_frame.h"

#include <libaegisub/format.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>

#ifdef WIN32
#define CSRIAPI
#include <windows.h>
#endif

#include <csri/csri.h>

namespace {
// CSRI renderers are not required to be thread safe (and VSFilter very much
// is not)
std::mutex csri_mutex;

struct closer {
	void operator()(csri_inst *inst) { if (inst) csri_close(inst); }
};

#ifdef _WIN32
struct LoadedCsriFont {
	HANDLE handle = nullptr;
	std::string original_name;
	DWORD face_count = 0;
};

struct LoadedCsriFontSet {
	std::shared_ptr<const TransientFontSet> fonts;
	std::vector<LoadedCsriFont> loaded_fonts;
	size_t ref_count = 0;
};

struct CsriTransientFontRegistry {
	std::unordered_map<uint64_t, LoadedCsriFontSet> active_generations;
	std::unordered_map<void const*, uint64_t> active_leases;

	void LogFontsDebug(char const* action, std::shared_ptr<const TransientFontSet> const& fonts) {
		if (!fonts || fonts->empty())
			return;

		for (size_t i = 0; i < fonts->fonts.size(); ++i) {
			auto const& font = fonts->fonts[i];
			LOG_D("subtitle/provider/csri") << action << ": " << font.original_name
				<< agi::format(" (%u/%u, %u bytes, generation %u)",
					static_cast<unsigned>(i + 1),
					static_cast<unsigned>(fonts->fonts.size()),
					static_cast<unsigned>(font.bytes.size()),
					static_cast<unsigned>(fonts->generation));
		}
	}

	void UnloadGenerationLocked(uint64_t generation, LoadedCsriFontSet& entry) {
		if (entry.loaded_fonts.empty() && !entry.fonts)
			return;

		size_t removed = 0;
		for (auto const& font : entry.loaded_fonts) {
			if (!font.handle)
				continue;

			LOG_D("subtitle/provider/csri") << "Removing transient CSRI font resource: " << font.original_name
				<< agi::format(" (%u face(s), generation %u)",
					static_cast<unsigned>(font.face_count),
					static_cast<unsigned>(generation));

			if (RemoveFontMemResourceEx(font.handle))
				++removed;
			else
				LOG_W("subtitle/provider/csri") << "Failed to remove transient CSRI font resource for "
					<< font.original_name << ", last_error=" << static_cast<unsigned long>(GetLastError());
		}

		LOG_I("subtitle/provider/csri") << "Cleared transient CSRI font registry"
			<< agi::format(" (%u font resource(s), generation %u)",
				static_cast<unsigned>(removed),
				static_cast<unsigned>(generation));
	}

	void ReleaseLocked(void const* owner) {
		auto lease = active_leases.find(owner);
		if (lease == active_leases.end())
			return;

		auto generation = lease->second;
		active_leases.erase(lease);

		auto loaded_generation = active_generations.find(generation);
		if (loaded_generation == active_generations.end())
			return;

		if (loaded_generation->second.ref_count > 1) {
			--loaded_generation->second.ref_count;
			LOG_I("subtitle/provider/csri") << "Released transient CSRI font lease"
				<< agi::format(" (generation %u, remaining lease(s) %u)",
					static_cast<unsigned>(generation),
					static_cast<unsigned>(loaded_generation->second.ref_count));
			return;
		}

		UnloadGenerationLocked(generation, loaded_generation->second);
		active_generations.erase(loaded_generation);
	}

	void AcquireLocked(void const* owner, std::shared_ptr<const TransientFontSet> fonts) {
		auto const generation = fonts && !fonts->empty() ? fonts->generation : 0;
		auto existing = active_leases.find(owner);
		if (existing != active_leases.end() && existing->second == generation)
			return;

		if (existing != active_leases.end())
			ReleaseLocked(owner);

		if (!fonts || fonts->empty())
			return;

		auto loaded_generation = active_generations.find(generation);
		if (loaded_generation != active_generations.end()) {
			++loaded_generation->second.ref_count;
			active_leases[owner] = generation;
			LOG_I("subtitle/provider/csri") << "Reusing transient CSRI font registry"
				<< agi::format(" (%u font resource(s), generation %u, lease(s) %u)",
					static_cast<unsigned>(loaded_generation->second.loaded_fonts.size()),
					static_cast<unsigned>(generation),
					static_cast<unsigned>(loaded_generation->second.ref_count));
			LogFontsDebug("Reused transient CSRI font", loaded_generation->second.fonts);
			return;
		}

		LoadedCsriFontSet entry;
		entry.fonts = std::move(fonts);
		entry.ref_count = 1;
		size_t loaded_count = 0;
		for (auto const& font : entry.fonts->fonts) {
			if (font.bytes.empty() || font.bytes.size() > std::numeric_limits<DWORD>::max()) {
				LOG_W("subtitle/provider/csri") << "Skipping transient CSRI font resource with invalid size: "
					<< font.original_name;
				continue;
			}

			DWORD face_count = 0;
			auto handle = AddFontMemResourceEx(
				const_cast<char *>(font.bytes.data()),
				static_cast<DWORD>(font.bytes.size()),
				nullptr,
				&face_count);
			if (!handle) {
				LOG_W("subtitle/provider/csri") << "Failed to load transient CSRI font resource: "
					<< font.original_name << ", last_error=" << static_cast<unsigned long>(GetLastError());
				continue;
			}

			entry.loaded_fonts.push_back({ handle, font.original_name, face_count });
			LOG_D("subtitle/provider/csri") << "Loaded transient CSRI font resource: " << font.original_name
				<< agi::format(" (%u/%u, %u bytes, %u face(s), generation %u)",
					static_cast<unsigned>(loaded_count + 1),
					static_cast<unsigned>(entry.fonts->fonts.size()),
					static_cast<unsigned>(font.bytes.size()),
					static_cast<unsigned>(face_count),
					static_cast<unsigned>(generation));
			++loaded_count;
		}

		LOG_I("subtitle/provider/csri") << "Loaded transient CSRI font registry"
			<< agi::format(" (%u/%u font resource(s), generation %u)",
				static_cast<unsigned>(loaded_count),
				static_cast<unsigned>(entry.fonts->fonts.size()),
				static_cast<unsigned>(generation));
		LogFontsDebug("Transient CSRI font set", entry.fonts);

		active_leases[owner] = generation;
		active_generations.emplace(generation, std::move(entry));
	}
};

CsriTransientFontRegistry& GetTransientFontRegistry() {
	static CsriTransientFontRegistry registry;
	return registry;
}
#endif

bool RenderCsriBgraOverlay(csri_inst *instance, SubtitleOverlay& overlay, double time) {
	if (!instance || !overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return false;

	csri_frame frame = { };
	auto *data = overlay.planes[0].data;
	auto stride = overlay.planes[0].stride;
	if (!data || stride == 0)
		return false;

	if (overlay.flipped && stride > 0) {
		data += (overlay.height - 1) * stride;
		stride = -stride;
	}

	frame.planes[0] = data;
	frame.strides[0] = stride;
	frame.pixfmt = CSRI_F_BGR_;

	csri_fmt format = {
		frame.pixfmt,
		static_cast<unsigned>(overlay.width),
		static_cast<unsigned>(overlay.height)
	};

	std::lock_guard<std::mutex> lock(csri_mutex);
	if (!csri_request_fmt(instance, &format)) {
		// Avoid losing a millisecond at subtitle boundaries: https://github.com/arch1t3cht/Aegisub/issues/205
		csri_render(instance, &frame, time + 1e-9);
	}
	return true;
}

class CSRISubtitlesProvider final : public SubtitlesProvider {
	std::unique_ptr<csri_inst, closer> instance;
	csri_rend *renderer = nullptr;
	std::shared_ptr<const TransientFontSet> transient_fonts;
	std::string provider_name;

	void LoadSubtitles(const char *data, size_t len) override {
		std::lock_guard<std::mutex> lock(csri_mutex);
		instance.reset(csri_open_mem(renderer, data, len, nullptr));
	}

public:
	CSRISubtitlesProvider(std::string subType, std::shared_ptr<const TransientFontSet> transient_fonts);
	~CSRISubtitlesProvider();
	void OnActivated() override;

	std::string GetDebugName() const override { return "CSRI/" + provider_name; }
	SubtitleRenderMode GetRenderMode() const override { return SubtitleRenderMode::CompatibilityFrameOnly; }
	void DrawSubtitles(VideoFrame &dst, double time) override;
};

CSRISubtitlesProvider::CSRISubtitlesProvider(std::string type, std::shared_ptr<const TransientFontSet> transient_fonts)
: transient_fonts(std::move(transient_fonts))
, provider_name(std::move(type)) {
	std::lock_guard<std::mutex> lock(csri_mutex);
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		if (provider_name == csri_renderer_info(cur)->name) {
			renderer = cur;
			break;
		}
	}

	if (!renderer)
		throw agi::InternalError("CSRI renderer vanished between initial list and creation?");
}

CSRISubtitlesProvider::~CSRISubtitlesProvider() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(csri_mutex);
	GetTransientFontRegistry().ReleaseLocked(this);
#endif
}

void CSRISubtitlesProvider::OnActivated() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(csri_mutex);
	GetTransientFontRegistry().AcquireLocked(this, transient_fonts);
#else
	(void)transient_fonts;
#endif
}

void CSRISubtitlesProvider::DrawSubtitles(VideoFrame &dst, double time) {
	auto overlay = MakeLegacyBgraSubtitleOverlayView(dst);
	RenderCsriBgraOverlay(instance.get(), overlay, time);
}
}

namespace csri {
std::vector<std::string> List() {
	std::vector<std::string> final;
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		std::string name(csri_renderer_info(cur)->name);
		if (name.find("aegisub") != name.npos)
			final.insert(final.begin(), name);
		else
			final.push_back(name);
	}
	return final;
}

std::unique_ptr<SubtitlesProvider> Create(std::string const& name, SubtitleRenderEnvironment const& env) {
	return agi::make_unique<CSRISubtitlesProvider>(name, env.transient_fonts);
}
}
#endif // WITH_CSRI
