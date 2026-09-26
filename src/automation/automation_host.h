// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_context_snapshot.h"
#include "automation_visual_guide_snapshot.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agi {
	class FileDialogService;
}

namespace Automation4 {
	class BackgroundScriptRunner;
	class AutomationMutationJournal;
	class ProgressSink;
	class ScriptDialog;
	class AutomationInvocationObserver;
	struct AutomationInvocation;
	struct AutomationOpenFileDialogRequest;
	struct AutomationSaveFileDialogRequest;

	struct AutomationVideoInfo {
		int width = 0;
		int height = 0;
		double aspect_ratio = 0.0;
		int aspect_ratio_type = 0;
	};

	struct AutomationAudioSelection {
		int begin = 0;
		int end = 0;
	};

	struct AutomationSubtitleEditBoxCursor {
		int start = 0;
		int stop = 0;
	};

	struct AutomationUiAnchor {
		// Opaque UI handle passed through the GUI seam to avoid leaking wx types
		// into engine-agnostic automation contracts.
		void *native_parent = nullptr;
	};

	struct AutomationProjectPropertiesView {
		std::string automation_scripts;
		std::string export_filters;
		std::string export_encoding;
		std::string style_storage;
		double video_zoom = 0.0;
		double ar_value = 0.0;
		int scroll_position = 0;
		int active_row = 0;
		int ar_mode = 0;
		int video_position = 0;
		agi::fs::path audio_file;
		agi::fs::path video_file;
		agi::fs::path timecodes_file;
		agi::fs::path keyframes_file;
	};

	class AutomationMediaState {
	public:
		virtual ~AutomationMediaState() = default;
		virtual AutomationMediaSnapshot CaptureSnapshot() const = 0;
		virtual std::optional<int> FrameFromMs(int ms) const = 0;
		virtual std::optional<int> MsFromFrame(int frame) const = 0;
		virtual std::optional<AutomationVideoInfo> TryGetVideoInfo() const = 0;
		virtual std::vector<int> GetKeyframes() const = 0;
		virtual std::optional<AutomationAudioSelection> TryGetAudioSelection() const = 0;
	};

	class AutomationUiProxy {
	public:
		virtual ~AutomationUiProxy() = default;
		[[nodiscard]] virtual std::shared_ptr<AutomationInvocationObserver> BeginInvocationObservation(AutomationInvocation const&) const { return {}; }
		virtual void ShowStatus(std::string const& message, int timeout_ms = 10000) = 0;
		virtual bool SupportsInteractiveDialogs() const = 0;
		virtual std::unique_ptr<BackgroundScriptRunner> CreateBackgroundScriptRunner(std::string const& title, AutomationUiAnchor anchor = {}) const = 0;
		virtual void ShowDialog(ProgressSink& sink, ScriptDialog& dialog) = 0;
		virtual std::vector<agi::fs::path> RequestOpenFiles(ProgressSink& sink, AutomationOpenFileDialogRequest const& request) = 0;
		virtual agi::fs::path RequestSaveFile(ProgressSink& sink, AutomationSaveFileDialogRequest const& request) = 0;
		virtual std::shared_ptr<agi::FileDialogService> GetFileDialogService() const = 0;
		virtual bool CanFocusSubtitleEditBox() const = 0;
		virtual std::optional<AutomationSubtitleEditBoxCursor> TryGetSubtitleEditBoxCursor() const = 0;
		// Captures temporary visual-guide UI state as a value object. A missing
		// value means that this host has no live GUI/controller/video snapshot.
		virtual std::optional<AutomationVisualGuideSnapshot> TryGetVisualGuides() const = 0;
		virtual bool ScrollAudioToTime(int time_ms) = 0;
		virtual bool FocusSubtitleEditBox() = 0;
		virtual bool SetSubtitleEditBoxCursor(int character_index, bool after) = 0;
		virtual bool SetSubtitleEditBoxSelection(int start, int stop) = 0;
	};

	class AutomationHost {
	public:
		virtual ~AutomationHost() = default;
		virtual bool HasProjectContext() const = 0;
		virtual void const* ProjectContextIdentity() const = 0;
		virtual AutomationProjectSnapshot CaptureProjectSnapshot() const = 0;
		virtual AutomationMediaState const& Media() const = 0;
		virtual AutomationUiProxy& Ui() = 0;
		virtual AutomationUiProxy const& Ui() const = 0;
		virtual std::optional<agi::fs::path> TryGetFileName() const = 0;
		virtual agi::fs::path DecodePath(std::string const& path) const = 0;
		virtual std::optional<AutomationProjectPropertiesView> TryGetProjectProperties() const = 0;
		virtual std::shared_ptr<AutomationMutationJournal> GetMutationJournal() const = 0;
	};
}
