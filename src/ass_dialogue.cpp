// Copyright (c) 2005, Rodrigo Braz Monteiro
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

/// @file ass_dialogue.cpp
/// @brief Class for dialogue lines in subtitles
/// @ingroup subs_storage

#include "ass_dialogue.h"
#include "ass_compat.h"
#include "ass_parse_error.h"
#include "ass_style.h"

#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/split.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <boost/regex.hpp>

static int next_id = 0;

int FindDialogueBlockForRead(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	int raw_position)
{
	if (blocks.empty())
		return -1;

	size_t const position = static_cast<size_t>(std::max(raw_position, 0));
	size_t block_end = 0;
	for (size_t index = 0; index < blocks.size(); ++index) {
		block_end += blocks[index]->GetRawLength();
		// Caret positions sit between bytes. Choosing the block on the left at a
		// boundary prevents a following override (notably a trailing \r) from
		// affecting the state before the caret. Position zero still selects the
		// first block, which is the formatting applied to the first visible text.
		if (position <= block_end)
			return static_cast<int>(index);
	}

	return static_cast<int>(blocks.size()) - 1;
}

AssDialogue::AssDialogue() {
	Id = ++next_id;
}

AssDialogue::AssDialogue(AssDialogue const& that)
: AssDialogueBase(that)
, AssEntryListHook(that)
{
	Id = ++next_id;
}

AssDialogue::AssDialogue(AssDialogueBase const& that) : AssDialogueBase(that) { }

AssDialogue::AssDialogue(std::string const& data) {
	Id = ++next_id;
	Parse(data);
}

AssDialogue::~AssDialogue () { }

class tokenizer {
	agi::StringRange str;
	agi::split_iterator<agi::StringRange::const_iterator> pos;

public:
	tokenizer(agi::StringRange const& str) : str(str) , pos(agi::Split(str, ',')) { }

	agi::StringRange next_tok() {
		if (pos.eof())
			throw SubtitleFormatParseError("Failed parsing line: " + std::string(str.begin(), str.end()));
		return *pos++;
	}

	std::string next_str() { return agi::str(next_tok()); }
	std::string next_str_trim() { return agi::util::strings::trim_copy(agi::str(next_tok())); }
};

void AssDialogue::Parse(std::string const& raw) {
	agi::StringRange str;
	if (agi::util::strings::starts_with(raw, "Dialogue:")) {
		Comment = false;
		str = agi::StringRange(raw.begin() + std::min<size_t>(raw.size(), 10), raw.end());
	}
	else if (agi::util::strings::starts_with(raw, "Comment:")) {
		Comment = true;
		str = agi::StringRange(raw.begin() + std::min<size_t>(raw.size(), 9), raw.end());
	}
	else
		throw SubtitleFormatParseError("Failed parsing line: " + raw);

	tokenizer tkn(str);

	// Get first token and see if it has "Marked=" in it
	auto tmp = tkn.next_str_trim();
	bool ssa = agi::util::strings::istarts_with(tmp, "marked=");

	// Get layer number
	if (ssa)
		Layer = 0;
	else if (!AssCompat::ParseInteger(tmp, Layer))
		throw SubtitleFormatParseError("Failed parsing line: " + raw);

	int time = 0;
	if (!AssCompat::ParseTime(tkn.next_str_trim(), time))
		throw SubtitleFormatParseError("Failed parsing line: " + raw);
	Start = time;
	if (!AssCompat::ParseTime(tkn.next_str_trim(), time))
		throw SubtitleFormatParseError("Failed parsing line: " + raw);
	End = time;
	Style = tkn.next_str_trim();
	Actor = tkn.next_str_trim();
	for (int& margin : Margin) {
		if (!AssCompat::ParseInteger(tkn.next_str(), margin))
			throw SubtitleFormatParseError("Failed parsing line: " + raw);
		margin = agi::util::mid(AssStyle::MinMargin, margin, AssStyle::MaxMargin);
	}
	Effect = tkn.next_str_trim();

	std::string text{tkn.next_tok().begin(), str.end()};

	if (text.size() > 1 && text[0] == '{' && text[1] == '=') {
		static const boost::regex extradata_test("^\\{(=\\d+)+\\}");
		boost::match_results<std::string::iterator> rematch;
		if (boost::regex_search(text.begin(), text.end(), rematch, extradata_test)) {
			std::string extradata_str = rematch.str(0);
			text = rematch.suffix().str();

			static const boost::regex idmatcher("=(\\d+)");
			auto start = extradata_str.begin();
			auto end = extradata_str.end();
			std::vector<uint32_t> ids;
			while (boost::regex_search(start, end, rematch, idmatcher)) {
				uint32_t id = 0;
				if (!agi::util::strings::parse_integer(rematch.str(1), id))
					throw SubtitleFormatParseError("Failed parsing line: " + raw);
				ids.push_back(id);
				start = rematch.suffix().first;
			}
			ExtradataIds = ids;
		}
	}

	Text = text;
}

static void append_int(std::string &str, int v) {
	str.append(AssCompat::FormatInteger(v));
	str.push_back(',');
}

static void append_str(std::string &out, std::string const& str) {
	out.append(str);
	out.push_back(',');
}

static void append_unsafe_str(std::string &out, std::string const& str) {
	for (auto c : str) {
		if (c == ',')
			out.push_back(';');
		else
			out.push_back(c);
	}
	out.push_back(',');
}

std::string AssDialogue::GetEntryData() const {
	return GetEntryData(
		AssCompat::FormatTime(static_cast<int>(Start)),
		AssCompat::FormatTime(static_cast<int>(End)));
}

std::string AssDialogue::GetEntryData(std::string const& formatted_start, std::string const& formatted_end) const {
	std::string str = Comment ? "Comment: " : "Dialogue: ";
	str.reserve(51 + Style.get().size() + Actor.get().size() + Effect.get().size() + Text.get().size());

	append_int(str, Layer);
	append_str(str, formatted_start);
	append_str(str, formatted_end);
	append_unsafe_str(str, Style);
	append_unsafe_str(str, Actor);
	for (auto margin : Margin)
		append_int(str, margin);
	append_unsafe_str(str, Effect);

	if (ExtradataIds.get().size() > 0) {
		str.push_back('{');
		for (auto id : ExtradataIds.get()) {
			str.push_back('=');
			str.append(AssCompat::FormatUnsignedInteger(id));
		}
		str.push_back('}');
	}

	for (auto c : Text.get()) {
		if (c != '\n' && c != '\r')
			str.push_back(c);
	}

	return str;
}

static bool is_escaped_brace(std::string const& text, size_t position) {
	size_t slash_count = 0;
	while (position > slash_count && text[position - slash_count - 1] == '\\')
		++slash_count;
	return (slash_count & 1) != 0;
}

static size_t find_unescaped_open_brace(std::string const& text, size_t start) {
	for (auto position = text.find('{', start);
	     position != std::string::npos;
	     position = text.find('{', position + 1)) {
		if (!is_escaped_brace(text, position))
			return position;
	}
	return std::string::npos;
}

std::vector<std::unique_ptr<AssDialogueBlock>> AssDialogue::ParseTags() const {
	std::vector<std::unique_ptr<AssDialogueBlock>> Blocks;

	// Empty line, make an empty block
	if (Text.get().empty()) {
		Blocks.push_back(agi::make_unique<AssDialogueBlockPlain>());
		return Blocks;
	}

	int drawingLevel = 0;
	std::string const& text(Text.get());

	for (size_t len = text.size(), cur = 0; cur < len; ) {
		// Overrides block
		if (text[cur] == '{' && !is_escaped_brace(text, cur)) {
			size_t end = agi::util::strings::find(text, '}', cur);

			// VSFilter requires that override blocks be closed, while libass
			// does not. We match VSFilter here.
			if (end == agi::util::strings::npos)
				goto plain;

			++cur;
			// Get contents of block
			std::string work = text.substr(cur, end - cur);
			cur = end + 1;

			if (!work.empty() && agi::util::strings::find(work, '\\') == agi::util::strings::npos) {
				//We've found an override block with no backslashes
				//We're going to assume it's a comment and not consider it an override block
				Blocks.push_back(agi::make_unique<AssDialogueBlockComment>(work));
			}
			else {
				// Create block
				auto block = agi::make_unique<AssDialogueBlockOverride>(work);
				block->ParseTags();

				// Look for \p in block
				for (auto const& tag : block->Tags) {
					if (tag.Name == "\\p")
						drawingLevel = tag.Params[0].Get<int>(0);
				}
				Blocks.push_back(std::move(block));
			}

			continue;
		}

		// Plain-text/drawing block
plain:
		std::string work;
		size_t end = find_unescaped_open_brace(text, cur + 1);
		if (end == agi::util::strings::npos) {
			work = text.substr(cur);
			cur = len;
		}
		else {
			work = text.substr(cur, end - cur);
			cur = end;
		}

		if (drawingLevel == 0)
			Blocks.push_back(agi::make_unique<AssDialogueBlockPlain>(work));
		else
			Blocks.push_back(agi::make_unique<AssDialogueBlockDrawing>(work, drawingLevel));
	}

	return Blocks;
}

void AssDialogue::StripTags() {
	Text = GetStrippedText();
}

void AssDialogue::UpdateText(std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	if (blocks.empty()) return;
	std::string joined;
	for (auto& block : blocks)
		joined += block->GetText();
	Text = std::move(joined);
}

bool AssDialogue::CollidesWith(const AssDialogue *target) const {
	if (!target) return false;
	return ((Start < target->Start) ? (target->Start < End) : (Start < target->End));
}

std::string AssDialogue::GetStrippedText() const {
	auto blocks = ParseTags();
	std::string joined;
	for (auto* block : blocks | agi::of_type<AssDialogueBlockPlain>())
		joined += block->GetText();
	return joined;
}
