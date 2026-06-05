// Copyright (c) 2006-2007, Rodrigo Braz Monteiro, Evgeniy Stepanov
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

/// @file subtitles_provider_libass.cpp
/// @brief libass-based subtitle renderer
/// @ingroup subtitle_rendering
///

#include "subtitles_provider_libass.h"

#include "include/aegisub/subtitles_provider.h"
#include "libass_runtime.h"
#include "ready_flag.h"
#include "subtitle_overlay_blend.h"
#include "transient_font_set.h"
#include "ui_dispatch.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include "translation_service.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <mutex>

namespace {
std::unique_ptr<agi::dispatch::Queue> cache_queue;
std::once_flag cache_queue_once;
std::once_flag cache_warmup_once;

void msg_callback(int level, const char *fmt, va_list args, void *) {
	if (level >= 7) return;
	char buf[1024];
#ifdef _WIN32
	vsprintf_s(buf, sizeof(buf), fmt, args);
#else
	vsnprintf(buf, sizeof(buf), fmt, args);
#endif

	if (level < 2) // warning/error
		LOG_I("subtitle/provider/libass") << buf;
	else // verbose
		LOG_D("subtitle/provider/libass") << buf;
}

agi::dispatch::Queue& GetCacheQueue() {
	std::call_once(cache_queue_once, [] {
		cache_queue = agi::dispatch::Create();
	});
	return *cache_queue;
}

void LogTransientLibassFontsDebug(char const* action, std::shared_ptr<const TransientFontSet> const& fonts) {
	if (!fonts || fonts->empty())
		return;

	for (size_t i = 0; i < fonts->fonts.size(); ++i) {
		auto const& font = fonts->fonts[i];
		LOG_D("subtitle/provider/libass") << action << ": " << font.original_name
			<< agi::format(" (%u/%u, %u bytes, generation %u)",
				static_cast<unsigned>(i + 1),
				static_cast<unsigned>(fonts->fonts.size()),
				static_cast<unsigned>(font.bytes.size()),
				static_cast<unsigned>(fonts->generation));
	}
}

void ConfigureRenderer(libass::runtime::Api const& api, ASS_Renderer *renderer) {
	api.ass_set_font_scale(renderer, 1.);
	api.ass_set_fonts(renderer, nullptr, "Sans", 1, nullptr, true);
}

struct cache_thread_shared {
	ASS_Library *library = nullptr;
	ASS_Renderer *renderer = nullptr;
	std::string error;
	std::mutex mutex;
	ReadyFlag ready;
	~cache_thread_shared() {
		// If construction failed before EnsureLoaded() completed, there are no
		// libass objects to free and GetApi() would rethrow from a destructor.
		if (!renderer && !library)
			return;

		// Runtime stays loaded for process lifetime; only destroy libass objects.
		auto const& api = libass::runtime::GetApi();
		if (renderer) api.ass_renderer_done(renderer);
		if (library) api.ass_library_done(library);
	}
};

class LibassSubtitlesProvider final : public SubtitlesProvider {
	agi::BackgroundRunner *br;
	std::shared_ptr<cache_thread_shared> shared;
	std::shared_ptr<const TransientFontSet> transient_fonts;
	ASS_Track* ass_track = nullptr;

	void WaitUntilReady() const {
		if (!shared->ready.IsReady()) {
			auto wait_for_ready = [&] {
				if (shared->ready.WaitFor(std::chrono::milliseconds(250)))
					return;

				if (!br) {
					shared->ready.Wait();
					return;
				}

				br->Run([=](agi::ProgressSink *ps) {
					ps->SetTitle(_("Updating font index"));
					ps->SetMessage(_("This may take several minutes"));
					ps->SetIndeterminate();
					shared->ready.Wait();
				});
			};

			agi::ui::MainInvoke(wait_for_ready);
		}

		std::lock_guard<std::mutex> lock(shared->mutex);
		if (!shared->library || !shared->renderer)
			throw agi::InternalError(shared->error.empty() ? "libass failed to initialize." : shared->error);
	}

	ASS_Library *library() {
		WaitUntilReady();

		std::lock_guard<std::mutex> lock(shared->mutex);
		return shared->library;
	}

	ASS_Renderer *renderer() {
		WaitUntilReady();

		std::lock_guard<std::mutex> lock(shared->mutex);
		return shared->renderer;
	}

public:
	LibassSubtitlesProvider(SubtitleRenderEnvironment const& env);
	~LibassSubtitlesProvider();

	std::string GetDebugName() const override { return "libass"; }
	void LoadSubtitles(const char *data, size_t len) override {
		auto const& api = libass::runtime::GetApi();
		auto *ass_library = library();
		if (ass_track) api.ass_free_track(ass_track);
		ass_track = api.ass_read_memory(ass_library, const_cast<char *>(data), len, nullptr);
		if (!ass_track) throw agi::InternalError("libass failed to load subtitles.");
	}

	SubtitleRenderMode GetRenderMode() const override { return SubtitleRenderMode::PremultipliedOverlay; }
	bool RenderOverlayClearsTarget() const override { return true; }
	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double time) override;
	void DrawSubtitles(VideoFrame &dst, double time) override;

	void Reinitialize() override {
		// No need to reinit if we're not even done with the initial init
		if (!shared->ready.IsReady())
			return;

		std::lock_guard<std::mutex> lock(shared->mutex);
		if (!shared->library || !shared->renderer)
			return;

		auto const& api = libass::runtime::GetApi();
		auto *new_renderer = api.ass_renderer_init(shared->library);
		if (!new_renderer) {
			LOG_E("subtitle/provider/libass/init") << "Failed to reinitialize libass renderer.";
			return;
		}

		ConfigureRenderer(api, new_renderer);
		api.ass_renderer_done(shared->renderer);
		shared->renderer = new_renderer;
	}
};

LibassSubtitlesProvider::LibassSubtitlesProvider(SubtitleRenderEnvironment const& env)
: br(env.background_runner)
, transient_fonts(env.transient_fonts)
, shared(std::make_shared<cache_thread_shared>())
{
	// Synchronously resolve the runtime before returning so factory selection can
	// fall back to CSRI when libass is missing or incomplete.
	libass::runtime::EnsureLoaded();

	auto state = shared;
	auto fonts = transient_fonts;
	GetCacheQueue().Async([state, fonts] {
		auto const& api = libass::runtime::GetApi();
		ASS_Library *library = nullptr;
		ASS_Renderer *renderer = nullptr;
		std::string error;

		library = api.ass_library_init();
		if (!library)
			error = "libass failed to initialize.";
		else {
			api.ass_set_message_cb(library, msg_callback, nullptr);
			api.ass_set_extract_fonts(library, 0);
			if (fonts && !fonts->empty()) {
				size_t loaded = 0;
				for (size_t i = 0; i < fonts->fonts.size(); ++i) {
					auto const& font = fonts->fonts[i];
					if (font.bytes.empty() || font.bytes.size() > INT_MAX)
						continue;
					api.ass_add_font(library, font.original_name.c_str(), font.bytes.data(), static_cast<int>(font.bytes.size()));
					LOG_D("subtitle/provider/libass") << "Registered transient libass font: " << font.original_name
						<< agi::format(" (%u/%u, %u bytes, generation %u)",
							static_cast<unsigned>(i + 1),
							static_cast<unsigned>(fonts->fonts.size()),
							static_cast<unsigned>(font.bytes.size()),
							static_cast<unsigned>(fonts->generation));
					++loaded;
				}
				LOG_I("subtitle/provider/libass") << "Registered " << loaded << " transient font(s) with libass"
					<< (fonts->generation ? agi::format(" (generation %u)", static_cast<unsigned>(fonts->generation)) : "");
			}
			renderer = api.ass_renderer_init(library);
			if (!renderer) {
				error = "libass failed to initialize the renderer.";
				api.ass_library_done(library);
				library = nullptr;
			}
			else {
				ConfigureRenderer(api, renderer);
			}
		}

		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->library = library;
			state->renderer = renderer;
			state->error = std::move(error);
		}
		state->ready.Signal();
	});
}

LibassSubtitlesProvider::~LibassSubtitlesProvider() {
	if (ass_track) {
		auto const& api = libass::runtime::GetApi();
		api.ass_free_track(ass_track);
	}
	if (transient_fonts && !transient_fonts->empty()) {
		LOG_I("subtitle/provider/libass") << "Releasing transient font registration from libass"
			<< agi::format(" (%u font(s), generation %u)",
				static_cast<unsigned>(transient_fonts->fonts.size()),
				static_cast<unsigned>(transient_fonts->generation));
		LogTransientLibassFontsDebug("Released transient libass font", transient_fonts);
	}
}

bool LibassSubtitlesProvider::RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double time) {
	if (!overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return false;

	auto const& api = libass::runtime::GetApi();
	auto *ass_renderer = renderer();

	int render_width = overlay.width;
	int render_height = overlay.height;
	if (source.IsValid()) {
		render_width = source.width;
		render_height = source.height;
	}

	api.ass_set_frame_size(ass_renderer, render_width, render_height);
	api.ass_set_storage_size(ass_renderer, render_width, render_height);

	// Avoid losing a millisecond at subtitle boundaries: https://github.com/arch1t3cht/Aegisub/issues/205
	ASS_Image *img = api.ass_render_frame(
		ass_renderer, ass_track, static_cast<long long>(std::floor((time * 1000) + 1e-6)), nullptr);
	BgraSubtitleTargetView target {
		overlay.planes[0].data,
		overlay.planes[0].stride,
		overlay.width,
		overlay.height,
		overlay.flipped
	};

	auto blend_mode = overlay.premultiplied_alpha
		? SubtitleOverlayBlendMode::PremultipliedOverlay
		: SubtitleOverlayBlendMode::LegacyBakeIn;
	overlay.composition_mode = overlay.premultiplied_alpha
		? SubtitleOverlayCompositionMode::PremultipliedAlpha
		: SubtitleOverlayCompositionMode::Unsupported;
	overlay.dirty_rects = nullptr;
	overlay.dirty_rect_count = 0;
	overlay.has_visible_content = false;
	if (overlay.premultiplied_alpha)
		ClearBgraSubtitleTarget(target);

	for (; img; img = img->next) {
		overlay.has_visible_content = true;
		BlendLibassMaskIntoBgraTarget(
			target,
			blend_mode,
			img->dst_x,
			img->dst_y,
			img->w,
			img->h,
			img->bitmap,
			img->stride,
			static_cast<std::uint32_t>(img->color));
	}

	return true;
}

void LibassSubtitlesProvider::DrawSubtitles(VideoFrame &frame,double time) {
	auto source = MakeSourceFrameView(frame);
	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	RenderOverlay(source, overlay, time);
}
}

namespace libass {
std::unique_ptr<SubtitlesProvider> Create(std::string const&, SubtitleRenderEnvironment const& env) {
	return agi::make_unique<LibassSubtitlesProvider>(env);
}

bool IsAvailable() noexcept {
	return libass::runtime::IsAvailable();
}

std::string GetAvailabilityError() {
	auto err = libass::runtime::GetLoadError();
	return err.empty() ? "runtime library is unavailable." : err;
}

void CacheFonts() {
	if (!libass::runtime::IsAvailable()) {
		static std::once_flag missing_runtime_log_once;
		std::call_once(missing_runtime_log_once, [] {
			auto err = libass::runtime::GetLoadError();
			if (err.empty())
				err = "runtime library is unavailable.";
			LOG_W("subtitle/provider/libass/warmup") << "Skipping libass font cache warmup: " << err;
		});
		return;
	}

	std::call_once(cache_warmup_once, [] {
		GetCacheQueue().Async([] {
			auto const& api = libass::runtime::GetApi();
			auto *library = api.ass_library_init();
			if (!library) {
				LOG_E("subtitle/provider/libass/warmup") << "Failed to initialize libass for warmup.";
				return;
			}

			api.ass_set_message_cb(library, msg_callback, nullptr);

			auto *renderer = api.ass_renderer_init(library);
			if (!renderer) {
				LOG_E("subtitle/provider/libass/warmup") << "Failed to initialize libass renderer for warmup.";
				api.ass_library_done(library);
				return;
			}

			ConfigureRenderer(api, renderer);
			api.ass_renderer_done(renderer);
			api.ass_library_done(library);
		});
	});
}
}
