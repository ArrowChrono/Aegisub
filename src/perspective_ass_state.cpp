#include "perspective_ass_state.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_font_state.h"
#include "ass_style.h"
#include "ass_style_resolution.h"
#include "ass_tag_scanner.h"

#include <libaegisub/string_utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace perspective {
namespace {

struct WorkingState {
	EvaluatedTransformState transform;
	AssStyle const* style = nullptr;
	bool style_valid = true;
	bool position_explicit = false;
	int border_style = 1;
	bool drawing_mode = false;
	int drawing_scale = 0;
	double drawing_baseline_offset = 0.0;
	int event_wrap_style = 0;
	std::string font_name;
	double font_size = 0.0;
	double spacing = 0.0;
	int font_weight = 400;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	int encoding = 1;
	int wrap_style = 0;
	double available_wrap_width = 0.0;
};

struct RawTag {
	std::size_t begin = 0;
	std::size_t end = 0;
	std::string raw;
	std::string name;
	bool valid = false;
	bool contains_move = false;
	bool geometry_animation = false;
	bool named_reset = false;
};

struct OverrideSpan {
	std::size_t begin = 0;
	std::size_t end = 0;
};

struct ParsedOverrideSpan {
	OverrideSpan span;
	std::vector<RawTag> tags;
};

bool IsEscapedBrace(std::string_view text, std::size_t position);

bool GapHasVisibleText(std::string_view gap) {
	for (std::size_t position = 0; position < gap.size(); ++position) {
		if (gap[position] == '{' && !IsEscapedBrace(gap, position)) {
			auto const close = gap.find('}', position + 1);
			if (close != std::string_view::npos) {
				position = close;
				continue;
			}
		}
		return true;
	}
	return false;
}

bool FinitePositive(double value) {
	return std::isfinite(value) && value > 0.0 && value <= MaxAbsCoordinate;
}

bool NearlyEqual(double left, double right) {
	double const scale = std::max({1.0, std::abs(left), std::abs(right)});
	return std::abs(left - right) <= 1.0e-9 * scale;
}

bool EquivalentRotation(double left, double right) {
	return std::abs(std::remainder(left - right, 360.0)) <= 1.0e-9;
}

bool SamePoint(Vec2 left, Vec2 right) {
	return NearlyEqual(left.x, right.x) && NearlyEqual(left.y, right.y);
}

bool SameOptionalPoint(std::optional<Vec2> const& left, std::optional<Vec2> const& right) {
	if (left.has_value() != right.has_value())
		return false;
	return !left || SamePoint(*left, *right);
}

bool SameTransform(EvaluatedTransformState const& left, EvaluatedTransformState const& right) {
	return left.alignment == right.alignment
		&& SamePoint(left.position, right.position)
		&& SameOptionalPoint(left.origin, right.origin)
		&& NearlyEqual(left.scale_x, right.scale_x)
		&& NearlyEqual(left.scale_y, right.scale_y)
		&& NearlyEqual(left.shear_x, right.shear_x)
		&& NearlyEqual(left.shear_y, right.shear_y)
		&& NearlyEqual(left.rotation_x, right.rotation_x)
		&& NearlyEqual(left.rotation_y, right.rotation_y)
		&& NearlyEqual(left.rotation_z, right.rotation_z)
		&& NearlyEqual(left.outline_x, right.outline_x)
		&& NearlyEqual(left.outline_y, right.outline_y)
		&& NearlyEqual(left.shadow_x, right.shadow_x)
		&& NearlyEqual(left.shadow_y, right.shadow_y);
}

bool SameRunState(WorkingState const& left, WorkingState const& right) {
	return SameTransform(left.transform, right.transform)
		&& left.border_style == right.border_style
		&& left.drawing_mode == right.drawing_mode
		&& left.drawing_scale == right.drawing_scale
		&& NearlyEqual(left.drawing_baseline_offset, right.drawing_baseline_offset)
		&& left.font_name == right.font_name
		&& NearlyEqual(left.font_size, right.font_size)
		&& NearlyEqual(left.spacing, right.spacing)
		&& left.font_weight == right.font_weight
		&& left.italic == right.italic
		&& left.underline == right.underline
		&& left.strikeout == right.strikeout
		&& left.encoding == right.encoding
		&& left.wrap_style == right.wrap_style
		&& NearlyEqual(left.available_wrap_width, right.available_wrap_width);
}

bool FiniteTransform(EvaluatedTransformState const& state) {
	if (state.alignment < 1 || state.alignment > 9)
		return false;
	for (double const value : {
		state.position.x, state.position.y,
		state.scale_x, state.scale_y,
		state.shear_x, state.shear_y,
		state.rotation_x, state.rotation_y, state.rotation_z,
		state.outline_x, state.outline_y,
		state.shadow_x, state.shadow_y,
	}) {
		if (!std::isfinite(value) || std::abs(value) > MaxAbsCoordinate)
			return false;
	}
	return !state.origin || (std::isfinite(state.origin->x)
		&& std::isfinite(state.origin->y)
		&& std::abs(state.origin->x) <= MaxAbsCoordinate
		&& std::abs(state.origin->y) <= MaxAbsCoordinate);
}

std::array<int, 3> ResolvedMargins(
	AssDialogue const& line,
	AssStyle const& style) {
	auto margins = line.Margin;
	for (std::size_t index = 0; index < margins.size(); ++index) {
		if (margins[index] == 0)
			margins[index] = style.Margin[index];
	}
	return margins;
}

Vec2 DefaultPosition(
	AssDialogue const& line,
	AssStyle const& style,
	Resolution play_resolution,
	int alignment) {
	auto const margins = ResolvedMargins(line, style);

	double x = 0.0;
	if (alignment % 3 == 1)
		x = margins[0];
	else if (alignment % 3 == 2)
		x = (play_resolution.width + margins[0] - margins[1]) / 2.0;
	else
		x = play_resolution.width - margins[1];

	double y = 0.0;
	if (alignment <= 3)
		y = play_resolution.height - margins[2];
	else if (alignment <= 6)
		y = play_resolution.height / 2.0;
	else
		y = margins[2];
	return {x, y};
}

AssStateError ResetToStyle(
	WorkingState& state,
	AssStyle const& style,
	AssDialogue const& line,
	Resolution play_resolution,
	std::int64_t capture_time_ms,
	bool preserve_line_wide) {
	if (style.alignment < 1 || style.alignment > 9)
		return AssStateError::InvalidAlignment;
	for (double const value : {
		style.scalex, style.scaley, style.angle, style.outline_w, style.shadow_w,
		style.fontsize, style.spacing,
	}) {
		if (!std::isfinite(value) || std::abs(value) > MaxAbsCoordinate)
			return AssStateError::NonFiniteStyle;
	}
	if (style.fontsize <= 0.0)
		return AssStateError::NonFiniteStyle;

	auto const line_alignment = state.transform.alignment;
	auto const line_position = state.transform.position;
	auto const line_origin = state.transform.origin;
	bool const line_position_explicit = state.position_explicit;
	state.style = &style;
	state.style_valid = true;
	state.position_explicit = false;
	state.font_name = style.font;
	state.font_size = style.fontsize;
	state.spacing = style.spacing;
	state.font_weight = style.bold ? 700 : 400;
	state.italic = style.italic;
	state.underline = style.underline;
	state.strikeout = style.strikeout;
	state.encoding = style.encoding;
	auto const margins = ResolvedMargins(line, style);
	state.available_wrap_width = play_resolution.width
		- static_cast<double>(margins[0])
		- static_cast<double>(margins[1]);
	state.transform = {};
	state.transform.event_time_ms = capture_time_ms;
	state.transform.alignment = style.alignment;
	state.transform.position = DefaultPosition(line, style, play_resolution, style.alignment);
	state.transform.scale_x = style.scalex;
	state.transform.scale_y = style.scaley;
	state.transform.rotation_z = style.angle;
	state.border_style = style.borderstyle;
	state.transform.outline_x = std::max(0.0, style.outline_w);
	state.transform.outline_y = std::max(0.0, style.outline_w);
	state.transform.shadow_x = std::max(0.0, style.shadow_w);
	state.transform.shadow_y = std::max(0.0, style.shadow_w);
	if (preserve_line_wide) {
		state.transform.alignment = line_alignment;
		state.transform.position = line_position;
		state.transform.origin = line_origin;
		state.position_explicit = line_position_explicit;
	}
	return FiniteTransform(state.transform) ? AssStateError::None : AssStateError::NonFiniteStyle;
}

std::optional<double> NumberParameter(AssOverrideTag const& tag, std::size_t index) {
	if (index >= tag.Params.size() || tag.Params[index].omitted || tag.Params[index].empty)
		return std::nullopt;
	double value = 0.0;
	if (!AssCompat::ParseFloat(tag.Params[index].Get<std::string>(), value)
		|| !std::isfinite(value) || std::abs(value) > MaxAbsCoordinate)
		return std::nullopt;
	return value;
}

std::optional<int> IntegerParameter(AssOverrideTag const& tag, std::size_t index) {
	if (index >= tag.Params.size() || tag.Params[index].omitted || tag.Params[index].empty)
		return std::nullopt;
	int value = 0;
	if (!AssCompat::ParseInteger(tag.Params[index].Get<std::string>(), value))
		return std::nullopt;
	return value;
}

std::optional<int> AssAlignment(AssOverrideTag const& tag, int style_alignment) {
	if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty)
		return style_alignment;
	int value = 0;
	if (!AssCompat::ParseInteger(tag.Params.front().Get<std::string>(), value))
		return std::nullopt;
	if (tag.Name == "\\a")
		return AssCompat::NormalizeLegacyAssAlignment(value, style_alignment);
	return value >= 1 && value <= 9 ? std::optional<int>(value) : std::nullopt;
}

bool TagContainsGeometryAnimation(AssOverrideTag const& tag);

// Re-splits an override block's body with the libass-faithful scanner
// (aegisub::ass_tag_scanner) and re-parses every tag span through the
// prototype table. AssOverrideTag's own span cut classifies by prefix
// without skipping spaces after the backslash, so "{\ frz30}" lands as
// junk there while libass renders frz 30; the canonical form -- backslash,
// name, parenthesized arguments -- restores the classification without
// touching the argument bytes.
// Recanonicalizes the nested tag list inside a \t argument region, keeping
// every non-tag byte verbatim. libass parses the nested list with the same
// space-skipping rule as the top level (ass_parse.c), so the prototype
// table's lazy AssDialogueBlockOverride::ParseTags only agrees with it when
// the nested bytes are already canonical -- without this,
// "\t(0,4000,\ frz30)" animated the line for libass while the evaluator and
// the animation check read the body as junk and let the line through as
// static.
std::string CanonicalizeTagRegion(std::string_view region, int depth);

std::string CanonicalizeTagText(aegisub::ass_tag_scanner::RawTag const& scanned, int depth) {
	// The scanner ends the name only at '(' or '\', so the bytes between the
	// tag name and its argument region ride along: "\t (0,...)" scans as
	// "t " and "\t1(0,...)" as "t1". libass matches the tag by prefix
	// (mystrcmp) and renders both exactly like "\t(...)", so classification
	// and recursion must use the same prefix rule and the canonical spelling
	// must fold the junk away -- otherwise the nested body of "\t (0,4000,\
	// frz30)" reaches the prototype table still space-blind.
	bool const transform = aegisub::ass_tag_scanner::NameHasPrefix(scanned.name, "t");
	std::string canonical;
	canonical.reserve(1 + scanned.name.size() + (scanned.has_paren ? scanned.args.size() + 2 : 0));
	canonical += '\\';
	if (transform && scanned.has_paren)
		canonical += 't';
	else
		canonical += scanned.name;
	if (scanned.has_paren) {
		canonical += '(';
		if (transform)
			canonical += CanonicalizeTagRegion(scanned.args, depth);
		else
			canonical += scanned.args;
		canonical += ')';
	}
	return canonical;
}

std::string CanonicalizeTagRegion(std::string_view region, int depth) {
	std::string result;
	result.reserve(region.size());
	std::size_t cursor = 0;
	if (depth < 32) {
		aegisub::ass_tag_scanner::ScanRawTags(region,
											  [&](aegisub::ass_tag_scanner::RawTag const& scanned) {
												  auto const begin = static_cast<std::size_t>(
													  scanned.bytes.data() - region.data());
												  auto const end = begin + scanned.bytes.size();
												  result.append(region.substr(cursor, begin - cursor));
												  result += CanonicalizeTagText(scanned, depth + 1);
												  cursor = end;
											  });
	}
	result.append(region.substr(cursor));
	return result;
}

void CanonicalOverrideTags(std::string_view body,
						   std::vector<AssOverrideTag>& out) {
	aegisub::ass_tag_scanner::ScanRawTags(body,
										  [&](aegisub::ass_tag_scanner::RawTag const& scanned) {
											  out.emplace_back(CanonicalizeTagText(scanned, 0));
										  });
}

bool TransformContainsMove(AssDialogueBlockOverride const& block, int depth = 0) {
	if (depth >= 32)
		return false;
	for (auto const& tag : block.Tags) {
		if (tag.Name == "\\move")
			return true;
		if (tag.Name != "\\t" || tag.Params.empty())
			continue;
		auto const& parameter = tag.Params.back();
		if (parameter.omitted || parameter.empty
			|| parameter.GetType() != VariableDataType::BLOCK)
			continue;
		if (TransformContainsMove(
			*parameter.Get<AssDialogueBlockOverride*>(), depth + 1))
			return true;
	}
	return false;
}

bool TagContainsMove(AssOverrideTag const& tag) {
	if (tag.Name == "\\move")
		return true;
	if (tag.Name != "\\t" || tag.Params.empty())
		return false;
	auto const& parameter = tag.Params.back();
	return !parameter.omitted && !parameter.empty
		&& parameter.GetType() == VariableDataType::BLOCK
		&& TransformContainsMove(
			*parameter.Get<AssDialogueBlockOverride*>(), 0);
}

struct LineWideTags {
	AssOverrideTag const* position = nullptr;
	bool position_is_move = false;
	AssOverrideTag const* origin = nullptr;
	AssOverrideTag const* alignment = nullptr;
};

void MarkApplyBlocker(AssApplyBlocker& blocker, AssApplyBlocker value) {
	auto const priority = [](AssApplyBlocker candidate) {
		switch (candidate) {
			case AssApplyBlocker::None: return 0;
			case AssApplyBlocker::UnsupportedNamedReset: return 1;
			case AssApplyBlocker::UnsupportedGeometryAnimation: return 2;
			case AssApplyBlocker::UnsupportedMove: return 3;
		}
		return 0;
	};
	if (priority(value) > priority(blocker))
		blocker = value;
}

bool IsNamedReset(AssOverrideTag const& tag) {
	if (tag.Name != "\\r" || tag.Params.empty())
		return false;
	auto const& parameter = tag.Params.front();
	return !parameter.omitted && !parameter.empty
		&& parameter.GetType() == VariableDataType::TEXT
		&& !parameter.Get<std::string>().empty();
}

void ScanLineWideTags(
	std::vector<AssOverrideTag> const& tags,
	LineWideTags& line_wide,
	AssApplyBlocker& blocker,
	int depth = 0) {
	if (depth >= 32) {
		MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedGeometryAnimation);
		return;
	}
	for (auto const& tag : tags) {
		if (tag.Name == "\\move") {
			MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedMove);
			if (!line_wide.position) {
				line_wide.position = &tag;
				line_wide.position_is_move = true;
			}
		}
		else if (tag.Name == "\\pos") {
			if (!line_wide.position) {
				line_wide.position = &tag;
				line_wide.position_is_move = false;
			}
		}
		else if (tag.Name == "\\org") {
			if (!line_wide.origin)
				line_wide.origin = &tag;
		}
		else if (tag.Name == "\\an" || tag.Name == "\\a") {
			if (!line_wide.alignment)
				line_wide.alignment = &tag;
		}
		else if (tag.Name == "\\t") {
			if (TagContainsGeometryAnimation(tag))
				MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedGeometryAnimation);
			if (!tag.Params.empty()) {
				auto const& parameter = tag.Params.back();
				if (!parameter.omitted && !parameter.empty
					&& parameter.GetType() == VariableDataType::BLOCK)
					ScanLineWideTags(
						parameter.Get<AssDialogueBlockOverride*>()->Tags,
						line_wide, blocker, depth + 1);
			}
		}
	}
}

std::optional<double> TransformPower(
	AssOverrideTag const& tag,
	std::int64_t event_time_ms,
	std::int64_t event_duration_ms) {
	if (tag.Name != "\\t" || tag.Params.empty())
		return std::nullopt;
	auto const& params = tag.Params;
	std::int64_t start = 0;
	std::int64_t end = event_duration_ms;
	double acceleration = 1.0;
	if (params.size() >= 2 && !params[0].omitted && !params[1].omitted) {
		auto const first = IntegerParameter(tag, 0);
		auto const last = IntegerParameter(tag, 1);
		if (!first || !last)
			return std::nullopt;
		start = *first;
		end = *last;
		if (params.size() >= 3 && !params[2].omitted && !params[2].empty) {
			auto const value = NumberParameter(tag, 2);
			if (!value)
				return std::nullopt;
			acceleration = *value;
		}
	}
	else if (params.size() >= 3 && !params[2].omitted && !params[2].empty) {
		auto const value = NumberParameter(tag, 2);
		if (!value)
			return std::nullopt;
		acceleration = *value;
	}
	if (!std::isfinite(acceleration) || acceleration < 0.0)
		return std::nullopt;
	if (end == 0)
		end = event_duration_ms;
	double progress = 0.0;
	if (event_time_ms < start)
		progress = 0.0;
	else if (event_time_ms >= end)
		progress = 1.0;
	else if (end != start)
		progress = static_cast<double>(event_time_ms - start)
			/ static_cast<double>(end - start);
	progress = std::pow(progress, acceleration);
	return std::isfinite(progress) ? std::optional<double>(progress) : std::nullopt;
}

std::optional<Vec2> MovePosition(
	AssOverrideTag const& tag,
	std::int64_t event_time_ms,
	std::int64_t event_duration_ms) {
	auto const x1 = NumberParameter(tag, 0);
	auto const y1 = NumberParameter(tag, 1);
	auto const x2 = NumberParameter(tag, 2);
	auto const y2 = NumberParameter(tag, 3);
	if (!x1 || !y1 || !x2 || !y2)
		return std::nullopt;
	std::int64_t start = 0;
	std::int64_t end = event_duration_ms;
	if (tag.Params.size() >= 6
		&& !tag.Params[4].omitted && !tag.Params[5].omitted) {
		auto const first = IntegerParameter(tag, 4);
		auto const last = IntegerParameter(tag, 5);
		if (!first || !last)
			return std::nullopt;
		start = *first;
		end = *last;
		if (start > end)
			std::swap(start, end);
	}
	else if (tag.Params.size() >= 6
		&& (!tag.Params[4].omitted || !tag.Params[5].omitted)) {
		return std::nullopt;
	}
	if (start <= 0 && end <= 0) {
		start = 0;
		end = event_duration_ms;
	}
	double progress = 0.0;
	if (event_time_ms <= start)
		progress = 0.0;
	else if (event_time_ms >= end)
		progress = 1.0;
	else if (end != start)
		progress = static_cast<double>(event_time_ms - start)
			/ static_cast<double>(end - start);
	return Vec2 {
		*x1 + progress * (*x2 - *x1),
		*y1 + progress * (*y2 - *y1),
	};
}

AssStateError ApplyLineWideTags(
	WorkingState& state,
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	AssDialogue const& line,
	AssStyle const& event_style,
	Resolution play_resolution,
	std::int64_t event_time_ms,
	std::int64_t event_duration_ms,
	AssApplyBlocker& blocker) {
	LineWideTags line_wide;
	// One combined canonical list across all override blocks: the scan is
	// textual-order first-wins, and line_wide holds pointers into it.
	std::vector<AssOverrideTag> canonical;
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		CanonicalOverrideTags(
			static_cast<AssDialogueBlockOverride const&>(*block).GetRawText(),
			canonical);
	}
	ScanLineWideTags(canonical, line_wide, blocker);

	if (line_wide.alignment) {
		auto const value = AssAlignment(*line_wide.alignment, event_style.alignment);
		if (!value)
			return AssStateError::InvalidAlignment;
		state.transform.alignment = *value;
	}
	if (line_wide.position) {
		std::optional<Vec2> position;
		if (line_wide.position_is_move)
			position = MovePosition(*line_wide.position, event_time_ms, event_duration_ms);
		else
			position = [&]() -> std::optional<Vec2> {
				auto const x = NumberParameter(*line_wide.position, 0);
				auto const y = NumberParameter(*line_wide.position, 1);
				return x && y ? std::optional<Vec2>(Vec2 {*x, *y}) : std::nullopt;
			}();
		if (!position)
			return AssStateError::InvalidGeometryParameter;
		state.transform.position = *position;
		state.position_explicit = true;
	}
	else {
		state.transform.position = DefaultPosition(
			line, event_style, play_resolution, state.transform.alignment);
	}
	if (line_wide.origin) {
		auto const x = NumberParameter(*line_wide.origin, 0);
		auto const y = NumberParameter(*line_wide.origin, 1);
		if (!x || !y)
			return AssStateError::InvalidGeometryParameter;
		state.transform.origin = Vec2 {*x, *y};
	}
	return FiniteTransform(state.transform)
		? AssStateError::None : AssStateError::InvalidGeometryParameter;
}

int NormalizeFontWeight(int value, int baseline) {
	return aegisub::ass::NormalizeVsfilterAssWeight(value, baseline);
}

bool IsGeometryAnimationTag(std::string const& name) {
	return name == "\\pos" || name == "\\move" || name == "\\org"
		|| name == "\\an" || name == "\\a" || name == "\\r"
		|| name == "\\fsc" || name == "\\fscx" || name == "\\fscy"
		|| name == "\\fax" || name == "\\fay"
		|| name == "\\fr" || name == "\\frz" || name == "\\frx" || name == "\\fry"
		|| name == "\\bord" || name == "\\xbord" || name == "\\ybord"
		|| name == "\\shad" || name == "\\xshad" || name == "\\yshad"
		|| name == "\\fs" || name == "\\fsp" || name == "\\fn"
		|| name == "\\b" || name == "\\i" || name == "\\u" || name == "\\s"
		|| name == "\\fe" || name == "\\q"
		|| name == "\\p" || name == "\\pbo";
}

bool TransformContainsGeometry(AssDialogueBlockOverride const& block, int depth = 0) {
	if (depth >= 32)
		return true;
	for (auto const& tag : block.Tags) {
		if (IsGeometryAnimationTag(tag.Name))
			return true;
		if (tag.Name != "\\t" || tag.Params.empty())
			continue;
		auto const& parameter = tag.Params.back();
		if (parameter.omitted || parameter.empty || parameter.GetType() != VariableDataType::BLOCK)
			continue;
		if (TransformContainsGeometry(*parameter.Get<AssDialogueBlockOverride*>(), depth + 1))
			return true;
	}
	return false;
}

bool TagContainsGeometryAnimation(AssOverrideTag const& tag) {
	if (tag.Name != "\\t" || tag.Params.empty())
		return false;
	auto const& parameter = tag.Params.back();
	return !parameter.omitted && !parameter.empty
		&& parameter.GetType() == VariableDataType::BLOCK
		&& TransformContainsGeometry(*parameter.Get<AssDialogueBlockOverride*>(), 0);
}

double Blend(double current, double target, double power) {
	return current * (1.0 - power) + target * power;
}

AssStateError ApplyTag(
	WorkingState& state,
	AssOverrideTag const& tag,
	AssFile const& file,
	AssDialogue const& line,
	AssStyle const& event_style,
	Resolution play_resolution,
	std::int64_t event_time_ms,
	std::int64_t event_duration_ms,
	double animation_power,
	AssApplyBlocker& blocker,
	int depth = 0) {
	if (!tag.IsValid())
		return AssStateError::None;
	if (IsNamedReset(tag))
		MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedNamedReset);
	if (tag.Name == "\\move") {
		MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedMove);
		return AssStateError::None;
	}
	if (tag.Name == "\\t") {
		bool const geometry_animation = TagContainsGeometryAnimation(tag);
		if (geometry_animation)
			MarkApplyBlocker(blocker, AssApplyBlocker::UnsupportedGeometryAnimation);
		if (tag.Params.empty())
			return AssStateError::None;
		auto const& parameter = tag.Params.back();
		if (parameter.omitted || parameter.empty
			|| parameter.GetType() != VariableDataType::BLOCK)
			return AssStateError::None;
		if (depth >= 32)
			return geometry_animation
				? AssStateError::InvalidGeometryParameter : AssStateError::None;
		auto const power = TransformPower(tag, event_time_ms, event_duration_ms);
		if (!power)
			return geometry_animation
				? AssStateError::InvalidGeometryParameter : AssStateError::None;
		for (auto const& nested : parameter.Get<AssDialogueBlockOverride*>()->Tags) {
			auto const error = ApplyTag(
				state, nested, file, line, event_style, play_resolution,
				event_time_ms, event_duration_ms, *power, blocker, depth + 1);
			if (error != AssStateError::None)
				return error;
		}
		return AssStateError::None;
	}
	if (tag.Name == "\\pos" || tag.Name == "\\org"
		|| tag.Name == "\\an" || tag.Name == "\\a")
		return AssStateError::None;
	if (!state.style_valid && tag.Name != "\\r")
		return AssStateError::None;

	if (tag.Name == "\\r") {
		AssStyle const* reset_style = &event_style;
		if (!tag.Params.empty() && !tag.Params.front().omitted && !tag.Params.front().empty) {
			auto const name = tag.Params.front().Get<std::string>();
			if (!name.empty())
				reset_style = aegisub::ass_style_resolution::ResolveResetStyle(file, name);
		}
		if (!reset_style) {
			state.style_valid = false;
			return AssStateError::None;
		}
		return ResetToStyle(
			state, *reset_style, line, play_resolution, event_time_ms, true);
	}

	auto number_or_style = [&](std::size_t index, double style_default) -> std::optional<double> {
		if (index >= tag.Params.size() || tag.Params[index].omitted)
			return std::nullopt;
		if (tag.Params[index].empty)
			return style_default;
		return NumberParameter(tag, index);
	};
	auto number = [&](std::size_t index) { return NumberParameter(tag, index); };
	auto has_explicit_number = [&](std::size_t index) {
		return index < tag.Params.size() && !tag.Params[index].omitted
			&& !tag.Params[index].empty;
	};

	if (tag.Name == "\\fsc") {
		state.transform.scale_x = std::max(0.0, state.style->scalex);
		state.transform.scale_y = std::max(0.0, state.style->scaley);
	}
	else if (tag.Name == "\\fscx") {
		auto const value = number_or_style(0, state.style->scalex);
		if (!value) return AssStateError::InvalidGeometryParameter;
		state.transform.scale_x = std::max(0.0, has_explicit_number(0)
			? Blend(state.transform.scale_x, *value, animation_power) : *value);
	}
	else if (tag.Name == "\\fscy") {
		auto const value = number_or_style(0, state.style->scaley);
		if (!value) return AssStateError::InvalidGeometryParameter;
		state.transform.scale_y = std::max(0.0, has_explicit_number(0)
			? Blend(state.transform.scale_y, *value, animation_power) : *value);
	}
	else if (tag.Name == "\\fax" || tag.Name == "\\fay") {
		auto const value = number_or_style(0, 0.0);
		if (!value) return AssStateError::InvalidGeometryParameter;
		auto& current = tag.Name == "\\fax"
			? state.transform.shear_x : state.transform.shear_y;
		current = has_explicit_number(0)
			? Blend(current, *value, animation_power) : *value;
	}
	else if (tag.Name == "\\frx" || tag.Name == "\\fry"
		|| tag.Name == "\\frz" || tag.Name == "\\fr") {
		double const style_default = tag.Name == "\\frz" || tag.Name == "\\fr"
			? state.style->angle : 0.0;
		auto const value = number_or_style(0, style_default);
		if (!value) return AssStateError::InvalidGeometryParameter;
		auto& current = tag.Name == "\\frx" ? state.transform.rotation_x
			: tag.Name == "\\fry" ? state.transform.rotation_y
			: state.transform.rotation_z;
		current = has_explicit_number(0)
			? Blend(current, *value, animation_power) : *value;
	}
	else if (tag.Name == "\\bord" || tag.Name == "\\xbord" || tag.Name == "\\ybord") {
		auto const value = number_or_style(0, state.style->outline_w);
		if (!value) return AssStateError::InvalidGeometryParameter;
		if (tag.Name != "\\ybord") {
			state.transform.outline_x = std::max(0.0, has_explicit_number(0)
				? Blend(state.transform.outline_x, *value, animation_power) : *value);
		}
		if (tag.Name != "\\xbord") {
			state.transform.outline_y = std::max(0.0, has_explicit_number(0)
				? Blend(state.transform.outline_y, *value, animation_power) : *value);
		}
	}
	else if (tag.Name == "\\shad" || tag.Name == "\\xshad" || tag.Name == "\\yshad") {
		auto const value = number_or_style(0, state.style->shadow_w);
		if (!value) return AssStateError::InvalidGeometryParameter;
		if (tag.Name != "\\yshad") {
			double const animated = has_explicit_number(0)
				? Blend(state.transform.shadow_x, *value, animation_power) : *value;
			state.transform.shadow_x = tag.Name == "\\shad"
				? std::max(0.0, animated) : animated;
		}
		if (tag.Name != "\\xshad") {
			double const animated = has_explicit_number(0)
				? Blend(state.transform.shadow_y, *value, animation_power) : *value;
			state.transform.shadow_y = tag.Name == "\\shad"
				? std::max(0.0, animated) : animated;
		}
	}
	else if (tag.Name == "\\p") {
		auto value = IntegerParameter(tag, 0);
		if (!value && !tag.Params.empty() && tag.Params.front().empty)
			value = 0;
		if (!value || *value < 0 || *value > 16)
			return AssStateError::InvalidGeometryParameter;
		state.drawing_scale = *value;
		state.drawing_mode = *value != 0;
	}
	else if (tag.Name == "\\pbo") {
		auto value = IntegerParameter(tag, 0);
		if (!value && !tag.Params.empty() && tag.Params.front().empty)
			value = 0;
		if (!value) return AssStateError::InvalidGeometryParameter;
		state.drawing_baseline_offset = *value;
	}
	else if (tag.Name == "\\fn") {
		state.font_name = tag.Params.empty() || tag.Params.front().omitted
			|| tag.Params.front().empty
			? event_style.font : tag.Params.front().Get<std::string>();
	}
	else if (tag.Name == "\\fs") {
		if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty) {
			state.font_size = event_style.fontsize;
		}
		else {
			auto const value = number(0);
			auto const raw = tag.Params.front().Get<std::string>();
			bool const relative = !raw.empty()
				&& (raw.front() == '+' || raw.front() == '-');
			if (!value || (!relative && *value <= 0.0)) {
				state.font_size = event_style.fontsize;
			}
			else {
				if (relative)
					state.font_size *= 1.0 + animation_power * *value / 10.0;
				else
					state.font_size = Blend(
						state.font_size, *value, animation_power);
				if (state.font_size <= 0.0)
					state.font_size = event_style.fontsize;
			}
		}
		if (!std::isfinite(state.font_size) || state.font_size <= 0.0
			|| state.font_size > MaxAbsCoordinate)
			return AssStateError::InvalidGeometryParameter;
	}
	else if (tag.Name == "\\fsp") {
		auto const value = number_or_style(0, state.style->spacing);
		if (!value) return AssStateError::InvalidGeometryParameter;
		state.spacing = has_explicit_number(0)
			? Blend(state.spacing, *value, animation_power) : *value;
	}
	else if (tag.Name == "\\b") {
		if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty) {
			state.font_weight = event_style.bold
				? aegisub::ass::BoldFontWeight : aegisub::ass::DefaultFontWeight;
		}
		else {
			auto const value = IntegerParameter(tag, 0);
			if (!value) {
				state.font_weight = event_style.bold
					? aegisub::ass::BoldFontWeight : aegisub::ass::DefaultFontWeight;
			}
			else {
				state.font_weight = NormalizeFontWeight(
					*value, event_style.bold
						? aegisub::ass::BoldFontWeight : aegisub::ass::DefaultFontWeight);
			}
		}
	}
	else if (tag.Name == "\\i" || tag.Name == "\\u" || tag.Name == "\\s") {
		if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty) {
			if (tag.Name == "\\i") state.italic = event_style.italic;
			else if (tag.Name == "\\u") state.underline = event_style.underline;
			else state.strikeout = event_style.strikeout;
		}
		else {
			auto const value = IntegerParameter(tag, 0);
			if (!value) {
				if (tag.Name == "\\i") state.italic = event_style.italic;
				else if (tag.Name == "\\u") state.underline = event_style.underline;
				else state.strikeout = event_style.strikeout;
			}
			else if (tag.Name == "\\i") state.italic = aegisub::ass::NormalizeVsfilterAssItalic(
				*value, event_style.italic);
			else if (tag.Name == "\\u") state.underline = *value != 0;
			else state.strikeout = *value != 0;
		}
	}
	else if (tag.Name == "\\fe") {
		if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty) {
			state.encoding = event_style.encoding;
		}
		else {
			auto const value = IntegerParameter(tag, 0);
			state.encoding = value
				? aegisub::ass::NormalizeVsfilterAssCharset(*value, event_style.encoding)
				: event_style.encoding;
		}
	}
	else if (tag.Name == "\\q") {
		auto const value = IntegerParameter(tag, 0);
		state.wrap_style = value && *value >= 0 && *value <= 3
			? *value : state.event_wrap_style;
	}

	return FiniteTransform(state.transform)
		? AssStateError::None : AssStateError::InvalidGeometryParameter;
}

bool IsVisibleGeometryBlock(AssDialogueBlock const& block) {
	if (block.GetType() != AssBlockType::PLAIN && block.GetType() != AssBlockType::DRAWING)
		return false;
	return !const_cast<AssDialogueBlock&>(block).GetText().empty();
}

bool IsEscapedBrace(std::string_view text, std::size_t position) {
	std::size_t slash_count = 0;
	while (position > slash_count && text[position - slash_count - 1] == '\\')
		++slash_count;
	return (slash_count & 1U) != 0;
}

std::vector<OverrideSpan> FindOverrideSpans(std::string_view text) {
	std::vector<OverrideSpan> spans;
	for (std::size_t position = 0; position < text.size();) {
		auto const open = text.find('{', position);
		if (open == std::string_view::npos)
			break;
		if (IsEscapedBrace(text, open)) {
			position = open + 1;
			continue;
		}
		auto const close = text.find('}', open + 1);
		if (close == std::string_view::npos)
			break;
		auto const content = text.substr(open + 1, close - open - 1);
		if (content.find('\\') != std::string_view::npos)
			spans.push_back({open, close + 1});
		position = close + 1;
	}
	return spans;
}

std::vector<RawTag> ParseRawTags(std::string_view content) {
	std::vector<RawTag> tags;
	// The libass-faithful cut replaces the old depth-tracking span walk:
	// for everything but a backslash swallowing through an early ')', the
	// two agree byte for byte, and the scanner also restores the spaced
	// spellings ("{\ frz30}") the prototype table alone cannot classify.
	aegisub::ass_tag_scanner::ScanRawTags(content,
										  [&](aegisub::ass_tag_scanner::RawTag const& scanned) {
											  RawTag raw;
											  raw.begin = static_cast<std::size_t>(
												  scanned.bytes.data() - content.data());
											  raw.end = raw.begin + scanned.bytes.size();
											  raw.raw.assign(scanned.bytes);
											  AssOverrideTag parsed(CanonicalizeTagText(scanned, 0));
											  raw.name = parsed.Name;
											  raw.valid = parsed.IsValid();
											  raw.contains_move = raw.valid && TagContainsMove(parsed);
											  raw.geometry_animation = raw.valid && TagContainsGeometryAnimation(parsed);
											  if (raw.valid && raw.name == "\\r" && !parsed.Params.empty()) {
												  auto const& parameter = parsed.Params.front();
												  raw.named_reset = !parameter.omitted && !parameter.empty && parameter.GetType() == VariableDataType::TEXT && !parameter.Get<std::string>().empty();
											  }
											  tags.push_back(std::move(raw));
										  });
	return tags;
}

std::vector<std::vector<bool>> BuildResetInsertionMap(
	std::string_view source_text,
	std::vector<ParsedOverrideSpan> const& spans) {
	std::vector<std::vector<bool>> needs;
	needs.reserve(spans.size());
	for (auto const& parsed : spans)
		needs.emplace_back(parsed.tags.size(), false);

	for (std::size_t span_index = 0; span_index < spans.size(); ++span_index) {
		auto const& current = spans[span_index];
		for (std::size_t tag_index = 0; tag_index < current.tags.size(); ++tag_index) {
			if (current.tags[tag_index].name != "\\r")
				continue;

			bool visible_before_next_reset = false;
			bool superseded_by_reset = false;
			for (std::size_t next_tag = tag_index + 1;
				next_tag < current.tags.size(); ++next_tag) {
				if (current.tags[next_tag].name == "\\r") {
					superseded_by_reset = true;
					break;
				}
			}
			if (!superseded_by_reset) {
				size_t scan_end = current.span.end;
				for (std::size_t next_span = span_index + 1;
					next_span < spans.size() && !visible_before_next_reset; ++next_span) {
					auto const gap = source_text.substr(
						scan_end, spans[next_span].span.begin - scan_end);
					if (GapHasVisibleText(gap)) {
						visible_before_next_reset = true;
						break;
					}
					for (auto const& tag : spans[next_span].tags) {
						if (tag.name == "\\r") {
							superseded_by_reset = true;
							break;
						}
					}
					if (superseded_by_reset)
						break;
					scan_end = spans[next_span].span.end;
				}
				if (!visible_before_next_reset && !superseded_by_reset
					&& GapHasVisibleText(source_text.substr(scan_end)))
					visible_before_next_reset = true;
			}
			needs[span_index][tag_index] = visible_before_next_reset && !superseded_by_reset;
		}
	}
	return needs;
}

struct RewriteDelta {
	bool changed = false;
	std::string line_bundle;
	std::string run_bundle;
	// True when the restricted candidate keeps the source's own \org, \fay,
	// \frx and \fry: the rewrite then leaves those tags byte-for-byte instead
	// of removing and re-serializing them through the decimal caps, which is
	// what rounds a high-precision source value away from the state the
	// staged re-verification must match.
	bool keep_restricted_tags = false;
	// Keep each unchanged, nondefault shear axis in its original spelling.
	// This preserves frozen precision even when the other axis changes or
	// inherits the style's default.
	bool keep_shear_x_tags = false;
	bool keep_shear_y_tags = false;
	bool keep_rotation_z_tags = false;
};

bool HasChanges(RewriteDelta const& delta) {
	return delta.changed;
}

bool RemoveTag(
	std::string const& name,
	PerspectiveScalePolicy scale_policy,
	bool keep_restricted_tags,
	bool keep_shear_x_tags,
	bool keep_shear_y_tags,
	bool keep_rotation_z_tags) {
	if (scale_policy == PerspectiveScalePolicy::Preserve && (name == "\\fsc" || name == "\\fscx" || name == "\\fscy"))
		return false;
	if (keep_restricted_tags && (name == "\\org" || name == "\\fay" || name == "\\frx" || name == "\\fry"))
		return false;
	if ((keep_shear_x_tags && name == "\\fax") || (keep_shear_y_tags && name == "\\fay"))
		return false;
	if (keep_rotation_z_tags && (name == "\\fr" || name == "\\frz"))
		return false;
	return name == "\\an" || name == "\\a" || name == "\\pos" || name == "\\move" || name == "\\org" || name == "\\fsc" || name == "\\fscx" || name == "\\fscy" || name == "\\fax" || name == "\\fay" || name == "\\fr" || name == "\\frz" || name == "\\frx" || name == "\\fry";
}

bool AppendTag(std::string& bundle, std::string_view name, std::string const& value) {
	if (value.empty())
		return false;
	bundle.append(name);
	bundle.append(value);
	return true;
}

std::optional<RewriteDelta> MakeRewriteDelta(
	EvaluatedTransformState const& source,
	EvaluatedTransformState const& event_style,
	EvaluatedTransformState const& target,
	SerializedTransformState const& serialized,
	PerspectiveScalePolicy scale_policy,
	PerspectiveRepresentationPolicy representation_policy,
	bool best_fit,
	bool preserve_rotation_z) {
	if (!FiniteTransform(source) || !FiniteTransform(event_style)
		|| !FiniteTransform(target))
		return std::nullopt;
	if (source.event_time_ms != target.event_time_ms)
		return std::nullopt;
	if (scale_policy == PerspectiveScalePolicy::Preserve
		&& (source.scale_x != target.scale_x
			|| source.scale_y != target.scale_y))
		return std::nullopt;

	RewriteDelta delta;
	// A restricted candidate is either the clean subset form the policy
	// filter accepts outright, or it keeps the source's restricted tags
	// exactly. In the kept form the original spelling is the only text that
	// reproduces the value bit for bit, so the rewrite must keep those tags
	// and never re-emit them from rounded digits.
	delta.keep_restricted_tags =
		!best_fit && representation_policy == PerspectiveRepresentationPolicy::FaxFrzOnly && !MatchesPerspectiveRepresentationPolicy(target, representation_policy);
	delta.keep_shear_x_tags = NearlyEqual(source.shear_x, target.shear_x) && !NearlyEqual(target.shear_x, event_style.shear_x);
	delta.keep_shear_y_tags = NearlyEqual(source.shear_y, target.shear_y) && !NearlyEqual(target.shear_y, event_style.shear_y);
	delta.keep_rotation_z_tags = best_fit && preserve_rotation_z && source.rotation_z == target.rotation_z;
	delta.changed = best_fit || source.alignment != target.alignment || !SamePoint(source.position, target.position) || !SameOptionalPoint(source.origin, target.origin) || !NearlyEqual(source.scale_x, target.scale_x) || !NearlyEqual(source.scale_y, target.scale_y) || !NearlyEqual(source.shear_x, target.shear_x) || !NearlyEqual(source.shear_y, target.shear_y) || !NearlyEqual(source.rotation_x, target.rotation_x) || !NearlyEqual(source.rotation_y, target.rotation_y) || !NearlyEqual(source.rotation_z, target.rotation_z);
	if (!delta.changed)
		return delta;

	bool const inherit_alignment = !best_fit && target.alignment == event_style.alignment;
	if (!inherit_alignment && !AppendTag(delta.line_bundle, "\\an",
		FormatAssNumber(target.alignment, 0))) return std::nullopt;
	// A line-wide alignment override changes the renderer's default position.
	// Without the event Style margins here, keep position explicit in that case.
	bool const inherit_position = inherit_alignment
		&& SamePoint(target.position, event_style.position);
	if (!inherit_position && !AppendTag(delta.line_bundle, "\\pos",
										serialized.position))
		return std::nullopt;
	if (target.origin && !delta.keep_restricted_tags) {
		if (!serialized.origin || !AppendTag(delta.line_bundle, "\\org", *serialized.origin))
			return std::nullopt;
	}

	if (scale_policy == PerspectiveScalePolicy::Fit && (best_fit || !NearlyEqual(target.scale_x, event_style.scale_x))) {
		if (!AppendTag(delta.run_bundle, "\\fscx",
					   serialized.scale_x))
			return std::nullopt;
	}
	if (scale_policy == PerspectiveScalePolicy::Fit && (best_fit || !NearlyEqual(target.scale_y, event_style.scale_y))) {
		if (!AppendTag(delta.run_bundle, "\\fscy",
					   serialized.scale_y))
			return std::nullopt;
	}
	if (!delta.keep_shear_x_tags && (best_fit || !NearlyEqual(target.shear_x, event_style.shear_x)) && !AppendTag(delta.run_bundle, "\\fax", serialized.shear_x))
		return std::nullopt;
	if (!delta.keep_shear_y_tags && !delta.keep_restricted_tags && (best_fit || !NearlyEqual(target.shear_y, event_style.shear_y)) && !AppendTag(delta.run_bundle, "\\fay", serialized.shear_y))
		return std::nullopt;
	if (!delta.keep_restricted_tags) {
		if ((best_fit || !EquivalentRotation(target.rotation_x, event_style.rotation_x)) && !AppendTag(delta.run_bundle, "\\frx",
																									   serialized.rotation_x))
			return std::nullopt;
		if ((best_fit || !EquivalentRotation(target.rotation_y, event_style.rotation_y)) && !AppendTag(delta.run_bundle, "\\fry",
																									   serialized.rotation_y))
			return std::nullopt;
	}
	if (!delta.keep_rotation_z_tags && (best_fit || !EquivalentRotation(target.rotation_z, event_style.rotation_z)) && !AppendTag(delta.run_bundle, "\\frz", best_fit && preserve_rotation_z ? FormatAssNumber(target.rotation_z, 9) : serialized.rotation_z))
		return std::nullopt;

	return delta;
}

RewriteError ValidateRawTags(std::vector<RawTag> const& tags) {
	RewriteError result = RewriteError::None;
	auto const mark = [&](RewriteError error) {
		auto const priority = [](RewriteError candidate) {
			switch (candidate) {
				case RewriteError::UnsupportedNamedReset: return 1;
				case RewriteError::UnsupportedGeometryAnimation: return 2;
				case RewriteError::UnsupportedMove: return 3;
				default: return 0;
			}
		};
		if (priority(error) > priority(result))
			result = error;
	};
	for (auto const& raw : tags) {
		if (!raw.valid)
			continue;
		if (raw.contains_move)
			mark(RewriteError::UnsupportedMove);
		if (raw.geometry_animation)
			mark(RewriteError::UnsupportedGeometryAnimation);
		if (raw.named_reset)
			mark(RewriteError::UnsupportedNamedReset);
	}
	return result;
}

std::string RewriteOverrideContent(
	std::string_view content,
	std::vector<RawTag> const& tags,
	std::vector<bool> const& reset_needs,
	RewriteDelta const& delta,
	PerspectiveScalePolicy scale_policy,
	bool insert_line_initial,
	bool insert_run_initial,
	std::size_t& tag_count) {
	std::string output;
	output.reserve(content.size() + delta.line_bundle.size()
		+ delta.run_bundle.size());
	auto const append_bundle = [&](std::string const& bundle) {
		output.append(bundle);
		tag_count += static_cast<std::size_t>(std::ranges::count(bundle, '\\'));
	};
	std::size_t cursor = 0;
	bool line_inserted = !insert_line_initial || delta.line_bundle.empty();
	bool const initial_run_superseded = insert_run_initial && std::any_of(
		tags.begin(), tags.end(), [](RawTag const& tag) { return tag.name == "\\r"; });
	bool run_inserted = !insert_run_initial || delta.run_bundle.empty()
		|| initial_run_superseded;

	for (std::size_t index = 0; index < tags.size(); ++index) {
		auto const& tag = tags[index];
		output.append(content.substr(cursor, tag.begin - cursor));
		if (!line_inserted) {
			append_bundle(delta.line_bundle);
			line_inserted = true;
		}
		if (!run_inserted && tag.name != "\\r") {
			append_bundle(delta.run_bundle);
			run_inserted = true;
		}
		if (!RemoveTag(tag.name, scale_policy, delta.keep_restricted_tags,
					   delta.keep_shear_x_tags, delta.keep_shear_y_tags, delta.keep_rotation_z_tags)) {
			output.append(tag.raw);
			if (RemoveTag(tag.name, PerspectiveScalePolicy::Fit, false, false, false, false))
				++tag_count;
		}
		if (tag.name == "\\r" && reset_needs[index]) {
			append_bundle(delta.run_bundle);
			run_inserted = true;
		}
		cursor = tag.end;
	}
	output.append(content.substr(cursor));
	if (!line_inserted)
		append_bundle(delta.line_bundle);
	if (!run_inserted)
		append_bundle(delta.run_bundle);
	return output;
}
}

char const* DescribeAssStateError(AssStateError error) {
	switch (error) {
		case AssStateError::None: return "Perspective ASS state evaluated successfully";
		case AssStateError::InvalidInput: return "Perspective ASS state input is incomplete";
		case AssStateError::InvalidPlayResolution: return "Perspective PlayRes is invalid";
		case AssStateError::MissingEventStyle: return "the dialogue event style cannot be resolved";
		case AssStateError::MissingResetStyle: return "a Perspective-relevant reset style cannot be resolved";
		case AssStateError::InvalidCaptureTime: return "the capture time is outside the dialogue event";
		case AssStateError::InvalidGeometryParameter: return "a Perspective geometry override is invalid";
		case AssStateError::InvalidAlignment: return "the effective ASS alignment is invalid";
		case AssStateError::NonFiniteStyle: return "the effective ASS style contains invalid geometry";
		case AssStateError::MixedGeometryRuns: return "the dialogue has mixed Perspective geometry runs";
	}
	return "unknown Perspective ASS state error";
}

char const* DescribeAssApplyBlocker(AssApplyBlocker blocker) {
	switch (blocker) {
		case AssApplyBlocker::None: return "Perspective Apply is supported";
		case AssApplyBlocker::UnsupportedMove: return "Perspective Apply does not support \\move";
		case AssApplyBlocker::UnsupportedGeometryAnimation: return "Perspective Apply does not support animated geometry";
		case AssApplyBlocker::UnsupportedNamedReset: return "Perspective Apply does not support named style resets";
	}
	return "unknown Perspective Apply blocker";
}

AssStateResult EvaluateEffectiveAssState(AssStateInput const& input) {
	AssStateResult result;
	if (!input.file || !input.line) {
		result.error = AssStateError::InvalidInput;
		return result;
	}
	if (!FinitePositive(input.play_resolution.width)
		|| !FinitePositive(input.play_resolution.height)) {
		result.error = AssStateError::InvalidPlayResolution;
		return result;
	}
	auto const start = static_cast<std::int64_t>(input.line->Start.GetMillisecond());
	auto const end = static_cast<std::int64_t>(input.line->End.GetMillisecond());
	if (input.capture_time_ms < start || input.capture_time_ms >= end) {
		result.error = AssStateError::InvalidCaptureTime;
		return result;
	}

	auto const* event_style = aegisub::ass_style_resolution::ResolveEventStyle(
		*input.file, input.line->Style.get());
	if (!event_style) {
		result.error = AssStateError::MissingEventStyle;
		return result;
	}

	WorkingState current;
	current.event_wrap_style = input.file->GetScriptInfoAsInt("WrapStyle");
	if (current.event_wrap_style < 0 || current.event_wrap_style > 3)
		current.event_wrap_style = 0;
	current.wrap_style = current.event_wrap_style;
	auto const event_time_ms = input.capture_time_ms - start;
	auto const event_duration_ms = end - start;
	result.error = ResetToStyle(
		current, *event_style, *input.line, input.play_resolution, event_time_ms, false);
	if (result.error != AssStateError::None)
		return result;
	result.value.event_style_transform = current.transform;

	std::optional<WorkingState> first_run;
	auto blocks = input.line->ParseTags();
	result.error = ApplyLineWideTags(
		current, blocks, *input.line, *event_style, input.play_resolution,
		event_time_ms, event_duration_ms, result.apply_blocker);
	if (result.error != AssStateError::None)
		return result;
	for (auto const& block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			auto const& override_block = static_cast<AssDialogueBlockOverride const&>(*block);
			std::vector<AssOverrideTag> tags;
			CanonicalOverrideTags(override_block.GetRawText(), tags);
			for (auto const& tag : tags) {
				result.error = ApplyTag(
					current, tag, *input.file, *input.line, *event_style,
					input.play_resolution, event_time_ms, event_duration_ms,
					1.0, result.apply_blocker);
				if (result.error != AssStateError::None)
					return result;
			}
			continue;
		}
		if (!IsVisibleGeometryBlock(*block))
			continue;
		if (!current.style_valid) {
			result.error = AssStateError::MissingResetStyle;
			return result;
		}
		++result.value.geometry_run_count;
		if (!first_run)
			first_run = current;
		else if (!SameRunState(*first_run, current)) {
			result.error = AssStateError::MixedGeometryRuns;
			return result;
		}
	}

	auto const& effective = first_run ? *first_run : current;
	result.value.transform = effective.transform;
	result.value.text_style.font_name = effective.font_name;
	result.value.text_style.font_size = effective.font_size;
	result.value.text_style.spacing = effective.spacing;
	result.value.text_style.font_weight = effective.font_weight;
	result.value.text_style.italic = effective.italic;
	result.value.text_style.underline = effective.underline;
	result.value.text_style.strikeout = effective.strikeout;
	result.value.text_style.encoding = effective.encoding;
	result.value.text_style.wrap_style = effective.wrap_style;
	result.value.text_style.available_wrap_width = effective.available_wrap_width;
	result.value.drawing_mode = effective.drawing_mode;
	result.value.drawing_scale = effective.drawing_scale;
	result.value.drawing_baseline_offset = effective.drawing_baseline_offset;
	return result;
}

char const* DescribeRewriteError(RewriteError error) {
	switch (error) {
		case RewriteError::None: return "Perspective ASS rewrite succeeded";
		case RewriteError::InvalidInput: return "Perspective ASS rewrite input is invalid";
		case RewriteError::InvalidTarget: return "Perspective ASS rewrite target is invalid";
		case RewriteError::UnsupportedMove: return "Perspective ASS rewrite does not support \\move";
		case RewriteError::UnsupportedGeometryAnimation: return "Perspective ASS rewrite does not support animated geometry";
		case RewriteError::UnsupportedNamedReset: return "Perspective ASS rewrite does not support named style resets";
		case RewriteError::InvalidGeometryParameter: return "Perspective ASS rewrite contains invalid geometry";
	}
	return "unknown Perspective ASS rewrite error";
}

struct PerspectiveTagRewriter::Data {
	std::string source_text;
	EvaluatedTransformState source_state;
	EvaluatedTransformState event_style_state;
	std::vector<ParsedOverrideSpan> parsed_spans;
	std::vector<std::vector<bool>> reset_needs;
	RewriteError error = RewriteError::None;
	std::size_t source_tag_count = 0;
};

PerspectiveTagRewriter::PerspectiveTagRewriter(
	std::string_view source_text,
	EvaluatedTransformState const& source_state,
	EvaluatedTransformState const& event_style_state) {
	auto prepared = std::make_shared<Data>();
	data = prepared;
	if (source_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		prepared->error = RewriteError::InvalidInput;
		return;
	}
	prepared->source_text.assign(source_text);
	prepared->source_state = source_state;
	prepared->event_style_state = event_style_state;
	for (auto const& span : FindOverrideSpans(source_text)) {
		auto const content = source_text.substr(span.begin + 1, span.end - span.begin - 2);
		prepared->parsed_spans.push_back({.span = span, .tags = ParseRawTags(content)});
	}
	prepared->reset_needs = BuildResetInsertionMap(source_text, prepared->parsed_spans);
	for (auto const& parsed : prepared->parsed_spans) {
		auto const validation = ValidateRawTags(parsed.tags);
		if (validation == RewriteError::UnsupportedMove)
			prepared->error = validation;
		else if (validation == RewriteError::UnsupportedGeometryAnimation && prepared->error != RewriteError::UnsupportedMove)
			prepared->error = validation;
		else if (validation == RewriteError::UnsupportedNamedReset && prepared->error == RewriteError::None)
			prepared->error = validation;
		for (auto const& tag : parsed.tags) {
			if (RemoveTag(tag.name, PerspectiveScalePolicy::Fit, false, false, false, false))
				++prepared->source_tag_count;
		}
	}
}

RewriteResult PerspectiveTagRewriter::Rewrite(
	EvaluatedTransformState const& target,
	SerializedTransformState const& serialized,
	PerspectiveScalePolicy scale_policy,
	PerspectiveRepresentationPolicy representation_policy,
	bool best_fit,
	bool preserve_rotation_z) const {
	RewriteResult result;
	result.error = data->error;
	if (result.error != RewriteError::None)
		return result;
	std::string_view const source_text = data->source_text;
	auto const& parsed_spans = data->parsed_spans;
	auto const delta = MakeRewriteDelta(
		data->source_state, data->event_style_state, target, serialized, scale_policy,
		representation_policy, best_fit, preserve_rotation_z);
	if (!delta) {
		result.error = RewriteError::InvalidTarget;
		return result;
	}
	if (!HasChanges(*delta)) {
		result.text.assign(source_text);
		result.cost = {.tag_count = data->source_tag_count, .byte_count = result.text.size()};
		return result;
	}

	bool const has_bundle = !delta->line_bundle.empty() || !delta->run_bundle.empty();
	bool const prepend_bundle = has_bundle
		&& (parsed_spans.empty() || parsed_spans.front().span.begin != 0);
	bool insert_initial = !prepend_bundle;
	std::size_t cursor = 0;
	result.text.reserve(
		source_text.size() + delta->line_bundle.size() + delta->run_bundle.size() * 2
		+ 2);
	if (prepend_bundle) {
		result.text.append("{" + delta->line_bundle + delta->run_bundle + "}");
		result.cost.tag_count = static_cast<std::size_t>(std::ranges::count(result.text, '\\'));
	}

	for (std::size_t span_index = 0; span_index < parsed_spans.size(); ++span_index) {
		auto const& parsed = parsed_spans[span_index];
		auto const& span = parsed.span;
		result.text.append(source_text.substr(cursor, span.begin - cursor));
		auto const content = source_text.substr(span.begin + 1, span.end - span.begin - 2);
		result.text.push_back('{');
		result.text.append(RewriteOverrideContent(
			content, parsed.tags, data->reset_needs[span_index],
			*delta, scale_policy, insert_initial, insert_initial, result.cost.tag_count));
		result.text.push_back('}');
		insert_initial = false;
		cursor = span.end;
	}
	result.text.append(source_text.substr(cursor));
	result.changed = result.text != source_text;
	result.cost.byte_count = result.text.size();
	return result;
}

RewriteResult RewritePerspectiveTags(
	std::string_view source_text,
	EvaluatedTransformState const& source_state,
	EvaluatedTransformState const& event_style_state,
	SolverCandidate const& candidate,
	PerspectiveScalePolicy scale_policy,
	PerspectiveRepresentationPolicy representation_policy,
	bool best_fit,
	bool preserve_rotation_z) {
	return PerspectiveTagRewriter(source_text, source_state, event_style_state)
		.Rewrite(candidate.state, candidate.serialized, scale_policy, representation_policy, best_fit, preserve_rotation_z);
}
}
