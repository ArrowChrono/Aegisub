#include <main.h>

#include "../../src/motion_track/apply_source.h"
#include "../../src/perspective_ass_state.h"

#include <ass_file.h>
#include <ass_io_core.h>
#include <ass_style.h>

#include <libaegisub/fs.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

class motion_track_apply_source : public testing::Test {
	protected:
	AssFile file;
	MotionTrackApplySource source;
	ApplyPlanInput input;
	AssDialogue *untouched = nullptr;
	std::vector<AssDialogue *> selection;
	int commits = 0;

	void SetUp() override {
		file.Styles.push_back(*new AssStyle);
		auto *first = AddLine(R"({\pos(0,0)\frz15\fscx120\fscy80}first)");
		untouched = AddLine("unselected");
		auto *second = AddLine(R"({\pos(20,30)\frz15\fscx120\fscy80}second)");
		file.Commit("setup", AssFile::COMMIT_DIAG_ADDREM);
		selection = {first, second};
		input.model = TrackModel::Similarity;
		input.decode_interval = {.first = 0, .last = 19};
		input.direction_domain = {.first = 0, .last = 19};
		input.storage_width = input.script_width = 1920;
		input.storage_height = input.script_height = 1080;
		input.timecodes = agi::vfr::Framerate(10.0);
		input.video_frame_count = 20;
		input.options.compact_epsilon = 100.0;
		for (int frame = 0; frame < 20; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			sample.center_x = 2.0 * frame;
			double const angle = 0.5 * frame * std::numbers::pi / 180.0;
			double const scale = 1.0 + 0.01 * frame;
			sample.transform.matrix[0] = scale * std::cos(angle);
			sample.transform.matrix[1] = -scale * std::sin(angle);
			sample.transform.matrix[3] = scale * std::sin(angle);
			sample.transform.matrix[4] = scale * std::cos(angle);
			input.samples.push_back(sample);
		}
		source.Capture(file, selection, input.timecodes, input.video_frame_count);
	}

	AssDialogue *AddLine(std::string text) {
		auto *line = new AssDialogue;
		line->Start = 0;
		line->End = 1950;
		line->Text = std::move(text);
		file.Events.push_back(*line);
		return line;
	}

	bool Apply(MotionTrackApplyPlan const& plan, agi::vfr::Framerate const& timecodes,
			   std::function<void(AssFile&)> const& prepare = {}) {
		std::string message;
		auto prepared = source.Prepare(file, plan, timecodes, message);
		if (!prepared)
			return false;
		int const type = prepared->Candidate().PrepareCommit(
			AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);
		if (prepare)
			prepare(prepared->Candidate());
		prepared->Finalize(timecodes);
		prepared->Install();
		for (auto *line : selection)
			EXPECT_FALSE(line->Text.get().empty());
		selection = prepared->Outputs();
		int revision = -1;
		file.PublishCommit({.message = "apply", .commit_id = &revision, .single_line = nullptr, .type = type, .changed_lines = {}}, false);
		++commits;
		return true;
	}

	MotionTrackApplyPlan Plan(ApplyMode mode) {
		input.options.mode = mode;
		return BuildApplyPlan(file, source.Targets(), input);
	}

	[[nodiscard]] std::vector<std::string> Entries() const {
		std::vector<std::string> entries;
		for (auto const& line : file.Events)
			entries.push_back(line.GetEntryData());
		return entries;
	}

	[[nodiscard]] std::vector<std::uintptr_t> EventIdentities() const {
		std::vector<std::uintptr_t> identities;
		for (auto const& line : file.Events)
			identities.push_back(reinterpret_cast<std::uintptr_t>(&line));
		return identities;
	}

	void ExpectSourceSnapshot(MotionTrackSourceSnapshot const& expected) const {
		auto const& actual = source.Snapshot();
		EXPECT_EQ(expected.file_identity, actual.file_identity);
		EXPECT_EQ(expected.script_info_entries, actual.script_info_entries);
		EXPECT_EQ(expected.video_frame_count, actual.video_frame_count);
		ASSERT_EQ(expected.timecodes.IsLoaded(), actual.timecodes.IsLoaded());
		for (int frame = 0; frame <= expected.video_frame_count; ++frame)
			EXPECT_EQ(expected.timecodes.TimeAtFrame(frame), actual.timecodes.TimeAtFrame(frame)) << frame;
		ASSERT_EQ(expected.lines.size(), actual.lines.size());
		auto fields = [](MotionTrackLineFingerprint const& line) {
			return std::tie(line.identity, line.id, line.row, line.comment, line.layer, line.margins,
							line.start_ms, line.end_ms, line.style, line.actor, line.effect,
							line.extradata_ids, line.text, line.style_entry);
		};
		for (std::size_t index = 0; index < expected.lines.size(); ++index)
			EXPECT_EQ(fields(expected.lines[index]), fields(actual.lines[index])) << index;
	}
};
class motion_track_similarity_serialized_precision : public motion_track_apply_source,
													 public testing::WithParamInterface<int> {};

struct SerializedSimilarityPose {
	double x;
	double y;
	double rotation;
	double scale_x;
	double scale_y;
};

std::optional<SerializedSimilarityPose> ReadSerializedSimilarityPose(std::string const& text) {
	static std::regex const position(R"(\\pos\((-?[0-9]+(?:\.[0-9]+)?),(-?[0-9]+(?:\.[0-9]+)?)\))");
	auto match = std::sregex_iterator(text.begin(), text.end(), position);
	if (match == std::sregex_iterator())
		return std::nullopt;
	SerializedSimilarityPose pose{.x = std::stod((*match)[1]), .y = std::stod((*match)[2]), .rotation = 0.0, .scale_x = 0.0, .scale_y = 0.0};
	if (++match != std::sregex_iterator())
		return std::nullopt;
	auto channel = [&](char const *tag, double& value) {
		std::regex const pattern(std::string(R"(\\)") + tag + R"((-?[0-9]+(?:\.[0-9]+)?))");
		auto number = std::sregex_iterator(text.begin(), text.end(), pattern);
		if (number == std::sregex_iterator())
			return false;
		value = std::stod((*number)[1]);
		return ++number == std::sregex_iterator() && std::isfinite(value);
	};
	if (!channel("frz", pose.rotation) || !channel("fscx", pose.scale_x) || !channel("fscy", pose.scale_y))
		return std::nullopt;
	return pose;
}

class SerializedPrecisionFile {
	std::filesystem::path path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-motion-precision-%%%%%%%%%%%%%%%%.ass");

	public:
	~SerializedPrecisionFile() {
		std::error_code error;
		std::filesystem::remove(path, error);
	}

	[[nodiscard]] std::filesystem::path const& Path() const { return path; }
};

struct IndependentAssPoint {
	double x = 0.0;
	double y = 0.0;
};

struct IndependentAssPose {
	IndependentAssPoint position;
	std::optional<IndependentAssPoint> origin;
	std::array<double, 7> channels{100.0, 100.0, 0.0, 0.0, 0.0, 0.0, 0.0};
	std::string body;
	bool moved = false;
	bool animated = false;
	bool offset_window = false;
	bool drawing = false;
	bool top_left_alignment = false;
};

class IndependentAssReader {
	IndependentAssPose pose;
	int elapsed;
	int duration;
	bool positioned = false;
	std::set<std::string> static_tags;

	static double Number(std::string_view value) {
		size_t consumed = 0;
		double const number = std::stod(std::string(value), &consumed);
		if (consumed != value.size() || !std::isfinite(number))
			throw std::runtime_error("Unsupported or nonfinite ASS number");
		return number;
	}

	static std::vector<double> Numbers(std::string_view text) {
		std::vector<double> result;
		while (!text.empty()) {
			auto const comma = text.find(',');
			result.push_back(Number(text.substr(0, comma)));
			if (comma == std::string_view::npos)
				break;
			text.remove_prefix(comma + 1);
			if (text.empty())
				throw std::runtime_error("Empty ASS argument");
		}
		return result;
	}

	static std::optional<size_t> Channel(std::string_view tag) {
		constexpr std::array<std::string_view, 7> names{"fscx", "fscy", "fax", "fay", "frx", "fry", "frz"};
		for (size_t i = 0; i < names.size(); ++i)
			if (tag == names[i] || (i == 6 && tag == "fr"))
				return i;
		return std::nullopt;
	}

	void Block(std::string_view text, bool transform = false, double progress = 1.0) {
		std::set<size_t> transformed_channels;
		while (!text.empty()) {
			if (text.front() != '\\')
				throw std::runtime_error("Unconsumed ASS override text");
			text.remove_prefix(1);
			size_t length = 0;
			while (length < text.size() && std::isalpha(static_cast<unsigned char>(text[length])))
				++length;
			std::string const tag(text.substr(0, length));
			text.remove_prefix(length);
			bool const parenthesized = !text.empty() && text.front() == '(';
			std::string_view argument;
			if (parenthesized) {
				int depth = 0;
				size_t end = 0;
				for (; end < text.size(); ++end) {
					if (text[end] == '(')
						++depth;
					else if (text[end] == ')' && --depth == 0)
						break;
				}
				if (end == text.size())
					throw std::runtime_error("Unclosed ASS argument");
				argument = text.substr(1, end - 1);
				text.remove_prefix(end + 1);
			}
			else {
				auto const next = text.find('\\');
				argument = text.substr(0, next);
				text.remove_prefix(argument.size());
			}
			if (auto const channel = Channel(tag)) {
				if (parenthesized || (transform && !transformed_channels.insert(*channel).second) ||
					(!transform && !static_tags.insert("channel" + std::to_string(*channel)).second))
					throw std::runtime_error("Duplicate or malformed ASS channel");
				double const target = Number(argument);
				pose.channels[*channel] += progress * (target - pose.channels[*channel]);
				continue;
			}
			if (transform)
				throw std::runtime_error("Unsupported animated ASS tag: " + tag);
			if (tag == "t" && parenthesized) {
				auto const first_tag = argument.find('\\');
				if (first_tag == std::string_view::npos)
					throw std::runtime_error("ASS transform has no channels");
				auto prefix = argument.substr(0, first_tag);
				if (!prefix.empty()) {
					if (prefix.back() != ',')
						throw std::runtime_error("Malformed ASS transform timing");
					prefix.remove_suffix(1);
				}
				auto const timing = Numbers(prefix);
				double first = 0.0, last = duration, acceleration = 1.0;
				if (timing.size() == 1)
					acceleration = timing[0];
				else if (timing.size() == 2 || timing.size() == 3) {
					first = timing[0];
					last = timing[1];
					if (timing.size() == 3)
						acceleration = timing[2];
				}
				else if (!timing.empty())
					throw std::runtime_error("Unsupported ASS transform timing");
				if (last <= first || acceleration <= 0.0)
					throw std::runtime_error("Invalid ASS transform interval");
				double const factor = std::pow(std::clamp((elapsed - first) / (last - first), 0.0, 1.0), acceleration);
				Block(argument.substr(first_tag), true, factor);
				pose.animated = true;
				pose.offset_window |= first != 0.0;
				continue;
			}
			if (tag == "pos" || tag == "move" || tag == "org") {
				if (!parenthesized)
					throw std::runtime_error("ASS point tag must have arguments");
				auto const values = Numbers(argument);
				if (tag == "org") {
					if (values.size() != 2 || pose.origin)
						throw std::runtime_error("Duplicate or malformed ASS origin");
					pose.origin = IndependentAssPoint{.x = values[0], .y = values[1]};
				}
				else {
					if (positioned || (tag == "pos" ? values.size() != 2 : values.size() != 6))
						throw std::runtime_error("Duplicate or malformed ASS position");
					positioned = true;
					pose.position = {.x = values[0], .y = values[1]};
					if (tag == "move") {
						if (values[5] <= values[4])
							throw std::runtime_error("Invalid ASS move interval");
						double const factor = std::clamp((elapsed - values[4]) / (values[5] - values[4]), 0.0, 1.0);
						pose.position.x += factor * (values[2] - values[0]);
						pose.position.y += factor * (values[3] - values[1]);
						pose.moved = true;
						pose.offset_window |= values[4] != 0.0;
					}
				}
				continue;
			}
			if (parenthesized || !static_tags.insert(tag).second)
				throw std::runtime_error("Duplicate or unsupported ASS tag: " + tag);
			if (tag == "an" && argument == "7") {
				pose.top_left_alignment = true;
				continue;
			}
			if (tag == "p" && argument == "1") {
				pose.drawing = true;
				continue;
			}
			if (tag == "b" && argument == "1")
				continue;
			throw std::runtime_error("Unsupported ASS tag: " + tag);
		}
	}

	public:
	IndependentAssReader(int timestamp, int start, int end) : elapsed(timestamp - start), duration(end - start) {
		if (elapsed < 0 || elapsed >= duration)
			throw std::runtime_error("ASS event is not active");
	}

	IndependentAssPose Read(std::string const& text) {
		std::string_view remaining(text);
		while (!remaining.empty() && remaining.front() == '{') {
			auto const end = remaining.find('}');
			if (end == std::string_view::npos)
				throw std::runtime_error("Unclosed ASS override block");
			Block(remaining.substr(1, end - 1));
			remaining.remove_prefix(end + 1);
		}
		if (!positioned || remaining.find_first_of("{}") != std::string_view::npos)
			throw std::runtime_error("Unsupported ASS body or absent position");
		if (pose.drawing && !pose.top_left_alignment)
			throw std::runtime_error("Drawing requires explicit supported alignment");
		pose.body = remaining;
		return pose;
	}
};

IndependentAssPoint IndependentProjectDrawing(IndependentAssPose const& pose, IndependentAssPoint point,
											  double aspect, double camera) {
	if (!pose.drawing || !pose.top_left_alignment)
		throw std::runtime_error("Independent projection requires an explicit an7 drawing");
	using Matrix = std::array<std::array<double, 4>, 4>;
	auto const origin = pose.origin.value_or(pose.position);
	std::array<double, 4> value{point.x, point.y, 0.0, 1.0};
	auto multiply = [&](Matrix const& matrix) {
		std::array<double, 4> next{};
		for (size_t row = 0; row < 4; ++row)
			for (size_t column = 0; column < 4; ++column)
				next[row] += matrix[row][column] * value[column];
		value = next;
	};
	auto const& c = pose.channels;
	multiply({{{c[0] * aspect / 100.0, c[0] * c[2] / 100.0, 0.0, (pose.position.x - origin.x) * aspect},
			   {c[1] * aspect * c[3] / 100.0, c[1] / 100.0, 0.0, pose.position.y - origin.y},
			   {0.0, 0.0, 1.0, 0.0},
			   {0.0, 0.0, 0.0, 1.0}}});
	auto radians = [](double degrees) { return degrees * std::numbers::pi / 180.0; };
	double a = radians(-c[6]);
	multiply({{{std::cos(a), -std::sin(a), 0.0, 0.0}, {std::sin(a), std::cos(a), 0.0, 0.0}, {0.0, 0.0, 1.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}});
	a = radians(-c[4]);
	multiply({{{1.0, 0.0, 0.0, 0.0}, {0.0, std::cos(a), -std::sin(a), 0.0}, {0.0, std::sin(a), std::cos(a), 0.0}, {0.0, 0.0, 0.0, 1.0}}});
	a = radians(c[5]);
	multiply({{{std::cos(a), 0.0, -std::sin(a), 0.0}, {0.0, 1.0, 0.0, 0.0}, {std::sin(a), 0.0, std::cos(a), 0.0}, {0.0, 0.0, 0.0, 1.0}}});
	double const denominator = 1.0 + value[2] / camera;
	if (!(denominator > 0.0) || !(aspect > 0.0) || !(camera > 0.0))
		throw std::runtime_error("Independent projection crosses the camera plane");
	IndependentAssPoint const projected{.x = origin.x + value[0] / (aspect * denominator), .y = origin.y + value[1] / denominator};
	if (!std::isfinite(projected.x) || !std::isfinite(projected.y))
		throw std::runtime_error("Nonfinite independent projection");
	return projected;
}

AssDialogue const *UniqueActiveEvent(AssFile const& file, std::string const& actor, int timestamp) {
	AssDialogue const *found = nullptr;
	for (auto const& line : file.Events) {
		if (line.Actor.get() != actor || line.Start > timestamp || line.End <= timestamp)
			continue;
		if (found)
			throw std::runtime_error("Overlapping serialized target events");
		found = &line;
	}
	if (!found)
		throw std::runtime_error("Missing serialized target event");
	return found;
}

IndependentAssPose ReadActivePose(AssFile const& file, std::string const& actor, int timestamp) {
	auto const *line = UniqueActiveEvent(file, actor, timestamp);
	return IndependentAssReader(timestamp, line->Start, line->End).Read(line->Text.get());
}

class motion_track_geometry_serialized_precision : public motion_track_apply_source,
												   public testing::WithParamInterface<std::tuple<TrackModel, int>> {
	protected:
	static constexpr double StorageX = 2.0;
	static constexpr double StorageY = 3.0;
	std::array<std::string, 2> const actors{"geometry-first", "geometry-second"};
	std::array<std::string, 2> const drawings{"m 0 0 l 80 0 80 40 0 40", "m 0 0 l 60 0 60 50 0 50"};
	std::array<std::array<IndependentAssPoint, 4>, 2> const corners{
		std::array<IndependentAssPoint, 4>{{{.x = 0, .y = 0}, {.x = 80, .y = 0}, {.x = 80, .y = 40}, {.x = 0, .y = 40}}},
		std::array<IndependentAssPoint, 4>{{{.x = 0, .y = 0}, {.x = 60, .y = 0}, {.x = 60, .y = 50}, {.x = 0, .y = 50}}}};

	void ConfigureGeometry(TrackModel model) {
		file.SetScriptInfo("PlayResX", "960");
		file.SetScriptInfo("PlayResY", "360");
		file.SetScriptInfo("LayoutResX", "1920");
		file.SetScriptInfo("LayoutResY", "1080");
		file.GetStyle("Default")->outline_w = 0;
		file.GetStyle("Default")->shadow_w = 0;
		file.GetStyle("Default")->UpdateData();
		for (size_t target = 0; target < selection.size(); ++target) {
			selection[target]->Actor = actors[target];
			selection[target]->Text = R"({\an7\pos(200,150)\fscx125\fscy80\p1})" + drawings[target];
			selection[target]->End = 750;
		}
		input.model = model;
		input.script_width = 960;
		input.script_height = 360;
		input.storage_width = 1920;
		input.storage_height = 1080;
		input.origin_center_x = 400;
		input.origin_center_y = 450;
		input.video_frame_count = 8;
		input.decode_interval = input.direction_domain = {.first = 0, .last = 7};
		input.options.compact_epsilon = 0.075;
		input.samples.clear();
		for (int frame = 0; frame < 8; ++frame) {
			double const angle = 0.08 + 0.015 * frame;
			double const zoom = 1.0 + 0.01 * frame;
			double const shear = 0.07 + 0.005 * frame;
			double const tilt = model == TrackModel::Homography ? 0.04 + 0.006 * frame : 0.0;
			double const aspect = StorageX / StorageY;
			double const a = zoom * std::cos(angle);
			double const b = zoom * (std::cos(angle) * shear - std::sin(angle)) / aspect;
			double const c = zoom * std::sin(angle) * aspect;
			double const d = zoom * (std::sin(angle) * shear + std::cos(angle));
			double const p = std::sin(tilt) * c / (312.5 / StorageY);
			double const q = std::sin(tilt) * d / (312.5 / StorageY);
			double const tx = 4.0 * frame, ty = 2.0 * frame;
			TrackSample sample;
			sample.frame = frame;
			sample.model = model;
			sample.status = TrackStatus::Ok;
			sample.center_x = input.origin_center_x + tx * StorageX;
			sample.center_y = input.origin_center_y + ty * StorageY;
			sample.transform.matrix = {a + tx * p, (b + tx * q) * StorageX / StorageY, tx * StorageX,
									   (std::cos(tilt) * c + ty * p) * StorageY / StorageX, std::cos(tilt) * d + ty * q, ty * StorageY,
									   p / StorageX, q / StorageY, 1.0};
			input.samples.push_back(sample);
		}
		source.Capture(file, selection, input.timecodes, input.video_frame_count);
	}

	void ExpectSerializedCoverage(AssFile const& reopened, int end = 750) const {
		int preserved = 0;
		for (auto const& line : reopened.Events) {
			if (line.Actor.get().empty()) {
				EXPECT_EQ(untouched->GetEntryData(), line.GetEntryData());
				++preserved;
			}
			else {
				ASSERT_TRUE(line.Actor.get() == actors[0] || line.Actor.get() == actors[1]);
				EXPECT_EQ("Default", line.Style.get());
				EXPECT_TRUE(line.Effect.get().empty());
				EXPECT_GT(line.End, line.Start);
				EXPECT_EQ(0, static_cast<int>(line.Start) % 10);
				EXPECT_EQ(0, static_cast<int>(line.End) % 10);
				EXPECT_NO_THROW(IndependentAssReader(line.Start, line.Start, line.End).Read(line.Text.get()));
			}
		}
		EXPECT_EQ(1, preserved);
		for (auto const& actor : actors) {
			std::vector<AssDialogue const *> events;
			for (auto const& line : reopened.Events)
				if (line.Actor.get() == actor)
					events.push_back(&line);
			ASSERT_FALSE(events.empty());
			std::ranges::sort(events, {}, [](auto const *line) { return static_cast<int>(line->Start); });
			EXPECT_EQ(0, events.front()->Start);
			EXPECT_EQ(end, events.back()->End);
			for (size_t event = 1; event < events.size(); ++event) {
				int const boundary = events[event]->Start;
				EXPECT_EQ(events[event - 1]->End, boundary);
				EXPECT_EQ(events[event - 1], UniqueActiveEvent(reopened, actor, boundary - 1));
				EXPECT_EQ(events[event], UniqueActiveEvent(reopened, actor, boundary));
				EXPECT_NO_THROW(ReadActivePose(reopened, actor, boundary - 1));
				EXPECT_NO_THROW(ReadActivePose(reopened, actor, boundary));
				EXPECT_NO_THROW(ReadActivePose(reopened, actor, boundary + 1));
			}
		}
	}

	void RunGeometry(ApplyMode mode) {
		auto const [model, decimals] = GetParam();
		ConfigureGeometry(model);
		int end = 750;
		if (mode == ApplyMode::Compact) {
			input.timecodes = agi::vfr::Framerate({0, 43, 99, 157, 233, 311, 397, 499, 601});
			end = 550;
			for (auto *line : selection)
				line->End = end;
			input.samples[3].status = input.samples[4].status = TrackStatus::Failed;
			source.Capture(file, selection, input.timecodes, input.video_frame_count);
		}
		input.options.position_decimals = decimals;
		std::array<IndependentAssPose, 2> const baseline{
			ReadActivePose(file, actors[0], 0), ReadActivePose(file, actors[1], 0)};
		auto const plan = Plan(mode);
		ASSERT_TRUE(plan.has_mutations()) << plan.message;
		ASSERT_EQ(2u, plan.lines.size());
		ASSERT_TRUE(Apply(plan, input.timecodes));
		EXPECT_EQ(1, commits);
		SerializedPrecisionFile saved;
		WriteAssFileForCore(&file, saved.Path(), input.timecodes, "UTF-8", AssWriteOptions{});
		AssFile const reopened = ReadAssFileForCore(saved.Path(), "UTF-8");
		ExpectSerializedCoverage(reopened, end);
		ExpectGeometry(reopened, mode == ApplyMode::Exact ? 0.05 : input.options.compact_epsilon, baseline, mode == ApplyMode::Compact);
	}

	void ExpectGeometry(AssFile const& reopened, double tolerance,
						std::array<IndependentAssPose, 2> const& baseline, bool require_animation = false) const {
		bool moved = false, animated = false, offset_window = false;
		double max_error = 0.0;
		int checked_corners = 0;
		for (size_t target = 0; target < actors.size(); ++target) {
			TrackSample const *previous = nullptr;
			AssDialogue const *held_event = nullptr;
			for (auto const& sample : input.samples) {
				if (sample.status == TrackStatus::Ok)
					previous = &sample;
				ASSERT_NE(nullptr, previous);
				int const time = input.timecodes.TimeAtFrame(sample.frame, agi::vfr::Time::EXACT);
				SCOPED_TRACE(testing::Message() << "target=" << target << " frame=" << sample.frame);
				auto const actual = ReadActivePose(reopened, actors[target], time);
				EXPECT_EQ(drawings[target], actual.body);
				EXPECT_TRUE(actual.drawing);
				moved |= actual.moved;
				animated |= actual.animated;
				offset_window |= actual.offset_window;
				if (sample.status == TrackStatus::Failed) {
					auto const *current = UniqueActiveEvent(reopened, actors[target], time);
					if (!held_event)
						held_event = current;
					EXPECT_EQ(held_event, current);
					EXPECT_FALSE(actual.moved);
					EXPECT_FALSE(actual.animated);
				}
				for (auto const corner : corners[target]) {
					auto const original = IndependentProjectDrawing(baseline[target], corner, StorageX / StorageY, 312.5 / StorageY);
					double const x = original.x * StorageX - input.origin_center_x;
					double const y = original.y * StorageY - input.origin_center_y;
					auto const& m = previous->transform.matrix;
					double const denominator = m[6] * x + m[7] * y + m[8];
					ASSERT_GT(denominator, 0.0);
					double const expected_x = input.origin_center_x + (m[0] * x + m[1] * y + m[2]) / denominator;
					double const expected_y = input.origin_center_y + (m[3] * x + m[4] * y + m[5]) / denominator;
					auto const actual_corner = IndependentProjectDrawing(actual, corner, StorageX / StorageY, 312.5 / StorageY);
					double const error = std::hypot(actual_corner.x * StorageX - expected_x, actual_corner.y * StorageY - expected_y);
					max_error = std::max(max_error, error);
					++checked_corners;
					EXPECT_LE(error, tolerance + 1e-8);
				}
			}
		}
		RecordProperty("checked_storage_corners", checked_corners);
		RecordProperty("max_storage_corner_error", std::to_string(max_error));
		if (require_animation) {
			EXPECT_TRUE(moved);
			EXPECT_TRUE(animated);
			EXPECT_TRUE(offset_window);
		}
	}
};
} // namespace

TEST_P(motion_track_similarity_serialized_precision, exact_apply_save_reload_preserves_every_sample_pose) {
	int const decimals = GetParam();
	std::array<double, 2> const base_x{13.375, -26.125};
	std::array<double, 2> const base_y{21.625, 47.875};
	std::array<std::string, 2> const actors{"precision-first", "precision-second"};
	std::array<std::string, 2> const bodies{R"(first, punctuation\Nsecond line)", R"(second \h subtitle)"};
	for (size_t target = 0; target < selection.size(); ++target) {
		selection[target]->Actor = actors[target];
		selection[target]->Effect = "precision-acceptance";
		selection[target]->Text = target == 0
									  ? R"({\pos(13.375,21.625)\frz17.375\fscx123.456\fscy78.912\b1}first, punctuation\Nsecond line)"
									  : R"({\pos(-26.125,47.875)\frz17.375\fscx123.456\fscy78.912\b1}second \h subtitle)";
	}
	std::string const untouched_entry = untouched->GetEntryData();
	input.options.position_decimals = decimals;
	for (auto& sample : input.samples) {
		double const angle = 0.731 * sample.frame * std::numbers::pi / 180.0;
		double const scale = 1.0 + 0.0137 * sample.frame;
		sample.center_x = 1.127 * sample.frame;
		sample.center_y = -0.863 * sample.frame;
		sample.transform.matrix[0] = scale * std::cos(angle);
		sample.transform.matrix[1] = -scale * std::sin(angle);
		sample.transform.matrix[3] = scale * std::sin(angle);
		sample.transform.matrix[4] = scale * std::cos(angle);
	}
	source.Capture(file, selection, input.timecodes, input.video_frame_count);
	auto const plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(40u, plan.event_count);
	ASSERT_TRUE(Apply(plan, input.timecodes));
	ASSERT_EQ(1, commits);

	SerializedPrecisionFile saved;
	WriteAssFileForCore(&file, saved.Path(), input.timecodes, "UTF-8", AssWriteOptions{});
	AssFile const reopened = ReadAssFileForCore(saved.Path(), "UTF-8");
	ASSERT_EQ(41, std::distance(reopened.Events.begin(), reopened.Events.end()));
	int untouched_count = 0;
	for (auto const& line : reopened.Events) {
		if (line.Text.get() == "unselected") {
			++untouched_count;
			EXPECT_EQ(untouched_entry, line.GetEntryData());
		}
	}
	EXPECT_EQ(1, untouched_count);

	for (size_t target = 0; target < actors.size(); ++target) {
		int target_events = 0;
		for (auto const& line : reopened.Events)
			target_events += line.Actor.get() == actors[target];
		ASSERT_EQ(20, target_events);
		for (int frame = 0; frame < 20; ++frame) {
			SCOPED_TRACE(testing::Message() << "decimals=" << decimals << " target=" << target << " frame=" << frame);
			AssDialogue const *active = nullptr;
			int matches = 0;
			int const timestamp = frame * 100;
			for (auto const& line : reopened.Events) {
				if (line.Actor.get() == actors[target] && line.Start <= timestamp && timestamp < line.End) {
					active = &line;
					++matches;
				}
			}
			ASSERT_EQ(1, matches);
			ASSERT_NE(nullptr, active);
			EXPECT_EQ(frame == 0 ? 0 : frame * 100 - 50, active->Start);
			EXPECT_EQ(frame == 19 ? 1950 : frame * 100 + 50, active->End);
			EXPECT_EQ("precision-acceptance", active->Effect.get());
			EXPECT_EQ("Default", active->Style.get());
			std::string const text = active->Text.get();
			static std::regex const overrides(R"(\{([^}]*)\})");
			static std::regex const kept_tags(R"(\\pos\(-?[0-9]+(?:\.[0-9]+)?,-?[0-9]+(?:\.[0-9]+)?\)|\\(?:frz|fscx|fscy)-?[0-9]+(?:\.[0-9]+)?|\\b1)");
			for (auto block = std::sregex_iterator(text.begin(), text.end(), overrides); block != std::sregex_iterator(); ++block)
				EXPECT_EQ("", std::regex_replace((*block)[1].str(), kept_tags, "")) << text;
			EXPECT_EQ(bodies[target], std::regex_replace(text, overrides, ""));
			EXPECT_NE(std::string::npos, text.find(R"(\b1)"));
			EXPECT_EQ(std::string::npos, text.find(R"(\t()"));
			EXPECT_EQ(std::string::npos, text.find(R"(\move()"));
			auto const pose = ReadSerializedSimilarityPose(text);
			ASSERT_TRUE(pose.has_value()) << text;
			double const angle = 0.731 * frame * std::numbers::pi / 180.0;
			double const scale = 1.0 + 0.0137 * frame;
			double const expected_x = 1.127 * frame + scale * (base_x[target] * std::cos(angle) - base_y[target] * std::sin(angle));
			double const expected_y = -0.863 * frame + scale * (base_x[target] * std::sin(angle) + base_y[target] * std::cos(angle));
			double const position_tolerance = 0.5 * std::pow(10.0, -decimals) + 1e-9;
			EXPECT_NEAR(expected_x, pose->x, position_tolerance);
			EXPECT_NEAR(expected_y, pose->y, position_tolerance);
			EXPECT_NEAR(17.375 - 0.731 * frame, pose->rotation, 0.005 + 1e-9);
			EXPECT_NEAR(123.456 * scale, pose->scale_x, 0.005 + 1e-9);
			EXPECT_NEAR(78.912 * scale, pose->scale_y, 0.005 + 1e-9);
		}
	}
}

INSTANTIATE_TEST_SUITE_P(position_decimals, motion_track_similarity_serialized_precision,
						 testing::Values(0, 2, 6), [](testing::TestParamInfo<int> const& info) {
							 return "Decimals" + std::to_string(info.param);
						 });


TEST(motion_track_independent_ass_consumer, calibrates_known_geometry_and_event_relative_clock_fail_closed) {
	auto read = [](std::string const& text) { return IndependentAssReader(0, 0, 200).Read(text); };
	auto point = IndependentProjectDrawing(read(R"({\an7\pos(10,20)\p1}m 0 0 l 10 0 10 10)"), {.x = 5, .y = 7}, 1.0, 312.5);
	EXPECT_DOUBLE_EQ(15.0, point.x);
	EXPECT_DOUBLE_EQ(27.0, point.y);
	point = IndependentProjectDrawing(read(R"({\an7\pos(10,20)\fscx200\fscy50\fax0.25\fay-0.1\p1}m 0 0 l 10 0 10 10)"), {.x = 4, .y = 8}, 2.0, 312.5);
	EXPECT_NEAR(20.0, point.x, 1e-12);
	EXPECT_NEAR(23.6, point.y, 1e-12);
	point = IndependentProjectDrawing(read(R"({\an7\pos(10,20)\org(0,0)\frz90\p1}m 0 0 l 10 0 10 10)"), {.x = 0, .y = 0}, 2.0, 312.5);
	EXPECT_NEAR(10.0, point.x, 1e-12);
	EXPECT_NEAR(-20.0, point.y, 1e-12);
	point = IndependentProjectDrawing(read(R"({\an7\pos(0,0)\frx90\p1}m 0 0 l 10 0 10 10)"), {.x = 10, .y = 156.25}, 1.0, 312.5);
	EXPECT_NEAR(20.0, point.x, 1e-12);
	EXPECT_NEAR(0.0, point.y, 1e-12);
	point = IndependentProjectDrawing(read(R"({\an7\pos(0,0)\fry90\p1}m 0 0 l 10 0 10 10)"), {.x = 156.25, .y = 10}, 1.0, 312.5);
	EXPECT_NEAR(0.0, point.x, 1e-12);
	EXPECT_NEAR(20.0 / 3.0, point.y, 1e-12);
	auto const timed = IndependentAssReader(1060, 1000, 1200).Read(R"({\move(0,0,100,200,20,120)\frz10\t(20,120,2,\frz110)}timing)");
	EXPECT_DOUBLE_EQ(40.0, timed.position.x);
	EXPECT_DOUBLE_EQ(80.0, timed.position.y);
	EXPECT_NEAR(26.0, timed.channels[6], 1e-12);
	EXPECT_TRUE(timed.offset_window);
	std::string const sequential = R"({\pos(0,0)\frz10\t(0,100,\frz50)\t(100,200,\frz90)}sequential)";
	EXPECT_DOUBLE_EQ(30.0, IndependentAssReader(1050, 1000, 1300).Read(sequential).channels[6]);
	EXPECT_DOUBLE_EQ(70.0, IndependentAssReader(1150, 1000, 1300).Read(sequential).channels[6]);
	EXPECT_DOUBLE_EQ(90.0, IndependentAssReader(1250, 1000, 1300).Read(sequential).channels[6]);
	auto const reversed = IndependentAssReader(1150, 1000, 1300).Read(R"({\pos(0,0)\frz10\t(100,200,\frz90)\t(0,100,\frz50)}source-order)");
	EXPECT_DOUBLE_EQ(50.0, reversed.channels[6]);
	std::string const overlapping = R"({\pos(0,0)\frz10\fscx100\fscy80\t(0,100,\frz50\fscx140)\t(50,150,\fscy120)}overlapping)";
	auto const overlap_early = IndependentAssReader(1075, 1000, 1300).Read(overlapping);
	EXPECT_DOUBLE_EQ(40.0, overlap_early.channels[6]);
	EXPECT_DOUBLE_EQ(130.0, overlap_early.channels[0]);
	EXPECT_DOUBLE_EQ(90.0, overlap_early.channels[1]);
	auto const overlap_late = IndependentAssReader(1125, 1000, 1300).Read(overlapping);
	EXPECT_DOUBLE_EQ(50.0, overlap_late.channels[6]);
	EXPECT_DOUBLE_EQ(140.0, overlap_late.channels[0]);
	EXPECT_DOUBLE_EQ(110.0, overlap_late.channels[1]);
	EXPECT_THROW(read(R"({\pos(0,0)\p1}m 0 0 l 10 0 10 10)"), std::runtime_error);
	EXPECT_THROW(read(R"({\pos(0,0)\unknown3}unsupported)"), std::runtime_error);
	EXPECT_THROW(read(R"({\pos(0,0)\pos(1,1)}duplicate)"), std::runtime_error);
	EXPECT_THROW(read(R"({\pos(0,0)\frz1\fr2}ambiguous)"), std::runtime_error);
	EXPECT_THROW(read(R"({\pos(0,0)\t(0,100,\t(0,50,\frz2))}nested)"), std::runtime_error);
	EXPECT_THROW(read(R"({\pos(0,0)}body{\frz2}mixed)"), std::runtime_error);
}

TEST_P(motion_track_similarity_serialized_precision, compact_vfr_failed_hold_save_reload_preserves_every_sample_pose) {
	std::vector<int> const times{0, 43, 89, 151, 209, 267, 311, 367, 443, 499, 557, 601};
	input.timecodes = agi::vfr::Framerate(times);
	input.decode_interval = input.direction_domain = {.first = 0, .last = 10};
	input.video_frame_count = 11;
	input.options.position_decimals = GetParam();
	input.options.compact_epsilon = 1.0;
	std::array<std::string, 2> const actors{"vfr-first", "vfr-second"};
	std::array<std::string, 2> const bodies{"first, VFR\\Nretained", "second\\hVFR"};
	for (size_t target = 0; target < selection.size(); ++target) {
		selection[target]->Actor = actors[target];
		selection[target]->End = 580;
		selection[target]->Text = R"({\pos(0,0)\frz15\fscx120\fscy80\b1})" + bodies[target];
	}
	input.samples.clear();
	for (int frame = 0; frame <= 10; ++frame) {
		double const progress = std::pow(times[frame] / 557.0, 2.0);
		double const angle = 8.0 * progress * std::numbers::pi / 180.0;
		double const scale = 1.0 + 0.1 * progress;
		TrackSample sample;
		sample.frame = frame;
		sample.model = TrackModel::Similarity;
		sample.status = frame >= 4 && frame <= 6 ? TrackStatus::Failed : TrackStatus::Ok;
		sample.center_x = 0.2 * times[frame];
		sample.center_y = -0.04 * times[frame];
		sample.transform.matrix = {scale * std::cos(angle), -scale * std::sin(angle), 0,
								   scale * std::sin(angle), scale * std::cos(angle), 0, 0, 0, 1};
		input.samples.push_back(sample);
	}
	source.Capture(file, selection, input.timecodes, input.video_frame_count);
	auto const plan = Plan(ApplyMode::Compact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_TRUE(Apply(plan, input.timecodes));
	ASSERT_EQ(1, commits);
	SerializedPrecisionFile saved;
	WriteAssFileForCore(&file, saved.Path(), input.timecodes, "UTF-8", AssWriteOptions{});
	AssFile const reopened = ReadAssFileForCore(saved.Path(), "UTF-8");
	bool moved = false, animated = false, offset_window = false;
	int preserved = 0;
	int checked_poses = 0;
	for (auto const& line : reopened.Events) {
		if (line.Actor.get().empty()) {
			EXPECT_EQ(untouched->GetEntryData(), line.GetEntryData());
			++preserved;
		}
		else {
			ASSERT_TRUE(line.Actor.get() == actors[0] || line.Actor.get() == actors[1]);
			EXPECT_NO_THROW(IndependentAssReader(line.Start, line.Start, line.End).Read(line.Text.get()));
		}
	}
	EXPECT_EQ(1, preserved);
	for (size_t target = 0; target < actors.size(); ++target) {
		std::vector<AssDialogue const *> events;
		for (auto const& line : reopened.Events)
			if (line.Actor.get() == actors[target])
				events.push_back(&line);
		ASSERT_FALSE(events.empty());
		std::ranges::sort(events, {}, [](auto const *line) { return static_cast<int>(line->Start); });
		EXPECT_EQ(0, events.front()->Start);
		EXPECT_EQ(580, events.back()->End);
		for (size_t event = 1; event < events.size(); ++event) {
			int const boundary = events[event]->Start;
			EXPECT_EQ(events[event - 1]->End, boundary);
			EXPECT_EQ(events[event - 1], UniqueActiveEvent(reopened, actors[target], boundary - 1));
			EXPECT_EQ(events[event], UniqueActiveEvent(reopened, actors[target], boundary));
			EXPECT_NO_THROW(ReadActivePose(reopened, actors[target], boundary - 1));
			EXPECT_NO_THROW(ReadActivePose(reopened, actors[target], boundary));
			EXPECT_NO_THROW(ReadActivePose(reopened, actors[target], boundary + 1));
		}
		AssDialogue const *held_event = nullptr;
		for (int frame = 0; frame <= 10; ++frame) {
			SCOPED_TRACE(testing::Message() << "target=" << target << " frame=" << frame << " decimals=" << GetParam());
			auto const actual = ReadActivePose(reopened, actors[target], times[frame]);
			++checked_poses;
			EXPECT_EQ(bodies[target], actual.body);
			moved |= actual.moved;
			animated |= actual.animated;
			offset_window |= actual.offset_window;
			int const held_frame = frame >= 4 && frame <= 6 ? 3 : frame;
			double const progress = std::pow(times[held_frame] / 557.0, 2.0);
			EXPECT_LE(std::hypot(actual.position.x - 0.2 * times[held_frame], actual.position.y + 0.04 * times[held_frame]), input.options.compact_epsilon + 1e-8);
			EXPECT_NEAR(15.0 - 8.0 * progress, actual.channels[6], 0.05 + 1e-8);
			EXPECT_NEAR(120.0 * (1.0 + 0.1 * progress), actual.channels[0], 0.05 + 1e-8);
			EXPECT_NEAR(80.0 * (1.0 + 0.1 * progress), actual.channels[1], 0.05 + 1e-8);
			if (frame >= 4 && frame <= 6) {
				auto const *current = UniqueActiveEvent(reopened, actors[target], times[frame]);
				if (!held_event)
					held_event = current;
				EXPECT_EQ(held_event, current);
				EXPECT_FALSE(actual.moved);
				EXPECT_FALSE(actual.animated);
			}
		}
	}
	EXPECT_TRUE(moved);
	EXPECT_TRUE(animated);
	EXPECT_TRUE(offset_window);
	RecordProperty("checked_similarity_poses", checked_poses);
}

TEST_P(motion_track_geometry_serialized_precision, exact_apply_save_reload_preserves_every_sample_corner) {
	RunGeometry(ApplyMode::Exact);
}

TEST_P(motion_track_geometry_serialized_precision, compact_apply_save_reload_preserves_every_sample_corner) {
	RunGeometry(ApplyMode::Compact);
}

INSTANTIATE_TEST_SUITE_P(models_and_position_decimals, motion_track_geometry_serialized_precision,
						 testing::Combine(testing::Values(TrackModel::Affine, TrackModel::Homography), testing::Values(0, 2, 6)),
						 [](testing::TestParamInfo<std::tuple<TrackModel, int>> const& info) {
							 return std::string(std::get<0>(info.param) == TrackModel::Affine ? "Affine" : "Homography") +
									"Decimals" + std::to_string(std::get<1>(info.param));
						 });

class motion_track_geometry_atomic_serialized_precision : public motion_track_geometry_serialized_precision {};

TEST_P(motion_track_geometry_atomic_serialized_precision, rejects_both_targets_then_recovers_from_original_source) {
	auto const [model, compact] = GetParam();
	ConfigureGeometry(model);
	input.options.mode = compact ? ApplyMode::Compact : ApplyMode::Exact;
	input.options.position_decimals = 0;
	input.samples.resize(3);
	input.video_frame_count = 3;
	input.decode_interval = input.direction_domain = {.first = 0, .last = 2};
	for (auto *line : selection)
		line->End = 250;
	selection[1]->Text = R"({\an7\pos(200.4,150)\org(170,120)\frx12\fry-8\fscx125\fscy80\p1})" + drawings[1];
	for (int frame = 0; frame < 3; ++frame) {
		auto& sample = input.samples[frame];
		sample.transform.matrix = {1.0 + 0.01 * frame, 0.005 * frame, 0, 0, 1.0 - 0.005 * frame, 0,
								   model == TrackModel::Homography ? 0.00002 * frame : 0.0, 0, 1};
		sample.center_x = input.origin_center_x;
		sample.center_y = input.origin_center_y;
	}
	source.Capture(file, selection, input.timecodes, input.video_frame_count);
	std::array<IndependentAssPose, 2> const baseline{
		ReadActivePose(file, actors[0], 0), ReadActivePose(file, actors[1], 0)};
	auto const before = Entries();
	auto const selected_before = selection;
	AssFile single(file);
	AssDialogue *first = nullptr;
	for (auto& line : single.Events)
		if (line.Actor.get() == actors[0])
			first = &line;
	ASSERT_NE(nullptr, first);
	MotionTrackApplySource first_source;
	first_source.Capture(single, {first}, input.timecodes, input.video_frame_count);
	auto const first_plan = BuildApplyPlan(single, first_source.Targets(), input);
	ASSERT_TRUE(first_plan.has_mutations()) << first_plan.message;
	int first_commits = 0;
	std::string message;
	auto first_prepared = first_source.Prepare(single, first_plan, input.timecodes, message);
	ASSERT_NE(nullptr, first_prepared) << message;
	int const type = first_prepared->Candidate().PrepareCommit(AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_TIME);
	first_prepared->Finalize(input.timecodes);
	first_prepared->Install();
	int revision = -1;
	single.PublishCommit({.message = "apply", .commit_id = &revision, .single_line = nullptr, .type = type, .changed_lines = {}}, false);
	++first_commits;
	EXPECT_EQ(1, first_commits);
	auto const rejected = BuildApplyPlan(file, source.Targets(), input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, rejected.status);
	EXPECT_FALSE(rejected.has_mutations());
	EXPECT_EQ(nullptr, source.Prepare(file, rejected, input.timecodes, message));
	EXPECT_EQ(before, Entries());
	EXPECT_EQ(selected_before, selection);
	EXPECT_EQ(0, commits);
	EXPECT_TRUE(source.ContinueTargetsMatch(selection));
	EXPECT_NE(std::string::npos, message.find("Position decimals"));
	input.options.position_decimals = 3;
	auto const recovered = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(recovered.has_mutations()) << recovered.message;
	ASSERT_EQ(2u, recovered.lines.size());
	ASSERT_TRUE(Apply(recovered, input.timecodes));
	EXPECT_EQ(1, commits);
	SerializedPrecisionFile saved;
	WriteAssFileForCore(&file, saved.Path(), input.timecodes, "UTF-8", AssWriteOptions{});
	AssFile const reopened = ReadAssFileForCore(saved.Path(), "UTF-8");
	ExpectSerializedCoverage(reopened, 250);
	ExpectGeometry(reopened, compact ? input.options.compact_epsilon : 0.05, baseline);
}

INSTANTIATE_TEST_SUITE_P(models_and_modes, motion_track_geometry_atomic_serialized_precision,
						 testing::Combine(testing::Values(TrackModel::Affine, TrackModel::Homography), testing::Values(0, 1)),
						 [](testing::TestParamInfo<std::tuple<TrackModel, int>> const& info) {
							 return std::string(std::get<0>(info.param) == TrackModel::Affine ? "Affine" : "Homography") +
									(std::get<1>(info.param) ? "Compact" : "Exact");
						 });

TEST_F(motion_track_apply_source, exact_preparation_exception_preserves_original_source_and_allows_retry) {
	auto const plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(2u, plan.lines.size());
	ASSERT_EQ(40u, plan.event_count);
	auto const entries = Entries();
	auto const identities = EventIdentities();
	auto const selected = selection;
	auto const snapshot = source.Snapshot();
	auto const baseline_targets = source.Targets();
	int attempts = 0;
	auto reject_preparation = [&](AssFile& candidate) {
		++attempts;
		EXPECT_NE(&file, &candidate);
		EXPECT_EQ(41u, candidate.Events.size());
		throw std::runtime_error("candidate preparation failed");
	};

	EXPECT_THROW(Apply(plan, input.timecodes, reject_preparation), std::runtime_error);
	EXPECT_EQ(1, attempts);
	EXPECT_EQ(0, commits);
	EXPECT_EQ(entries, Entries());
	ExpectSourceSnapshot(snapshot);
	ASSERT_EQ(identities, EventIdentities());
	EXPECT_EQ(selected, selection);
	EXPECT_EQ(baseline_targets, source.Targets());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, snapshot, input.timecodes));
	EXPECT_TRUE(source.ContinueTargetsMatch(selection));

	ASSERT_TRUE(Apply(plan, input.timecodes));
	EXPECT_EQ(1, commits);
	ASSERT_EQ(40u, selection.size());
	EXPECT_EQ(41u, Entries().size());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	std::size_t selected_index = 0;
	for (auto const& line : plan.lines)
		for (auto const& part : line.parts) {
			auto const *output = selection[selected_index++];
			EXPECT_EQ(part.text, output->Text.get());
			EXPECT_EQ(part.start_ms, output->Start);
			EXPECT_EQ(part.end_ms, output->End);
		}
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));
}

TEST_F(motion_track_apply_source, reapply_preparation_exception_preserves_last_apply_and_allows_retry) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Exact), input.timecodes));
	ASSERT_EQ(40u, selection.size());
	auto const plan = Plan(ApplyMode::Compact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(2u, plan.lines.size());
	ASSERT_EQ(2u, plan.event_count);
	auto const entries = Entries();
	auto const identities = EventIdentities();
	auto const selected = selection;
	auto const snapshot = source.Snapshot();
	auto const baseline_targets = source.Targets();
	int attempts = 0;
	auto reject_preparation = [&](AssFile& candidate) {
		++attempts;
		EXPECT_NE(&file, &candidate);
		EXPECT_EQ(3u, candidate.Events.size());
		throw std::runtime_error("candidate reapply preparation failed");
	};

	EXPECT_THROW(Apply(plan, input.timecodes, reject_preparation), std::runtime_error);
	EXPECT_EQ(1, attempts);
	EXPECT_EQ(1, commits);
	EXPECT_EQ(entries, Entries());
	ExpectSourceSnapshot(snapshot);
	ASSERT_EQ(identities, EventIdentities());
	EXPECT_EQ(selected, selection);
	EXPECT_EQ(baseline_targets, source.Targets());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, snapshot, input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));

	ASSERT_TRUE(Apply(plan, input.timecodes));
	EXPECT_EQ(2, commits);
	ASSERT_EQ(2u, selection.size());
	EXPECT_EQ(3u, Entries().size());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	for (std::size_t index = 0; index < plan.lines.size(); ++index) {
		ASSERT_EQ(1u, plan.lines[index].parts.size());
		auto const& part = plan.lines[index].parts.front();
		EXPECT_EQ(part.text, selection[index]->Text.get());
		EXPECT_EQ(part.start_ms, selection[index]->Start);
		EXPECT_EQ(part.end_ms, selection[index]->End);
	}
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));
}

TEST_F(motion_track_apply_source, compact_exact_round_trip_reuses_baseline_and_preserves_other_lines) {
	auto compact = Plan(ApplyMode::Compact);
	ASSERT_TRUE(compact.has_mutations()) << compact.message;
	ASSERT_EQ(2u, compact.event_count);
	ASSERT_TRUE(Apply(compact, input.timecodes));
	auto const compact_entries = Entries();
	ASSERT_EQ(3u, compact_entries.size());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));

	auto exact = Plan(ApplyMode::Exact);
	ASSERT_TRUE(exact.has_mutations()) << exact.message;
	ASSERT_EQ(40u, exact.event_count);
	ASSERT_TRUE(Apply(exact, input.timecodes));
	ASSERT_EQ(40u, selection.size());
	EXPECT_EQ(41u, Entries().size());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_EQ(0, selection.front()->Row);
	EXPECT_EQ(40, selection.back()->Row);
	for (int frame : {0, 9, 19}) {
		auto *line = selection[frame];
		auto const state = perspective::EvaluateEffectiveAssState({.file = &file,
																   .line = line,
																   .play_resolution = {.width = 1920.0, .height = 1080.0},
																   .capture_time_ms = input.timecodes.TimeAtFrame(frame)});
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(2.0 * frame, state.value.transform.position.x, 1e-6);
		EXPECT_NEAR(15.0 - 0.5 * frame, state.value.transform.rotation_z, 1e-6);
		EXPECT_NEAR(120.0 * (1.0 + 0.01 * frame), state.value.transform.scale_x, 1e-6);
		EXPECT_NEAR(80.0 * (1.0 + 0.01 * frame), state.value.transform.scale_y, 1e-6);
	}
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	EXPECT_EQ(compact_entries, Entries());
	EXPECT_EQ(2u, selection.size());
	EXPECT_EQ(3, commits);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
}

TEST_F(motion_track_apply_source, hand_edit_after_plan_preparation_rejects_reapply_without_mutation) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	selection.front()->Text = "edited after preview";
	auto const edited_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(edited_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, undo_replacement_rejects_reapply_without_mutation) {
	AssFile original(file);
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	file = original;
	selection.clear();
	auto const restored_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(restored_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, cleared_source_rejects_pending_plan_without_accessing_old_baseline) {
	auto plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations());
	auto const entries = Entries();
	source.Clear();
	EXPECT_FALSE(Apply(plan, input.timecodes));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(0, commits);
}

TEST_F(motion_track_apply_source, changed_style_or_timecodes_rejects_reapply) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	auto const entries = Entries();
	auto *style = file.GetStyle("Default");
	ASSERT_NE(nullptr, style);
	double const previous_scale = style->scalex;
	style->scalex = previous_scale + 10;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	style->scalex = previous_scale;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, agi::vfr::Framerate(20.0)));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(1, commits);
}
