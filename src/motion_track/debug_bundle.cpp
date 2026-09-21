#include "debug_bundle.h"

#include "session.h"
#include "../ass_dialogue.h"
#include "../ass_info.h"
#include "../ass_style.h"
#include "../ass_style_resolution.h"
#include "../ass_tag_scanner.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace aegisub::motion_track {
namespace {

constexpr int schema_version = 1;

json::UnknownElement Number(double value) {
	if (std::isnan(value))
		return "NaN";
	if (std::isinf(value))
		return value < 0 ? "-Infinity" : "Infinity";
	return value;
}

double Number(json::UnknownElement const& value) {
	try {
		return static_cast<json::Double const&>(value);
	}
	catch (json::Exception const&) {
	}
	try {
		return static_cast<double>(static_cast<json::Integer const&>(value));
	}
	catch (json::Exception const&) {
	}
	auto const& text = static_cast<json::String const&>(value);
	if (text == "NaN")
		return std::numeric_limits<double>::quiet_NaN();
	if (text == "Infinity")
		return std::numeric_limits<double>::infinity();
	if (text == "-Infinity")
		return -std::numeric_limits<double>::infinity();
	throw std::runtime_error("Invalid diagnostic number");
}

int Integer(json::UnknownElement const& value) {
	auto const number = static_cast<json::Integer const&>(value);
	if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
		throw std::runtime_error("Diagnostic integer is out of range");
	return static_cast<int>(number);
}

json::Array const& Array(json::Object const& object, char const *key) {
	return static_cast<json::Array const&>(object.at(key));
}

json::Object const& Object(json::Object const& object, char const *key) {
	return static_cast<json::Object const&>(object.at(key));
}

json::Object Interval(FrameInterval interval) {
	json::Object value;
	value["first"] = interval.first;
	value["last"] = interval.last;
	return value;
}

FrameInterval Interval(json::Object const& object) {
	return {.first = Integer(object.at("first")), .last = Integer(object.at("last"))};
}

bool IsRenderingInfo(std::string const& key) {
	constexpr char const *names[]{"ScriptType", "PlayResX", "PlayResY", "LayoutResX",
								  "LayoutResY", "WrapStyle", "ScaledBorderAndShadow", "YCbCr Matrix",
								  "Collisions", "Timer", "Kerning"};
	return std::ranges::any_of(names, [&](auto name) {
		return agi::util::strings::iequals(key, name);
	});
}

struct StyleSelection {
	AssFile const& file;
	std::set<AssStyle const *> styles;
};

void CollectResetStyles(std::string_view body, StyleSelection& selection, int depth = 0) {
	if (depth >= 32)
		return;
	ass_tag_scanner::ScanRawTags(body, [&](auto const& raw) {
		AssOverrideTag tag("\\" + std::string(raw.name) +
						   (raw.has_paren ? "(" + std::string(raw.args) + ")" : ""));
		if (tag.Name == "\\r" && !tag.Params.empty()) {
			auto name = tag.Params[0].Get<std::string>(std::string{});
			// The simple and full-geometry planners have different reset resolvers.
			selection.styles.insert(ass_style_resolution::ResolveResetStyle(selection.file, name));
			selection.styles.insert(ass_style_resolution::ResolveEventStyle(selection.file, name));
		}
		if (raw.has_paren && ass_tag_scanner::NameHasPrefix(raw.name, "t"))
			CollectResetStyles(raw.args, selection, depth + 1);
	});
}

json::Object Style(AssStyle const& style) {
	auto copy = style;
	copy.UpdateData();
	json::Object value;
	value["entry"] = copy.GetEntryData();
	// ASS's three-decimal serialization is not lossless for live style values.
	value["name"] = style.name;
	value["font"] = style.font;
	value["fontsize"] = Number(style.fontsize);
	value["scalex"] = Number(style.scalex);
	value["scaley"] = Number(style.scaley);
	value["spacing"] = Number(style.spacing);
	value["angle"] = Number(style.angle);
	value["outline_w"] = Number(style.outline_w);
	value["shadow_w"] = Number(style.shadow_w);
	return value;
}

std::unique_ptr<AssStyle> Style(json::Object const& value) {
	auto style = std::make_unique<AssStyle>(static_cast<json::String const&>(value.at("entry")));
	style->name = static_cast<json::String const&>(value.at("name"));
	style->font = static_cast<json::String const&>(value.at("font"));
	style->fontsize = Number(value.at("fontsize"));
	style->scalex = Number(value.at("scalex"));
	style->scaley = Number(value.at("scaley"));
	style->spacing = Number(value.at("spacing"));
	style->angle = Number(value.at("angle"));
	style->outline_w = Number(value.at("outline_w"));
	style->shadow_w = Number(value.at("shadow_w"));
	return style;
}

json::Object Options(ApplyPlanOptions const& options) {
	json::Object value;
	value["mode"] = static_cast<int>(options.mode);
	value["compact_epsilon"] = Number(options.compact_epsilon);
	value["position_decimals"] = options.position_decimals;
	value["smooth_frames"] = options.smooth_frames;
	value["stabilize"] = options.stabilization.enable;
	value["position_floor_storage_px"] = Number(options.stabilization.position_floor_storage_px);
	value["scale_floor"] = Number(options.stabilization.scale_floor);
	value["angle_floor_deg"] = Number(options.stabilization.angle_floor_deg);
	value["apply_fad"] = options.apply_fad;
	value["scale_border"] = options.scale_border;
	value["scale_shadow"] = options.scale_shadow;
	value["scale_blur"] = options.scale_blur;
	return value;
}

ApplyPlanOptions Options(json::Object const& value) {
	ApplyPlanOptions options;
	options.mode = static_cast<ApplyMode>(Integer(value.at("mode")));
	options.compact_epsilon = Number(value.at("compact_epsilon"));
	options.position_decimals = Integer(value.at("position_decimals"));
	options.smooth_frames = Integer(value.at("smooth_frames"));
	options.stabilization.enable = static_cast<json::Boolean const&>(value.at("stabilize"));
	options.stabilization.position_floor_storage_px = Number(value.at("position_floor_storage_px"));
	options.stabilization.scale_floor = Number(value.at("scale_floor"));
	options.stabilization.angle_floor_deg = Number(value.at("angle_floor_deg"));
	options.apply_fad = static_cast<json::Boolean const&>(value.at("apply_fad"));
	options.scale_border = static_cast<json::Boolean const&>(value.at("scale_border"));
	options.scale_shadow = static_cast<json::Boolean const&>(value.at("scale_shadow"));
	options.scale_blur = static_cast<json::Boolean const&>(value.at("scale_blur"));
	return options;
}

json::Object FadeEdge(FadeIntervalEdge const& edge) {
	json::Object value;
	value["detected"] = edge.detected;
	value["outer_frame"] = edge.outer_frame;
	value["full_visibility_frame"] = edge.full_visibility_frame;
	value["confidence"] = Number(edge.confidence);
	return value;
}

FadeIntervalEdge FadeEdge(json::Object const& value) {
	return {.detected = static_cast<json::Boolean const&>(value.at("detected")),
			.outer_frame = Integer(value.at("outer_frame")),
			.full_visibility_frame = Integer(value.at("full_visibility_frame")),
			.confidence = Number(value.at("confidence"))};
}

json::Object Timecodes(agi::vfr::Framerate const& timecodes) {
	auto state = timecodes.GetState();
	json::Object value;
	value["numerator"] = state.numerator;
	value["denominator"] = state.denominator;
	value["last"] = state.last;
	value["drop"] = state.drop;
	json::Array frames;
	for (auto time : state.timecodes)
		frames.emplace_back(time);
	value["timecodes"] = std::move(frames);
	return value;
}

agi::vfr::Framerate Timecodes(json::Object const& value) {
	agi::vfr::FramerateState state;
	state.numerator = static_cast<json::Integer const&>(value.at("numerator"));
	state.denominator = static_cast<json::Integer const&>(value.at("denominator"));
	state.last = static_cast<json::Integer const&>(value.at("last"));
	state.drop = static_cast<json::Boolean const&>(value.at("drop"));
	state.timecodes.clear();
	for (auto const& time : Array(value, "timecodes"))
		state.timecodes.push_back(Integer(time));
	return agi::vfr::Framerate::FromState(std::move(state));
}

json::Object Sample(TrackSample const& sample) {
	json::Object value;
	value["frame"] = sample.frame;
	value["model"] = static_cast<int>(sample.model);
	value["status"] = static_cast<int>(sample.status);
	value["failure"] = static_cast<int>(sample.failure);
	value["confidence"] = Number(sample.confidence);
	value["residual"] = Number(sample.residual);
	value["center_x"] = Number(sample.center_x);
	value["center_y"] = Number(sample.center_y);
	value["fade_visibility"] = Number(sample.fade_visibility);
	json::Array matrix;
	for (double entry : sample.transform.matrix)
		matrix.emplace_back(Number(entry));
	value["matrix"] = std::move(matrix);
	return value;
}

TrackSample Sample(json::Object const& value) {
	TrackSample sample;
	sample.frame = Integer(value.at("frame"));
	sample.model = static_cast<TrackModel>(Integer(value.at("model")));
	sample.status = static_cast<TrackStatus>(Integer(value.at("status")));
	sample.failure = static_cast<TrackFailureReason>(Integer(value.at("failure")));
	sample.confidence = Number(value.at("confidence"));
	sample.residual = Number(value.at("residual"));
	sample.center_x = Number(value.at("center_x"));
	sample.center_y = Number(value.at("center_y"));
	sample.fade_visibility = Number(value.at("fade_visibility"));
	auto const& matrix = Array(value, "matrix");
	if (matrix.size() != sample.transform.matrix.size())
		throw std::runtime_error("Diagnostic transform must have nine entries");
	std::ranges::transform(matrix, sample.transform.matrix.begin(),
						   [](auto const& entry) { return Number(entry); });
	return sample;
}

int SourceIndex(std::vector<AssDialogue *> const& targets, AssDialogue const *source) {
	auto found = std::ranges::find(targets, source);
	if (found == targets.end())
		throw std::runtime_error("Plan source is not a diagnostic target");
	return static_cast<int>(found - targets.begin());
}

AssDialogue *Source(std::vector<AssDialogue *> const& targets, json::UnknownElement const& value) {
	int index = Integer(value);
	if (index < 0 || static_cast<size_t>(index) >= targets.size())
		throw std::runtime_error("Diagnostic plan source is out of range");
	return targets[index];
}

json::Object Plan(MotionTrackApplyPlan const& plan, std::vector<AssDialogue *> const& targets) {
	json::Object value;
	value["status"] = static_cast<int>(plan.status);
	value["message"] = plan.message;
	value["event_count"] = static_cast<int64_t>(plan.event_count);
	json::Array lines;
	for (auto const& line : plan.lines) {
		json::Object item;
		item["source"] = SourceIndex(targets, line.source);
		json::Array parts;
		for (auto const& part : line.parts) {
			json::Object p;
			p["start_ms"] = part.start_ms;
			p["end_ms"] = part.end_ms;
			p["text"] = part.text;
			p["covered"] = part.covered;
			p["x0"] = Number(part.x0);
			p["y0"] = Number(part.y0);
			p["x1"] = Number(part.x1);
			p["y1"] = Number(part.y1);
			parts.emplace_back(std::move(p));
		}
		item["parts"] = std::move(parts);
		lines.emplace_back(std::move(item));
	}
	value["lines"] = std::move(lines);
	json::Array uncovered;
	for (auto const& line : plan.uncovered) {
		json::Object item;
		item["source"] = SourceIndex(targets, line.source);
		json::Array ranges;
		for (auto range : line.ranges)
			ranges.emplace_back(Interval(FrameInterval{.first = range.first, .last = range.last}));
		item["ranges"] = std::move(ranges);
		uncovered.emplace_back(std::move(item));
	}
	value["uncovered"] = std::move(uncovered);
	json::Array review;
	for (auto *line : plan.needs_manual_review)
		review.emplace_back(SourceIndex(targets, line));
	value["needs_manual_review"] = std::move(review);
	return value;
}

MotionTrackApplyPlan Plan(json::Object const& value, std::vector<AssDialogue *> const& targets) {
	MotionTrackApplyPlan plan;
	plan.status = static_cast<ApplyPlanStatus>(Integer(value.at("status")));
	plan.message = static_cast<json::String const&>(value.at("message"));
	int count = Integer(value.at("event_count"));
	if (count < 0)
		throw std::runtime_error("Negative diagnostic event count");
	plan.event_count = static_cast<size_t>(count);
	for (auto const& entry : Array(value, "lines")) {
		auto const& item = static_cast<json::Object const&>(entry);
		PlannedLine line;
		line.source = Source(targets, item.at("source"));
		for (auto const& part : Array(item, "parts")) {
			auto const& p = static_cast<json::Object const&>(part);
			line.parts.push_back({.start_ms = Integer(p.at("start_ms")), .end_ms = Integer(p.at("end_ms")), .text = static_cast<json::String const&>(p.at("text")), .covered = static_cast<json::Boolean const&>(p.at("covered")), .x0 = Number(p.at("x0")), .y0 = Number(p.at("y0")), .x1 = Number(p.at("x1")), .y1 = Number(p.at("y1"))});
		}
		plan.lines.push_back(std::move(line));
	}
	for (auto const& entry : Array(value, "uncovered")) {
		auto const& item = static_cast<json::Object const&>(entry);
		LineUncovered line;
		line.source = Source(targets, item.at("source"));
		for (auto const& range : Array(item, "ranges")) {
			auto interval = Interval(static_cast<json::Object const&>(range));
			line.ranges.push_back({.first = interval.first, .last = interval.last});
		}
		plan.uncovered.push_back(std::move(line));
	}
	for (auto const& entry : Array(value, "needs_manual_review"))
		plan.needs_manual_review.push_back(Source(targets, entry));
	return plan;
}

// Imported diagnostics are external input. Framerate's fast integer arithmetic
// assumes a practical frame/time domain; check that domain before replay can
// query its mapping, without altering the original rate or rounding phase.
void ValidateReplayTimeDomain(MotionTrackDebugBundle const& bundle) {
	auto const& input = bundle.input;
	auto state = input.timecodes.GetState();
	if (state.numerator == 0)
		return;
	long double first_frame = -1;
	long double last_frame = std::max(1, input.video_frame_count);
	auto include_frame = [&](int frame) {
		first_frame = std::min(first_frame, static_cast<long double>(frame) - 1);
		last_frame = std::max(last_frame, static_cast<long double>(frame) + 1);
	};
	include_frame(input.decode_interval.first);
	include_frame(input.decode_interval.last);
	include_frame(input.direction_domain.first);
	include_frame(input.direction_domain.last);
	for (auto const& sample : input.samples)
		include_frame(sample.frame);
	long double const integer_limit = std::numeric_limits<int>::max();
	long double const wide_limit = std::numeric_limits<int64_t>::max();
	long double const base = static_cast<long double>(state.denominator) * 1000;
	auto const numerator = static_cast<long double>(state.numerator);
	long double const extrapolated = std::max(0.0L, last_frame - state.timecodes.size() + 1) * base + state.last + numerator / 2;
	if (first_frame <= -integer_limit || last_frame >= integer_limit ||
		extrapolated >= wide_limit || first_frame * base <= -wide_limit)
		throw std::runtime_error("Diagnostic frame domain exceeds safe timecode arithmetic");
	long double first_ms = std::min(static_cast<long double>(input.seed_time_ms), std::floor(first_frame * base / numerator));
	long double last_ms = std::max({static_cast<long double>(input.seed_time_ms),
									static_cast<long double>(state.timecodes.back()), std::ceil(extrapolated / numerator)});
	for (auto *line : bundle.targets) {
		first_ms = std::min({first_ms, static_cast<long double>(line->Start.GetMillisecond()), static_cast<long double>(line->End.GetMillisecond())});
		last_ms = std::max({last_ms, static_cast<long double>(line->Start.GetMillisecond()), static_cast<long double>(line->End.GetMillisecond())});
	}
	if (first_ms <= -integer_limit || last_ms >= integer_limit ||
		(first_ms - 1) * numerator <= -wide_limit + 999 ||
		(last_ms + 1) * numerator + base >= wide_limit)
		throw std::runtime_error("Diagnostic time domain exceeds safe timecode arithmetic");
}
} // namespace

void WriteMotionTrackDebugBundle(std::ostream& output, AssFile const& file,
								 std::vector<AssDialogue *> const& targets, ApplyPlanInput const& input,
								 MotionTrackApplyPlan const& plan, MotionTrackSnapshot const *snapshot,
								 std::string const& application_version) {
	json::Object root;
	root["format"] = "aegisub-motion-track-debug";
	root["version"] = schema_version;
	root["application_version"] = application_version;
	root["text_extents"] = input.text_extents ? "platform-provider" : "deterministic-approximation";
	json::Object source;
	json::Array info;
	for (auto const& entry : file.Info) {
		if (!IsRenderingInfo(entry.Key()))
			continue;
		json::Object value;
		value["key"] = entry.Key();
		value["value"] = entry.Value();
		info.emplace_back(std::move(value));
	}
	source["script_info"] = std::move(info);
	StyleSelection selection{.file = file, .styles = {}};
	json::Array dialogue;
	for (auto *line : targets) {
		if (!line)
			throw std::runtime_error("Null diagnostic target");
		json::Object value;
		value["entry"] = line->GetEntryData();
		value["start_ms"] = line->Start.GetMillisecond();
		value["end_ms"] = line->End.GetMillisecond();
		value["text"] = line->Text.get();
		value["style"] = line->Style.get();
		value["actor"] = line->Actor.get();
		value["effect"] = line->Effect.get();
		json::Array margins;
		for (int margin : line->Margin)
			margins.emplace_back(margin);
		value["margins"] = std::move(margins);
		dialogue.emplace_back(std::move(value));
		selection.styles.insert(ass_style_resolution::ResolveEventStyle(file, line->Style));
		for (auto const& block : line->ParseTags())
			if (block->GetType() == AssBlockType::OVERRIDE)
				CollectResetStyles(block->GetRawText(), selection);
	}
	source["dialogue"] = std::move(dialogue);
	json::Array styles;
	for (auto const& style : file.Styles)
		if (selection.styles.contains(&style))
			styles.emplace_back(Style(style));
	source["styles"] = std::move(styles);
	root["source"] = std::move(source);
	json::Object parameters;
	parameters["model"] = static_cast<int>(input.model);
	parameters["origin_center_x"] = Number(input.origin_center_x);
	parameters["origin_center_y"] = Number(input.origin_center_y);
	parameters["decode_interval"] = Interval(input.decode_interval);
	parameters["direction_domain"] = Interval(input.direction_domain);
	parameters["storage_width"] = input.storage_width;
	parameters["storage_height"] = input.storage_height;
	parameters["script_width"] = input.script_width;
	parameters["script_height"] = input.script_height;
	parameters["video_frame_count"] = input.video_frame_count;
	parameters["seed_time_ms"] = input.seed_time_ms;
	parameters["timecodes"] = Timecodes(input.timecodes);
	parameters["options"] = Options(input.options);
	if (input.fade_interval) {
		json::Object fade;
		fade["in"] = FadeEdge(input.fade_interval->fade_in);
		fade["out"] = FadeEdge(input.fade_interval->fade_out);
		parameters["fade_interval"] = std::move(fade);
	}
	json::Array samples;
	for (auto const& sample : input.samples)
		samples.emplace_back(Sample(sample));
	parameters["samples"] = std::move(samples);
	root["input"] = std::move(parameters);
	root["plan"] = Plan(plan, targets);
	if (snapshot) {
		json::Object metadata;
		json::Object roi;
		roi["x"] = snapshot->roi.x;
		roi["y"] = snapshot->roi.y;
		roi["w"] = snapshot->roi.w;
		roi["h"] = snapshot->roi.h;
		metadata["roi"] = std::move(roi);
		metadata["origin_seed_frame"] = snapshot->origin_seed_frame;
		metadata["backend_seed_frame"] = snapshot->backend_seed_frame;
		metadata["direction"] = static_cast<int>(snapshot->direction);
		metadata["stop_reason"] = static_cast<int>(snapshot->stop_reason);
		metadata["success_count"] = snapshot->success_count;
		metadata["failure_count"] = snapshot->failure_count;
		root["tracking"] = std::move(metadata);
	}
	agi::JsonWriter::Write(root, output);
	if (!output)
		throw std::runtime_error("Could not write motion track debug data");
}

MotionTrackDebugBundle ReadMotionTrackDebugBundle(std::istream& stream,
												  perspective::AssTextExtentsProvider text_extents) {
	json::UnknownElement document;
	json::Reader::Read(document, stream);
	auto const& root = static_cast<json::Object const&>(document);
	if (static_cast<json::String const&>(root.at("format")) != "aegisub-motion-track-debug" ||
		Integer(root.at("version")) != schema_version)
		throw std::runtime_error("Unsupported motion track diagnostic format/version");
	MotionTrackDebugBundle bundle;
	bundle.file = std::make_unique<AssFile>();
	bundle.used_platform_text_extents = static_cast<json::String const&>(root.at("text_extents")) == "platform-provider";
	auto const& source = Object(root, "source");
	for (auto const& entry : Array(source, "script_info")) {
		auto const& value = static_cast<json::Object const&>(entry);
		auto const& key = static_cast<json::String const&>(value.at("key"));
		if (!IsRenderingInfo(key))
			throw std::runtime_error("Unexpected diagnostic script info key");
		bundle.file->Info.emplace_back(key, static_cast<json::String const&>(value.at("value")));
	}
	for (auto const& entry : Array(source, "styles")) {
		auto style = Style(static_cast<json::Object const&>(entry));
		bundle.file->Styles.push_back(*style.release());
	}
	for (auto const& entry : Array(source, "dialogue")) {
		auto const& value = static_cast<json::Object const&>(entry);
		auto line = std::make_unique<AssDialogue>(static_cast<json::String const&>(value.at("entry")));
		line->Start = Integer(value.at("start_ms"));
		line->End = Integer(value.at("end_ms"));
		line->Text = static_cast<json::String const&>(value.at("text"));
		line->Style = static_cast<json::String const&>(value.at("style"));
		line->Actor = static_cast<json::String const&>(value.at("actor"));
		line->Effect = static_cast<json::String const&>(value.at("effect"));
		auto const& margins = Array(value, "margins");
		if (margins.size() != line->Margin.size())
			throw std::runtime_error("Diagnostic dialogue must have three margins");
		std::ranges::transform(margins, line->Margin.begin(), Integer);
		bundle.targets.push_back(line.get());
		bundle.file->Events.push_back(*line.release());
	}
	auto const& value = Object(root, "input");
	auto& input = bundle.input;
	input.model = static_cast<TrackModel>(Integer(value.at("model")));
	input.origin_center_x = Number(value.at("origin_center_x"));
	input.origin_center_y = Number(value.at("origin_center_y"));
	input.decode_interval = Interval(Object(value, "decode_interval"));
	input.direction_domain = Interval(Object(value, "direction_domain"));
	input.storage_width = Integer(value.at("storage_width"));
	input.storage_height = Integer(value.at("storage_height"));
	input.script_width = Integer(value.at("script_width"));
	input.script_height = Integer(value.at("script_height"));
	input.video_frame_count = Integer(value.at("video_frame_count"));
	input.seed_time_ms = Integer(value.at("seed_time_ms"));
	input.timecodes = Timecodes(Object(value, "timecodes"));
	input.options = Options(Object(value, "options"));
	if (value.contains("fade_interval")) {
		auto const& fade = Object(value, "fade_interval");
		input.fade_interval = FadeInterval{.fade_in = FadeEdge(Object(fade, "in")), .fade_out = FadeEdge(Object(fade, "out"))};
	}
	for (auto const& sample : Array(value, "samples"))
		input.samples.push_back(Sample(static_cast<json::Object const&>(sample)));
	input.text_extents = text_extents;
	bundle.exported_plan = Plan(Object(root, "plan"), bundle.targets);
	ValidateReplayTimeDomain(bundle);
	return bundle;
}
} // namespace aegisub::motion_track
