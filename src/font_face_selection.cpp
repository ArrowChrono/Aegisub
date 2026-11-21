#include "font_face_selection.h"

#include "ass_tag_scanner.h"

#include <algorithm>
#include <bitset>
#include <vector>

namespace {
using aegisub::subtitle_edit_ops::ParsedLine;
using aegisub::subtitle_edit_ops::ResetStyleResolver;
using namespace aegisub::ass_tag_scanner;

constexpr auto FieldCount = FontOverrideIndex(FontOverrideField::Count);
constexpr auto ScaleX = FontOverrideIndex(FontOverrideField::ScaleX);
constexpr auto ScaleY = FontOverrideIndex(FontOverrideField::ScaleY);
constexpr auto Spacing = FontOverrideIndex(FontOverrideField::Spacing);
constexpr std::array<std::string_view, FieldCount> TagNames{"fn", "fs", "b", "i", "u", "fscx", "fscy", "fsp"};
constexpr std::array GeometryMembers{&FontGeometry::scale_x, &FontGeometry::scale_y, &FontGeometry::spacing};

std::string_view Trim(std::string_view value) {
	auto const first = value.find_first_not_of(" \t");
	return first == value.npos ? value.substr(value.size())
							   : value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

std::optional<std::size_t> FindField(std::string_view name) {
	for (std::size_t index = 0; index < TagNames.size(); ++index) {
		auto const token = TagNames[index];
		// Family names and numeric prefixes can immediately follow these names.
		if (index == FontOverrideIndex(FontOverrideField::Face) || index >= ScaleX) {
			if (NameHasPrefix(name, token))
				return index;
		}
		else if (NameIs(name, token))
			return index;
	}
	return std::nullopt;
}

std::string_view Argument(RawTag const& tag, std::size_t name_length) {
	auto const inline_arg = Trim(tag.name.substr(name_length));
	if (!inline_arg.empty() || !tag.has_paren)
		return inline_arg;
	auto const args = SplitLibassArgs(tag.args);
	return args.empty() ? inline_arg : args.front();
}

using FieldSet = std::bitset<FieldCount>;

FieldSet AnimatedFields(std::string_view body, int depth = 0) {
	FieldSet fields;
	if (depth == 32)
		return fields;
	ScanRawTags(body, [&](RawTag const& tag) {
		if (NameIs(tag.name, "t") && tag.has_paren)
			fields |= AnimatedFields(tag.args, depth + 1);
		else if (tag.name.starts_with('r'))
			fields.set();
		else if (Trim(tag.name) == "fsc")
			fields.set(ScaleX).set(ScaleY);
		else if (auto const field = FindField(tag.name))
			fields.set(*field);
	});
	return fields;
}

FontGeometry StyleGeometry(AssStyle const& style) {
	return {.scale_x = std::max(0.0, style.scalex), .scale_y = std::max(0.0, style.scaley), .spacing = style.spacing};
}

struct BlockAnalysis {
	std::array<FontOverrideEdit::Placement, FieldCount> placements;
	std::array<std::optional<std::size_t>, FieldCount> first_transform;
	FieldSet local_assignments;
};

class FontGeometryScanner {
	AssStyle const& event_style;
	ResetStyleResolver const& resolve_reset;
	FontGeometry baseline;

	public:
	FontGeometry geometry;
	FieldSet animated;

	FontGeometryScanner(AssStyle const& style, ResetStyleResolver const& resolver)
		: event_style(style), resolve_reset(resolver), baseline(StyleGeometry(style)), geometry(baseline) {}

	BlockAnalysis ReadBlock(std::string_view body, std::size_t body_offset) {
		BlockAnalysis local{};
		auto const inherited = animated;
		auto append_pos = body_offset + body.size();
		ScanRawTags(body, [&](RawTag const& tag) {
			auto const offset = body_offset + static_cast<std::size_t>(tag.bytes.data() - body.data());
			if (tag.has_paren && tag.bytes.back() != ')')
				append_pos = offset;

			if (tag.name.starts_with('r')) {
				baseline = StyleGeometry(*ParsedLine::reset_target(Argument(tag, 1), event_style, resolve_reset));
				geometry = baseline;
				animated.reset();
				local = {};
				local.local_assignments.set();
			}
			else if (Trim(tag.name) == "fsc") {
				geometry.scale_x = baseline.scale_x;
				geometry.scale_y = baseline.scale_y;
				for (auto const field : {ScaleX, ScaleY}) {
					animated.reset(field);
					local.placements[field] = {};
					local.first_transform[field].reset();
					local.local_assignments.set(field);
				}
			}
			else if (NameIs(tag.name, "t") && tag.has_paren) {
				auto const fields = AnimatedFields(tag.args);
				animated |= fields;
				for (std::size_t field = ScaleX; field < FieldCount; ++field) {
					if (fields[field] && !local.first_transform[field])
						local.first_transform[field] = offset;
				}
			}
			else if (auto const field = FindField(tag.name)) {
				auto const name_length = TagNames[*field].size();
				if (*field >= ScaleX) {
					auto const member = GeometryMembers[*field - ScaleX];
					auto const arg = Argument(tag, name_length);
					double const value = arg.empty() ? baseline.*member : ArgToDouble(arg);
					geometry.*member = *field == Spacing ? value : std::max(0.0, value);
				}
				animated.reset(*field);
				local.local_assignments.set(*field);
				auto const name_end = body_offset + static_cast<std::size_t>(tag.name.data() - body.data()) + name_length;
				auto const end = offset + (tag.has_paren ? tag.bytes.size() : Trim(tag.bytes).size());
				local.placements[*field] = {.begin = name_end, .end = end, .replaces_argument = true};
			}
		});
		for (std::size_t field = 0; field < FieldCount; ++field) {
			auto& placement = local.placements[field];
			if (!placement.replaces_argument) {
				auto const pos = local.first_transform[field].value_or(append_pos);
				placement = {.begin = pos, .end = pos, .inherits_animation = field >= ScaleX && inherited[field] && !local.local_assignments[field]};
			}
		}
		return local;
	}
};

struct Replacement {
	std::size_t begin;
	std::size_t end;
	bool replaces_argument;
	std::string text;
};

} // namespace

bool FontOverrideEdit::InheritsAnimation(FontOverrideField field) const {
	return placements[FontOverrideIndex(field)].inherits_animation;
}

FontOverrideResult FontOverrideEdit::Apply(std::string_view original, FontOverrideValues const& values,
										   int selection_start, int selection_end) const {
	FontOverrideResult result{.text = {}, .selection_start = selection_start, .selection_end = selection_end};
	std::vector<Replacement> replacements;
	std::string new_block;
	for (std::size_t field = 0; field < values.size(); ++field) {
		auto const& placement = placements[field];
		if (!values[field] || placement.inherits_animation)
			continue;
		std::string text;
		if (!placement.replaces_argument) {
			text.push_back('\\');
			text.append(TagNames[field]);
		}
		text.append(*values[field]);
		if (create_block)
			new_block += text;
		else
			replacements.push_back({.begin = placement.begin, .end = placement.end, .replaces_argument = placement.replaces_argument, .text = std::move(text)});
	}
	if (!new_block.empty()) {
		new_block.insert(new_block.begin(), '{');
		new_block.push_back('}');
		auto const pos = placements.front().begin;
		replacements.push_back({.begin = pos, .end = pos, .replaces_argument = false, .text = std::move(new_block)});
	}
	std::ranges::stable_sort(replacements, [](Replacement const& left, Replacement const& right) {
		if (left.begin != right.begin)
			return left.begin < right.begin;
		// A bare tag's argument ends at the same byte where a new tag can start.
		return left.replaces_argument && !right.replaces_argument;
	});

	std::size_t copied = 0;
	for (auto const& replacement : replacements) {
		result.text.append(original.substr(copied, replacement.begin - copied));
		auto map_position = [&](int original_pos, int& mapped_pos) {
			int const begin = static_cast<int>(replacement.begin);
			int const end = static_cast<int>(replacement.end);
			int const new_length = static_cast<int>(replacement.text.size());
			if (original_pos >= end)
				mapped_pos += new_length - (end - begin);
			else if (original_pos >= begin)
				mapped_pos = static_cast<int>(result.text.size()) + std::min(original_pos - begin, new_length);
		};
		map_position(selection_start, result.selection_start);
		map_position(selection_end, result.selection_end);
		result.text += replacement.text;
		copied = replacement.end;
	}
	result.text.append(original.substr(copied));
	return result;
}

FontGeometry ReadFontGeometry(ParsedLine const& line, int block, AssStyle const& event_style,
							  ResetStyleResolver const& resolve_reset) {
	FontGeometryScanner scanner(event_style, resolve_reset);
	for (int index = 0; index <= block; ++index) {
		auto const& current = *line.blocks[index];
		if (current.GetType() == AssBlockType::OVERRIDE)
			scanner.ReadBlock(current.GetRawText(), 0);
	}
	return scanner.geometry;
}

FontOverrideEdit CaptureFontOverrideEdit(ParsedLine const& line, int normalized_position, int raw_position,
										 AssStyle const& event_style, ResetStyleResolver const& resolve_reset) {
	auto const target = line.tag_write_target(normalized_position, raw_position);
	FontGeometryScanner scanner(event_style, resolve_reset);
	FontOverrideEdit edit;
	edit.create_block = target.block < 0 || line.blocks[target.block]->GetType() != AssBlockType::OVERRIDE;
	edit.read_block = edit.create_block ? target.block - 1 : target.block;
	std::size_t offset = 0;
	for (int index = 0; index <= edit.read_block; ++index) {
		auto const& block = *line.blocks[index];
		if (block.GetType() == AssBlockType::OVERRIDE)
			edit.placements = scanner.ReadBlock(block.GetRawText(), offset + 1).placements;
		offset += block.GetRawLength();
	}
	if (edit.create_block) {
		auto const pos = static_cast<std::size_t>(std::clamp(target.raw_pos, static_cast<int>(offset),
															 static_cast<int>(line.line->Text.get().size())));
		for (std::size_t field = 0; field < FieldCount; ++field)
			edit.placements[field] = {.begin = pos, .end = pos, .inherits_animation = field >= ScaleX && scanner.animated[field]};
	}
	edit.geometry = scanner.geometry;
	return edit;
}

bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view selected_name,
	bool face_modified) {
	return face_modified && stored_name != selected_name;
}
