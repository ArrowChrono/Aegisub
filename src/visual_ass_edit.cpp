// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "visual_ass_edit.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "visual_tool_shear_policy.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <unordered_map>

namespace aegisub::visual_edit {
namespace {
using Parameters = std::vector<AssOverrideParameter> const *;
using Blocks = std::vector<std::unique_ptr<AssDialogueBlock>>;

Parameters Find(Blocks const& blocks, std::string const& name) {
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride const&>(*block).Tags)
			if (tag.Name == name)
				return &tag.Params;
	}
	return nullptr;
}

Vector2D Point(Parameters tag, size_t x, size_t y) {
	if (!tag || tag->size() <= x || tag->size() <= y || (*tag)[x].omitted || (*tag)[y].omitted)
		return {};
	return {(*tag)[x].Get<float>(), (*tag)[y].Get<float>()};
}
}

Line ReadLine(AssDialogue const& dialogue, AssStyle const *style, Vector2D script_size) {
	Line line;
	line.id = dialogue.Id;
	line.row = dialogue.Row;
	line.text = dialogue.Text.get();
	line.start_ms = dialogue.Start;
	auto blocks = dialogue.ParseTags();
	line.static_shear = visual_shear::IsStatic(blocks);
	line.position = Point(Find(blocks, "\\pos"), 0, 1);
	if (!line.position)
		line.position = Point(Find(blocks, "\\move"), 0, 1);
	if (!line.position) {
		auto margin = dialogue.Margin;
		int alignment = style ? style->alignment : 2;
		if (style)
			for (size_t i = 0; i < margin.size(); ++i)
				if (!margin[i])
					margin[i] = style->Margin[i];
		int override_alignment = 0;
		if (auto tag = Find(blocks, "\\an"))
			override_alignment = (*tag)[0].Get<int>(0);
		else if (auto tag = Find(blocks, "\\a"))
			override_alignment = AssCompat::NormalizeLegacyAssAlignment((*tag)[0].Get<int>(0), 0);
		if (override_alignment > 0 && override_alignment <= 9)
			alignment = override_alignment;
		int horizontal = (alignment - 1) % 3;
		int vertical = (alignment - 1) / 3;
		int x = horizontal == 0 ? margin[0] : horizontal == 1 ? static_cast<int>((script_size.X() + margin[0] - margin[1]) / 2)
															  : static_cast<int>(script_size.X()) - margin[1];
		int y = vertical == 0 ? static_cast<int>(script_size.Y()) - margin[2] : vertical == 1 ? static_cast<int>(script_size.Y() / 2)
																							  : margin[2];
		line.position = {x, y};
	}
	if (auto tag = Find(blocks, "\\move")) {
		auto start = Point(tag, 0, 1);
		auto end = Point(tag, 2, 3);
		if (start && end)
			line.move = Move{.start = start, .end = end, .start_ms = (*tag)[4].Get<int>(0), .end_ms = (*tag)[5].Get<int>(0)};
	}
	if (auto origin = Point(Find(blocks, "\\org"), 0, 1))
		line.origin = origin;
	line.scale = style ? Vector2D(static_cast<float>(style->scalex), static_cast<float>(style->scaley)) : Vector2D(100, 100);
	line.rotation_z = style ? style->angle : 0;
	if (auto tag = Find(blocks, "\\frx"))
		line.rotation_x = tag->front().Get(line.rotation_x);
	if (auto tag = Find(blocks, "\\fry"))
		line.rotation_y = tag->front().Get(line.rotation_y);
	if (auto tag = Find(blocks, "\\frz"))
		line.rotation_z = tag->front().Get(line.rotation_z);
	else if (auto tag = Find(blocks, "\\fr"))
		line.rotation_z = tag->front().Get(line.rotation_z);
	if (auto tag = Find(blocks, "\\fscx"))
		line.scale = Vector2D(tag->front().Get(line.scale.X()), line.scale.Y());
	if (auto tag = Find(blocks, "\\fscy"))
		line.scale = Vector2D(line.scale.X(), tag->front().Get(line.scale.Y()));
	if (auto tag = Find(blocks, "\\fax"))
		line.fax = tag->front().Get(line.fax);
	if (auto tag = Find(blocks, "\\fay"))
		line.fay = tag->front().Get(line.fay);
	auto clip = Find(blocks, "\\iclip");
	line.clip.inverse = clip != nullptr;
	if (!clip)
		clip = Find(blocks, "\\clip");
	if (clip && clip->size() == 4)
		line.clip = {.first = Point(clip, 0, 1), .second = Point(clip, 2, 3), .inverse = line.clip.inverse};
	else
		line.clip = {.first = {0, 0}, .second = script_size - 1, .inverse = line.clip.inverse};
	return line;
}

Line ReadLine(AssDialogue const& dialogue, AssStyle const *style, Vector2D script_size, agi::vfr::Framerate const& timecodes) {
	auto line = ReadLine(dialogue, style, script_size);
	if (!dialogue.Comment) {
		line.first_frame = timecodes.FrameAtTime(dialogue.Start, agi::vfr::START);
		line.last_frame = timecodes.FrameAtTime(dialogue.End, agi::vfr::END);
	}
	line.position_to_move_start_ms = timecodes.TimeAtFrame(timecodes.FrameAtTime(dialogue.Start, agi::vfr::START)) - dialogue.Start;
	line.position_to_move_end_ms = timecodes.TimeAtFrame(timecodes.FrameAtTime(dialogue.Start, agi::vfr::END)) - dialogue.Start;
	return line;
}

void RefreshGeometryFromText(Line& line, Vector2D script_size) {
	AssDialogue dialogue{AssDialogueBase{}};
	dialogue.Text = line.text;
	auto geometry = ReadLine(dialogue, nullptr, script_size);
	line.position = geometry.position;
	line.move = geometry.move;
	line.origin = geometry.origin;
	line.clip = geometry.clip;
}

std::vector<Line> BuildLines(AssFile const& file, agi::vfr::Framerate const& timecodes, Vector2D script_size) {
	std::unordered_map<std::string, AssStyle const *> styles;
	for (auto const& style : file.Styles)
		styles.emplace(style.name, &style);
	std::vector<Line> lines;
	lines.reserve(file.Events.size());
	for (auto const& dialogue : file.Events) {
		auto style = styles.find(dialogue.Style.get());
		auto line = ReadLine(dialogue, style == styles.end() ? nullptr : style->second, script_size, timecodes);
		lines.push_back(std::move(line));
	}
	return lines;
}

void SetOverride(AssDialogue& line, std::string const& tag, std::string const& value) {
	std::string remove;
	if (tag == "\\1c")
		remove = "\\c";
	else if (tag == "\\frz")
		remove = "\\fr";
	else if (tag == "\\pos")
		remove = "\\move";
	else if (tag == "\\move")
		remove = "\\pos";
	else if (tag == "\\clip")
		remove = "\\iclip";
	else if (tag == "\\iclip")
		remove = "\\clip";
	auto blocks = line.ParseTags();
	if (blocks.front()->GetType() != AssBlockType::OVERRIDE) {
		line.Text = "{" + tag + value + "}" + line.Text.get();
		return;
	}
	auto& block = static_cast<AssDialogueBlockOverride&>(*blocks.front());
	std::erase_if(block.Tags, [&](auto const& existing) { return existing.Name == tag || existing.Name == remove; });
	block.AddTag(tag + value);
	line.UpdateText(blocks);
}

std::string SetOverride(std::string const& text, std::string const& tag, std::string const& value) {
	AssDialogue line{AssDialogueBase{}};
	line.Text = text;
	SetOverride(line, tag, value);
	return line.Text.get();
}
}
