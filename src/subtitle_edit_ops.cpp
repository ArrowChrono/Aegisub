#include "subtitle_edit_ops.h"

#include "ass_compat.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace {

bool is_inside_override_block(std::string_view text, int pos) {
	pos = std::clamp(pos, 0, static_cast<int>(text.size()));
	if (pos == 0)
		return false;

	auto const last_open = text.rfind('{', static_cast<size_t>(pos - 1));
	if (last_open == std::string_view::npos)
		return false;

	auto const last_close = text.rfind('}', static_cast<size_t>(pos - 1));
	return last_close == std::string_view::npos || last_open > last_close;
}

bool has_char_at(std::string_view text, int pos, char value) {
	return pos >= 0 && pos < static_cast<int>(text.size()) && text[static_cast<size_t>(pos)] == value;
}

bool is_utf8_continuation(char value) {
	return (static_cast<unsigned char>(value) & 0xC0) == 0x80;
}

aegisub::subtitle_edit_ops::AutoCloseEdit make_replace_edit(int start, int end, std::string replacement, int caret) {
	return {true, start, end, std::move(replacement), caret};
}

bool match_trim_token_from_start(std::string const& text, size_t start, size_t& token_len) {
	if (start >= text.size())
		return false;

	if (text[start] == ' ' || text[start] == '\t') {
		token_len = 1;
		return true;
	}

	if (start + 1 < text.size() && text[start] == '\\') {
		char const next = text[start + 1];
		if (next == 'n' || next == 'N' || next == 'h') {
			token_len = 2;
			return true;
		}
	}

	return false;
}

bool match_trim_token_from_end(std::string const& text, size_t end, size_t& token_len) {
	if (end == 0)
		return false;

	if (text[end - 1] == ' ' || text[end - 1] == '\t') {
		token_len = 1;
		return true;
	}

	if (end >= 2 && text[end - 2] == '\\') {
		char const next = text[end - 1];
		if (next == 'n' || next == 'N' || next == 'h') {
			token_len = 2;
			return true;
		}
	}

	return false;
}

std::string trim_recombine_text(std::string text) {
	size_t begin = 0;
	size_t end = text.size();
	size_t token_len = 0;

	while (begin < end && match_trim_token_from_start(text, begin, token_len))
		begin += token_len;

	while (begin < end && match_trim_token_from_end(text, end, token_len))
		end -= token_len;

	return text.substr(begin, end - begin);
}

void expand_times(AssDialogue *src, AssDialogue *dst) {
	dst->Start = std::min(dst->Start, src->Start);
	dst->End = std::max(dst->End, src->End);
}

bool check_start(AssDialogue *line, AssDialogue *other) {
	if (!agi::util::strings::starts_with(line->Text.get(), other->Text.get()))
		return false;

	line->Text = trim_recombine_text(line->Text.get().substr(other->Text.get().size()));
	expand_times(line, other);
	return true;
}

bool check_end(AssDialogue *line, AssDialogue *other) {
	if (!agi::util::strings::ends_with(line->Text.get(), other->Text.get()))
		return false;

	line->Text = trim_recombine_text(line->Text.get().substr(0, line->Text.get().size() - other->Text.get().size()));
	expand_times(line, other);
	return true;
}

void apply_join(AssDialogue *first, AssDialogue *second, aegisub::subtitle_edit_ops::JoinMode mode) {
	switch (mode) {
	case aegisub::subtitle_edit_ops::JoinMode::Karaoke:
		if (second)
			first->Text = first->Text.get() + "{\\k" + std::to_string((second->End - second->Start) / 10) + "}" + second->Text.get();
		else
			first->Text = "{\\k" + std::to_string((first->End - first->Start) / 10) + "}" + first->Text.get();
		break;
	case aegisub::subtitle_edit_ops::JoinMode::Concatenate:
		if (second)
			first->Text = first->Text.get() + " " + second->Text.get();
		break;
	case aegisub::subtitle_edit_ops::JoinMode::WithLineBreaks:
		if (second)
			first->Text = first->Text.get() + "\\N" + second->Text.get();
		break;
	case aegisub::subtitle_edit_ops::JoinMode::KeepFirst:
		break;
	}
}

bool is_text_block_token(int type) {
	namespace dt = agi::ass::DialogueTokenType;

	switch (type) {
	case dt::TEXT:
	case dt::WORD:
	case dt::DRAWING:
	case dt::KARAOKE_TEMPLATE:
	case dt::KARAOKE_VARIABLE:
		return true;
	default:
		return false;
	}
}

bool is_color_tag_name(std::string_view name) {
	constexpr auto& known = aegisub::subtitle_edit_ops::ColorTagNames;
	return std::find(std::begin(known), std::end(known), name) != std::end(known);
}

/// Mirrors the escaped-open-brace rule of AssDialogue::ParseTags: a '{'
/// preceded by an odd number of backslashes is plain text there (only the
/// opening brace checks escaping), so tags lexed inside such a run are not
/// override tags and must not become swatches.
///
/// Sibling of IsEscapedOpenBrace in subs_edit_ctrl_stc.cpp, which applies the
/// same rule to a live wxStyledTextCtrl. Callers here pass an OVR_BEGIN offset,
/// so the '{' at `position` is already known and is not re-checked.
bool is_escaped_open_brace(std::string_view text, size_t position) {
	size_t slashes = 0;
	while (position > slashes && text[position - slashes - 1] == '\\')
		++slashes;
	return (slashes & 1) != 0;
}

/// One unit a block nudge can move, as a byte range of the dialogue body:
/// a brace block ({...}) or a single two-byte escape (\N, \n, \h).
struct movable_block {
	int start = 0;
	int end = 0;
};

/// The brace blocks and escapes of a tokenized body, in document order.
///
/// An unterminated '{' yields no block: there is no closing brace to carry
/// along, and treating the rest of the line as the block would move text the
/// user never selected. An escaped '{' is plain text (is_escaped_open_brace),
/// so it opens nothing here — the lexer disagrees and will read the following
/// real block as an error region, which is why a block after an escaped brace
/// simply stops being movable rather than moving as something else.
std::vector<movable_block> find_movable_blocks(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens)
{
	namespace dt = agi::ass::DialogueTokenType;

	std::vector<movable_block> blocks;
	int offset = 0;
	int brace_start = -1;

	for (auto const& token : tokens) {
		int const len = static_cast<int>(token.length);

		if (token.type == dt::OVR_BEGIN) {
			if (!is_escaped_open_brace(text, static_cast<size_t>(offset)))
				brace_start = offset;
		}
		else if (token.type == dt::OVR_END) {
			if (brace_start >= 0)
				blocks.push_back({brace_start, offset + len});
			brace_start = -1;
		}
		else if (token.type == dt::LINE_BREAK) {
			// Adjacent escapes coalesce into one token; each moves on its own.
			for (int escape = offset; escape + 1 < offset + len; escape += 2)
				blocks.push_back({escape, escape + 2});
		}

		offset += len;
	}

	return blocks;
}

/// Index into `blocks` of the block a nudge at `pos` should move, or -1.
///
/// A caret at a boundary between two blocks moves the one starting there,
/// following the caret's usual forward bias. Between a block and plain text
/// there is no contest: only the block is movable, from either of its edges.
int find_block_for_move(std::vector<movable_block> const& blocks, int pos) {
	int best = -1;
	for (size_t i = 0; i < blocks.size(); ++i) {
		if (blocks[i].start > pos)
			break;
		if (pos <= blocks[i].end)
			best = static_cast<int>(i);
	}
	return best;
}

/// Start of the backslash run ending at `pos`.
int backslash_run_start(std::string_view text, int pos) {
	while (pos > 0 && text[static_cast<size_t>(pos - 1)] == '\\')
		--pos;
	return pos;
}

/// Start of the plain-text unit ending at `end`: one codepoint, extended left
/// over an odd backslash run so a run stays glued to the byte it escapes. Left
/// unextended, a nudge over the tail of "\\{" would leave a bare '{' opening a
/// block, or leave the moved block's own '{' newly escaped.
int plain_unit_start(std::string_view text, int end) {
	int start = end - 1;
	while (start > 0 && is_utf8_continuation(text[static_cast<size_t>(start)]))
		--start;

	int const run_start = backslash_run_start(text, start);
	if ((start - run_start) % 2 != 0)
		start = run_start;
	return start;
}

/// End of the plain-text unit starting at `start`, mirroring plain_unit_start:
/// a unit opening with a backslash run takes the whole run, plus the codepoint
/// the run escapes when the run is odd.
int plain_unit_end(std::string_view text, int start) {
	auto const size = static_cast<int>(text.size());
	int end = start;

	if (text[static_cast<size_t>(end)] == '\\') {
		while (end < size && text[static_cast<size_t>(end)] == '\\')
			++end;
		if ((end - start) % 2 == 0 || end == size)
			return end;
	}

	++end;
	while (end < size && is_utf8_continuation(text[static_cast<size_t>(end)]))
		++end;
	return end;
}

/// Style slot a colour/alpha tag writes: the leading digit of \1c-style
/// names, primary for bare \c, and 0 for the all-slot \alpha.
int color_tag_slot(std::string_view name) {
	if (name[0] >= '1' && name[0] <= '4')
		return name[0] - '0';
	return name == "alpha" ? 0 : 1;
}
}

namespace aegisub::subtitle_edit_ops {

TextChangeRange FindMinimalTextChange(std::string_view old_text, std::string_view new_text) {
	std::size_t prefix = 0;
	auto const shared_size = std::min(old_text.size(), new_text.size());
	while (prefix < shared_size && old_text[prefix] == new_text[prefix])
		++prefix;

	if (prefix == old_text.size() && prefix == new_text.size())
		return {prefix, prefix, prefix, prefix, false};

	// A changed codepoint can share its leading UTF-8 byte with the old one.
	while (prefix > 0
		&& ((prefix < old_text.size() && is_utf8_continuation(old_text[prefix]))
			|| (prefix < new_text.size() && is_utf8_continuation(new_text[prefix]))))
		--prefix;

	std::size_t suffix = 0;
	auto const max_suffix = std::min(old_text.size() - prefix, new_text.size() - prefix);
	while (suffix < max_suffix
		&& old_text[old_text.size() - suffix - 1] == new_text[new_text.size() - suffix - 1])
		++suffix;

	// Matching continuation bytes at the end of different codepoints are not a
	// reusable suffix. Advance both suffix starts to the next codepoint boundary.
	while (suffix > 0
		&& (is_utf8_continuation(old_text[old_text.size() - suffix])
			|| is_utf8_continuation(new_text[new_text.size() - suffix])))
		--suffix;

	return {
		prefix,
		old_text.size() - suffix,
		prefix,
		new_text.size() - suffix,
		true,
	};
}

bool JoinSelectionIntoFirst(std::vector<AssDialogue *> const& selection, JoinMode mode) {
	if (selection.empty())
		return false;

	AssDialogue *first = selection.front();
	apply_join(first, nullptr, mode);
	for (size_t i = 1; i < selection.size(); ++i) {
		apply_join(first, selection[i], mode);
		first->End = std::max(first->End, selection[i]->End);
	}

	return true;
}

RecombineResult RecombineSelection(std::vector<AssDialogue *> selection) {
	RecombineResult result;
	if (selection.size() < 2)
		return result;

	std::sort(selection.begin(), selection.end(), [](AssDialogue const* a, AssDialogue const* b) {
		return a->Start < b->Start;
	});

	for (auto *line : selection)
		line->Text = trim_recombine_text(line->Text.get());

	for (size_t i = 0; i + 1 < selection.size();) {
		auto *d1 = selection[i];
		auto *d2 = selection[i + 1];

		if (d1->Text == d2->Text) {
			expand_times(d1, d2);
			result.lines_to_remove.push_back(d1);
			selection.erase(selection.begin() + i);
			continue;
		}

		if (d1->Text.get().empty()) {
			result.lines_to_remove.push_back(d1);
			selection.erase(selection.begin() + i);
			continue;
		}

		if (i + 1 == selection.size() - 1 && d2->Text.get().empty()) {
			result.lines_to_remove.push_back(d2);
			selection.erase(selection.begin() + i + 1);
			continue;
		}

		size_t j = i + 1;

		while (j < selection.size() && check_start(selection[j], d1))
			++j;

		while (j < selection.size() && check_end(selection[j], d1))
			++j;

		while (j < selection.size() && check_end(d1, selection[j]))
			++j;

		while (j < selection.size() && check_start(d1, selection[j]))
			++j;

		++i;
	}

	return result;
}

std::pair<std::string, std::string> SplitTextAtPosition(std::string const& text, int pos) {
	size_t const split_pos = static_cast<size_t>(std::clamp(pos, 0, static_cast<int>(text.size())));
	return {
		agi::util::strings::trim_utf8_right_copy(std::string_view(text).substr(0, split_pos)),
		agi::util::strings::trim_utf8_left_copy(std::string_view(text).substr(split_pos))
	};
}

std::optional<int> EstimateSplitTime(int start_ms, int end_ms, std::string const& first_text, std::string const& second_text) {
	size_t const combined_length = first_text.size() + second_text.size();
	if (!combined_length)
		return std::nullopt;

	double const split_pos = static_cast<double>(first_text.size()) / combined_length;
	return static_cast<int>((end_ms - start_ms) * split_pos) + start_ms;
}

std::string BuildTagOnlyText(AssDialogue const& line) {
	auto blocks = line.ParseTags();
	std::string text;
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::PLAIN)
			text += block->GetText();
	}
	return text;
}

std::string ReplaceRangeWithText(std::string text, int start, int end, std::string const& replacement) {
	int const clamped_start_int = std::clamp(start, 0, static_cast<int>(text.size()));
	int const clamped_end_int = std::clamp(end, clamped_start_int, static_cast<int>(text.size()));
	size_t const clamped_start = static_cast<size_t>(clamped_start_int);
	size_t const clamped_end = static_cast<size_t>(clamped_end_int);
	agi::util::strings::replace_range_inplace(text, clamped_start, clamped_end, replacement);
	return text;
}

AutoCloseEdit BuildAutoCloseEdit(std::string_view text, int selection_start, int selection_end, AutoCloseKey key) {
	int const text_size = static_cast<int>(text.size());
	selection_start = std::clamp(selection_start, 0, text_size);
	selection_end = std::clamp(selection_end, 0, text_size);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);

	if (selection_start != selection_end && key != AutoCloseKey::OpenBrace)
		return {};

	int const pos = selection_start;
	switch (key) {
	case AutoCloseKey::OpenBrace:
		if (selection_start != selection_end) {
			auto selected = text.substr(static_cast<size_t>(selection_start), static_cast<size_t>(selection_end - selection_start));
			return make_replace_edit(selection_start, selection_end, "{" + std::string(selected) + "}", selection_end + 2);
		}
		return make_replace_edit(pos, pos, "{}", pos + 1);
	case AutoCloseKey::OpenParen:
		if (is_inside_override_block(text, pos))
			return make_replace_edit(pos, pos, "()", pos + 1);
		return {};
	case AutoCloseKey::CloseBrace:
		if (has_char_at(text, pos, '}'))
			return make_replace_edit(pos, pos, "", pos + 1);
		return {};
	case AutoCloseKey::CloseParen:
		if (has_char_at(text, pos, ')') && is_inside_override_block(text, pos))
			return make_replace_edit(pos, pos, "", pos + 1);
		return {};
	case AutoCloseKey::Backspace:
		if (has_char_at(text, pos - 1, '{') && has_char_at(text, pos, '}'))
			return make_replace_edit(pos - 1, pos + 1, "", pos - 1);
		if (has_char_at(text, pos - 1, '(') && has_char_at(text, pos, ')') && is_inside_override_block(text, pos))
			return make_replace_edit(pos - 1, pos + 1, "", pos - 1);
		return {};
	}

	return {};
}

TextDragPreview BuildTextDragPreview(
	std::string_view text,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy) {
	int const text_length = static_cast<int>(text.size());
	selection_start = std::clamp(selection_start, 0, text_length);
	selection_end = std::clamp(selection_end, 0, text_length);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);
	drop_position = std::clamp(drop_position, 0, text_length);

	TextDragPreview preview{std::string(text), selection_start, selection_end, false};
	if (selection_start == selection_end)
		return preview;

	bool const drop_inside_selection = drop_position > selection_start && drop_position < selection_end;
	bool const drop_on_selection_edge = drop_position == selection_start || drop_position == selection_end;
	if (drop_inside_selection || (!copy && drop_on_selection_edge))
		return preview;

	std::string const selected(text.substr(
		static_cast<size_t>(selection_start),
		static_cast<size_t>(selection_end - selection_start)));
	int insertion_position = drop_position;
	if (!copy) {
		preview.text.erase(
			static_cast<size_t>(selection_start),
			static_cast<size_t>(selection_end - selection_start));
		if (drop_position > selection_end)
			insertion_position -= selection_end - selection_start;
	}

	preview.text.insert(static_cast<size_t>(insertion_position), selected);
	preview.selection_start = insertion_position;
	preview.selection_end = insertion_position + static_cast<int>(selected.size());
	preview.changed = true;
	return preview;
}

int MapTextDragPreviewPosition(
	int preview_position,
	int text_length,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy,
	bool preview_changed) {
	text_length = std::max(text_length, 0);
	selection_start = std::clamp(selection_start, 0, text_length);
	selection_end = std::clamp(selection_end, 0, text_length);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);
	drop_position = std::clamp(drop_position, 0, text_length);

	int const selection_length = selection_end - selection_start;
	int const preview_length = text_length + (copy && preview_changed ? selection_length : 0);
	preview_position = std::clamp(preview_position, 0, preview_length);
	if (!preview_changed || selection_length == 0)
		return std::min(preview_position, text_length);

	if (copy) {
		if (preview_position <= drop_position)
			return preview_position;
		if (preview_position <= drop_position + selection_length)
			return drop_position;
		return preview_position - selection_length;
	}

	if (drop_position < selection_start) {
		if (preview_position <= drop_position)
			return preview_position;
		if (preview_position <= drop_position + selection_length)
			return drop_position;
		if (preview_position < selection_end)
			return preview_position - selection_length;
		return preview_position;
	}

	if (preview_position <= selection_start)
		return preview_position;
	if (preview_position < drop_position - selection_length)
		return preview_position + selection_length;
	if (preview_position <= drop_position)
		return drop_position;
	return preview_position;
}

int GetPreviousBlockStart(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	namespace dt = agi::ass::DialogueTokenType;

	if (tokens.empty() || pos <= 0)
		return 0;

	std::vector<int> block_starts;
	int offset = 0;
	bool in_override = false;
	bool in_text_block = false;

	auto add_block_start = [&] {
		if (block_starts.empty() || block_starts.back() != offset)
			block_starts.push_back(offset);
	};

	for (auto const& tok : tokens) {
		int const len = static_cast<int>(tok.length);

		if (!in_override) {
			if (tok.type == dt::OVR_BEGIN) {
				add_block_start();
				in_override = true;
				in_text_block = false;
			}
			else if (tok.type == dt::LINE_BREAK) {
				add_block_start();
				in_text_block = false;
			}
			else if (is_text_block_token(tok.type)) {
				if (!in_text_block)
					add_block_start();
				in_text_block = true;
			}
			else {
				in_text_block = false;
			}
		}
		else {
			if (tok.type == dt::OVR_END) {
				in_override = false;
				in_text_block = false;
			}
		}

		offset += len;
	}

	if (block_starts.empty())
		return 0;

	int target = 0;
	for (int bs : block_starts) {
		if (bs < pos)
			target = bs;
		else
			break;
	}

	return target;
}

int GetNextBlockEnd(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	namespace dt = agi::ass::DialogueTokenType;

	if (tokens.empty())
		return 0;

	std::vector<int> block_ends;
	int offset = 0;
	bool in_override = false;
	bool in_text_block = false;

	auto add_block_end = [&] {
		if (block_ends.empty() || block_ends.back() != offset)
			block_ends.push_back(offset);
	};

	for (auto const& tok : tokens) {
		int const len = static_cast<int>(tok.length);

		if (!in_override) {
			if (is_text_block_token(tok.type)) {
				if (!in_text_block)
					in_text_block = true;
			}
			else {
				if (in_text_block) {
					add_block_end();
					in_text_block = false;
				}

				if (tok.type == dt::OVR_BEGIN) {
					in_override = true;
				}
				else if (tok.type == dt::LINE_BREAK) {
					offset += len;
					add_block_end();
					continue;
				}
			}
		}
		else {
			if (tok.type == dt::OVR_END) {
				offset += len;
				add_block_end();
				in_override = false;
				continue;
			}
		}

		offset += len;
	}

	if (in_text_block)
		add_block_end();

	if (block_ends.empty())
		return 0;

	int text_len = offset;
	for (int be : block_ends) {
		if (be > pos)
			return be;
	}

	return text_len;
}

BlockMoveEdit MoveBlockAtPosition(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens,
	int pos,
	BlockMoveDirection direction)
{
	BlockMoveEdit edit;
	if (pos < 0 || pos > static_cast<int>(text.size()))
		return edit;

	auto const blocks = find_movable_blocks(text, tokens);
	int const index = find_block_for_move(blocks, pos);
	if (index < 0)
		return edit;

	auto const block = blocks[static_cast<size_t>(index)];
	int step_start = 0, step_end = 0;

	if (direction == BlockMoveDirection::Left) {
		if (block.start == 0)
			return edit;

		step_end = block.start;
		// Step over a neighbouring block whole rather than into it.
		step_start = index > 0 && blocks[static_cast<size_t>(index - 1)].end == block.start
			? blocks[static_cast<size_t>(index - 1)].start
			: plain_unit_start(text, step_end);

		edit.replacement.append(text.substr(
			static_cast<size_t>(block.start),
			static_cast<size_t>(block.end - block.start)));
		edit.replacement.append(text.substr(
			static_cast<size_t>(step_start),
			static_cast<size_t>(step_end - step_start)));
		edit.replace_start = step_start;
		edit.replace_end = block.end;
		edit.delta = step_start - block.start;
	}
	else {
		if (block.end == static_cast<int>(text.size()))
			return edit;

		step_start = block.end;
		step_end = index + 1 < static_cast<int>(blocks.size())
				&& blocks[static_cast<size_t>(index + 1)].start == block.end
			? blocks[static_cast<size_t>(index + 1)].end
			: plain_unit_end(text, step_start);

		edit.replacement.append(text.substr(
			static_cast<size_t>(step_start),
			static_cast<size_t>(step_end - step_start)));
		edit.replacement.append(text.substr(
			static_cast<size_t>(block.start),
			static_cast<size_t>(block.end - block.start)));
		edit.replace_start = block.start;
		edit.replace_end = step_end;
		edit.delta = step_end - step_start;
	}

	edit.handled = true;
	edit.block_start = block.start;
	edit.block_end = block.end;
	return edit;
}

std::pair<int, int> GetBoundsOfEscapeAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	if (pos < 0)
		return {0, 0};

	int offset = 0;
	for (auto const& token : tokens) {
		int const len = static_cast<int>(token.length);
		if (pos < offset + len) {
			if (token.type != agi::ass::DialogueTokenType::LINE_BREAK)
				return {0, 0};

			// Adjacent escapes are coalesced into one LINE_BREAK token. Select
			// only the two-byte escape containing the clicked character.
			return {offset + (pos - offset) / 2 * 2, 2};
		}
		offset += len;
	}

	return {0, 0};
}

std::pair<int, int> GetBoundsOfTagAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	namespace dt = agi::ass::DialogueTokenType;

	if (pos < 0)
		return {0, 0};

	// Locate the token containing pos, tracking byte offsets as we go.
	size_t hit_index = tokens.size();
	int hit_start = 0;
	for (size_t i = 0; i < tokens.size(); ++i) {
		int const len = static_cast<int>(tokens[i].length);
		if (pos < hit_start + len) {
			hit_index = i;
			break;
		}
		hit_start += len;
	}
	if (hit_index == tokens.size())
		return {0, 0};

	int const hit_type = tokens[hit_index].type;

	// When pos is on a value, select just that argument so the selection does
	// not swallow adjacent tag name characters: the "5" in \bord5 is selected
	// alone, not "bord5".
	if (hit_type == dt::ARG)
		return {hit_start, static_cast<int>(tokens[hit_index].length)};

	if (hit_type != dt::TAG_START && hit_type != dt::TAG_NAME)
		return {0, 0};

	// Walk back to the TAG_START opening this tag. Only WHITESPACE can sit
	// between the backslash and the name, but stop at block boundaries anyway.
	size_t start_index = hit_index;
	int start = hit_start;
	if (hit_type != dt::TAG_START) {
		bool found = false;
		for (size_t i = hit_index; i-- > 0; ) {
			int const type = tokens[i].type;
			if (type == dt::OVR_BEGIN || type == dt::OVR_END || type == dt::ERROR)
				break;
			start -= static_cast<int>(tokens[i].length);
			if (type == dt::TAG_START) {
				start_index = i;
				found = true;
				break;
			}
		}
		if (!found)
			return {0, 0};
	}

	// Walk forward to the end of the tag, tracking paren depth so that a tag
	// nested in another tag's arguments (\t(0,500,\frz30)) neither truncates the
	// outer tag at the inner backslash nor lets the inner tag run past the paren
	// closing the outer one. Runs of ')' are lexed as a single CLOSE_PAREN token
	// (the "))" in \t(0,100,\clip(...))), so the end can land inside a token.
	int end = start + static_cast<int>(tokens[start_index].length);
	int depth = 0;
	for (size_t i = start_index + 1; i < tokens.size(); ++i) {
		int const type = tokens[i].type;
		int const len = static_cast<int>(tokens[i].length);

		// ERROR is a stray '{' inside the block.
		if (type == dt::OVR_BEGIN || type == dt::OVR_END || type == dt::ERROR)
			break;
		// Only a backslash outside of parens starts the next tag.
		if (type == dt::TAG_START && depth == 0)
			break;

		if (type == dt::OPEN_PAREN)
			depth += len;
		else if (type == dt::CLOSE_PAREN) {
			if (len > depth) {
				// This run also closes parens opened by an enclosing tag; take
				// only the ones that are ours.
				end += depth;
				break;
			}
			depth -= len;
		}

		end += len;
	}

	return {start, end - start};
}

std::pair<int, int> GetBoundsOfTagNameAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	if (pos < 0)
		return {0, 0};

	int offset = 0;
	for (auto const& token : tokens) {
		int const len = static_cast<int>(token.length);
		if (pos < offset + len) {
			if (token.type == agi::ass::DialogueTokenType::TAG_NAME)
				return {offset, len};
			return {0, 0};
		}
		offset += len;
	}

	return {0, 0};
}

TagDoubleClickPlan PlanTagDoubleClick(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens,
	int pos,
	std::pair<int, int> repeat_tag_name_bounds) {
	TagDoubleClickPlan plan;
	plan.selection = GetBoundsOfTagAtPosition(tokens, pos);
	if (plan.selection.second == 0)
		return plan;

	auto const name_bounds = GetBoundsOfTagNameAtPosition(tokens, pos);
	if (name_bounds.second == 0)
		return plan;

	auto const name = text.substr(name_bounds.first, name_bounds.second);
	if (name != "pos" && name != "move")
		return plan;

	if (name_bounds != repeat_tag_name_bounds) {
		plan.selection = name_bounds;
		plan.repeat_tag_name_bounds = name_bounds;
	}

	return plan;
}

std::vector<ColorSpan> FindColorSpans(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens) {
	namespace dt = agi::ass::DialogueTokenType;

	std::vector<ColorSpan> spans;
	size_t pos = 0;
	// Colour tags never take parentheses of their own, so any paren depth
	// above zero marks the tag as nested inside a \t(...) transform.
	int paren_depth = 0;
	std::string_view pending_name;
	bool name_is_color_tag = false;
	// True while inside a backslash-escaped '{...}' run, which the lexer
	// treats as an override block but ParseTags treats as plain text.
	bool escaped_block = false;

	for (auto const& token : tokens) {
		switch (token.type) {
			case dt::TAG_NAME: {
				auto const name = text.substr(pos, token.length);
				name_is_color_tag = !escaped_block && is_color_tag_name(name);
				if (name_is_color_tag)
					pending_name = name;
				break;
			}
			case dt::OPEN_PAREN:
				++paren_depth;
				name_is_color_tag = false;
				break;
			case dt::CLOSE_PAREN:
				if (paren_depth > 0)
					--paren_depth;
				name_is_color_tag = false;
				break;
			case dt::OVR_BEGIN:
				paren_depth = 0;
				name_is_color_tag = false;
				escaped_block = is_escaped_open_brace(text, pos);
				break;
			case dt::OVR_END:
				paren_depth = 0;
				name_is_color_tag = false;
				escaped_block = false;
				break;
			case dt::WHITESPACE:
				// Renderers skip the gap between a tag name and its value.
				break;
			case dt::ARG: {
				if (!name_is_color_tag)
					break;
				name_is_color_tag = false;
				ColorSpan span;
				span.byte_start = static_cast<int>(pos);
				span.byte_length = static_cast<int>(token.length);
				{
					// The lexer keeps leading/trailing blanks inside the
					// argument run; the AssOverrideTag rewrite path trims them,
					// so the value parse must too.
					auto const arg = text.substr(pos, token.length);
					size_t begin = 0, end = arg.size();
					while (begin < end && agi::util::strings::is_space(arg[begin]))
						++begin;
					while (end > begin && agi::util::strings::is_space(arg[end - 1]))
						--end;
					if (!AssCompat::ParseOverrideColor(arg.substr(begin, end - begin), span.color))
						break;
				}
				span.slot = color_tag_slot(pending_name);
				span.is_alpha = pending_name.back() == 'a';
				span.nested = paren_depth > 0;
				spans.push_back(span);
				break;
			}
			default:
				name_is_color_tag = false;
				break;
		}
		pos += token.length;
	}

	return spans;
}

std::pair<int, int> GetColorValueBounds(std::string_view text, ColorSpan const& span) {
	// Swatches cover only the value digits: skip the blanks and &/H sigils
	// the parse tolerates, then stop at the first non-hex byte, so the &H
	// prefix and trailing & keep their syntax colour and stay click-free.
	size_t begin = static_cast<size_t>(span.byte_start);
	size_t const end = begin + static_cast<size_t>(span.byte_length);
	while (begin < end && (agi::util::strings::is_space(text[begin]) || text[begin] == '&' || agi::util::strings::ascii_iequals(text[begin], 'h')))
		++begin;
	size_t digits = begin;
	std::uint32_t dummy = 0;
	while (digits < end && AssCompat::digit_value(text[digits], 16, dummy))
		++digits;
	return {static_cast<int>(begin), static_cast<int>(digits - begin)};
}

std::optional<int> GetTagEndInWrittenBlock(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	int blockn,
	std::string_view tag_name,
	std::string_view alt_name) {
	if (blockn < 0 || blockn >= static_cast<int>(blocks.size()))
		return std::nullopt;
	auto *ovr = dynamic_cast<AssDialogueBlockOverride const*>(blocks[blockn].get());
	if (!ovr)
		return std::nullopt;

	// GetText() of an override block is "{" + the tags serialized in order +
	// "}", so the tag's end offset is the sum of the preceding blocks, the
	// opening brace, and every tag before the match.
	size_t offset = 1;
	for (int index = 0; index < blockn; ++index)
		offset += blocks[index]->GetText().size();
	for (auto const& tag : ovr->Tags) {
		std::string const serialized = static_cast<std::string>(tag);
		if (tag.Name == tag_name || tag.Name == alt_name)
			return static_cast<int>(offset + serialized.size());
		offset += serialized.size();
	}
	return std::nullopt;
}

std::optional<int> GetTagEndInBlock(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	int caret_pos,
	std::string_view tag_name,
	std::string_view alt_name) {
	int const blockn = FindDialogueBlockForRead(blocks, caret_pos);
	// The resolved block first, then each neighbour once: a caret in plain
	// text resolves to the plain block while the written tag lives in an
	// override beside it, and re-serialization can shift a raw position by a
	// block. No wider search — the same tag name in a far block is a
	// different tag, and a wrong hit is worse than none. Callers that can
	// know the written block (set_tag reports it) should use
	// GetTagEndInWrittenBlock instead: this fallback can pick an earlier
	// same-named tag when a plain-text caret resolved between two overrides.
	if (auto end = GetTagEndInWrittenBlock(blocks, blockn, tag_name, alt_name))
		return end;
	if (auto end = GetTagEndInWrittenBlock(blocks, blockn - 1, tag_name, alt_name))
		return end;
	return GetTagEndInWrittenBlock(blocks, blockn + 1, tag_name, alt_name);
}
}
