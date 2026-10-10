#pragma once

#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

namespace aegisub {
struct TimingProcessorPreferences {
	int lead_in_ms = 0;
	int lead_out_ms = 0;
	int key_start_before_ms = 0;
	int key_start_after_ms = 0;
	int key_end_before_ms = 0;
	int key_end_after_ms = 0;
	int adjacent_gap_ms = 0;
	int adjacent_overlap_ms = 0;
	double adjacent_bias = 0;
	bool lead_in_enabled = false;
	bool lead_out_enabled = false;
	bool keyframes_enabled = false;
	bool adjacent_enabled = false;
	bool only_selection = false;
};

[[nodiscard]] inline TimingProcessorPreferences LoadTimingProcessorPreferences(agi::Options& options) {
	return {
		.lead_in_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Lead/IN")->GetInt()),
		.lead_out_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Lead/OUT")->GetInt()),
		.key_start_before_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Key Start Before")->GetInt()),
		.key_start_after_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Key Start After")->GetInt()),
		.key_end_before_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Key End Before")->GetInt()),
		.key_end_after_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Key End After")->GetInt()),
		.adjacent_gap_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Adjacent Gap")->GetInt()),
		.adjacent_overlap_ms = static_cast<int>(options.Get("Tool/Timing Post Processor/Threshold/Adjacent Overlap")->GetInt()),
		.adjacent_bias = options.Get("Tool/Timing Post Processor/Adjacent Bias")->GetDouble(),
		.lead_in_enabled = options.Get("Tool/Timing Post Processor/Enable/Lead/IN")->GetBool(),
		.lead_out_enabled = options.Get("Tool/Timing Post Processor/Enable/Lead/OUT")->GetBool(),
		.keyframes_enabled = options.Get("Tool/Timing Post Processor/Enable/Keyframe")->GetBool(),
		.adjacent_enabled = options.Get("Tool/Timing Post Processor/Enable/Adjacent")->GetBool(),
		.only_selection = options.Get("Tool/Timing Post Processor/Only Selection")->GetBool(),
	};
}

inline void SaveTimingProcessorPreferences(agi::Options& options, TimingProcessorPreferences const& value, bool update_keyframe_preference) {
	options.Get("Tool/Timing Post Processor/Lead/IN")->SetInt(value.lead_in_ms);
	options.Get("Tool/Timing Post Processor/Lead/OUT")->SetInt(value.lead_out_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Key Start Before")->SetInt(value.key_start_before_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Key Start After")->SetInt(value.key_start_after_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Key End Before")->SetInt(value.key_end_before_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Key End After")->SetInt(value.key_end_after_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Adjacent Gap")->SetInt(value.adjacent_gap_ms);
	options.Get("Tool/Timing Post Processor/Threshold/Adjacent Overlap")->SetInt(value.adjacent_overlap_ms);
	options.Get("Tool/Timing Post Processor/Adjacent Bias")->SetDouble(value.adjacent_bias);
	options.Get("Tool/Timing Post Processor/Enable/Lead/IN")->SetBool(value.lead_in_enabled);
	options.Get("Tool/Timing Post Processor/Enable/Lead/OUT")->SetBool(value.lead_out_enabled);
	if (update_keyframe_preference)
		options.Get("Tool/Timing Post Processor/Enable/Keyframe")->SetBool(value.keyframes_enabled);
	options.Get("Tool/Timing Post Processor/Enable/Adjacent")->SetBool(value.adjacent_enabled);
	options.Get("Tool/Timing Post Processor/Only Selection")->SetBool(value.only_selection);
}
}
