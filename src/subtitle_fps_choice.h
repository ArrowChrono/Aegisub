#pragma once

#include "ui_services.h"

#include <libaegisub/vfr.h>

#include <string>
#include <vector>

struct SubtitleFpsChoiceModel {
	std::vector<std::string> choices;
	bool includes_video_choice = false;
	bool show_smpte = false;
};

agi::SingleChoiceInteractionRequest BuildSubtitleFpsChoiceRequest(SubtitleFpsChoiceModel const& model);
SubtitleFpsChoiceModel BuildSubtitleFpsChoiceModel(bool allow_vfr, bool show_smpte, agi::vfr::Framerate const& fps);
agi::vfr::Framerate ResolveSubtitleFpsChoiceSelection(SubtitleFpsChoiceModel const& model, int selection, agi::vfr::Framerate const& fps);

std::string FormatVideoFpsDescription(agi::vfr::Framerate const& fps);
