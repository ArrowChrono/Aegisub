// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "visual_feature.h"
#include "visual_tool.h"
#include "visual_tool_vector_clip_shared.h"

class wxToolBar;

/// @class VisualToolVectorClipDraggableFeature
/// @brief VisualDraggableFeature with information about a feature's location
///        in the spline
struct VisualToolVectorClipDraggableFeature final : public VisualDraggableFeature {
	/// Which curve in the spline this feature is a point on
	size_t idx = 0;
	/// 0-3; indicates which part of the curve this point is
	int point = 0;
};

class VisualToolVectorClip final : public VisualTool<VisualToolVectorClipDraggableFeature> {
public:
	/// Number of sub-modes (BUTTON_LAST - BUTTON_DRAG).
static constexpr int kSubModeCount = aegisub::visual_vector_clip::kModeCount;

private:
	Spline spline; /// The current spline
	std::vector<aegisub::visual_vector_clip::Feature> overlay_features;
	wxToolBar *toolBar = nullptr; /// The subtoolbar
	int mode = 0; /// 0-8
	bool inverse = false; /// is iclip?
	std::string last_committed_clip;

	std::set<Feature *> box_added;

	void RetireDerivedSubtitleRowBorrows() noexcept override {
		VisualTool<VisualToolVectorClipDraggableFeature>::RetireDerivedSubtitleRowBorrows();
		box_added.clear();
	}

	/// @brief Set the mode
	/// @param mode 0-8
	void SetMode(int mode);
	void OnSubTool(wxCommandEvent &event);

	std::string BuildClipValue() const;
	bool Save(std::string const& value);
	void Commit(wxString message=wxString()) override;

	void MakeFeature(size_t idx);
	void MakeFeatures();

	bool InitializeHold() override;
	void UpdateHold() override;

	void UpdateDrag(Feature *feature) override;
	bool InitializeDrag(Feature *feature) override;

	void DoRefresh() override;
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext &context) override;

public:
	VisualToolVectorClip(VideoDisplay *parent, agi::Context *context);
	void SetToolbar(wxToolBar *tb) override;
	bool SetSubMode(int mode) override;
	int GetSubMode() const override { return mode; }
	std::string GetHotkeyContext() const override { return "Visual Vector Clip"; }
};
