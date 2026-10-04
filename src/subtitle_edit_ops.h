#pragma once

#include "ass_dialogue.h"

#include <libaegisub/color.h>

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agi::ass {
	struct DialogueToken;
}

namespace aegisub::subtitle_edit_ops {

enum class JoinMode {
	Karaoke,
	Concatenate,
	WithLineBreaks,
	KeepFirst
};

struct RecombineResult {
	std::vector<AssDialogue *> lines_to_remove;
};

struct TagDoubleClickPlan {
	std::pair<int, int> selection{0, 0};
	std::pair<int, int> repeat_tag_name_bounds{-1, 0};
};

enum class AutoCloseKey {
	OpenBrace,
	CloseBrace,
	OpenParen,
	CloseParen,
	Backspace
};

struct AutoCloseEdit {
	bool handled = false;
	int replace_start = 0;
	int replace_end = 0;
	std::string replacement;
	int caret = 0;
};

struct TextDragPreview {
	std::string text;
	int selection_start = 0;
	int selection_end = 0;
	bool changed = false;
};

/// Minimal byte ranges which transform old_text into new_text. All boundaries
/// are UTF-8 codepoint boundaries even when the differing codepoints share
/// leading or trailing bytes.
struct TextChangeRange {
	std::size_t old_begin = 0;
	std::size_t old_end = 0;
	std::size_t new_begin = 0;
	std::size_t new_end = 0;
	bool changed = false;
};

bool JoinSelectionIntoFirst(std::vector<AssDialogue *> const& selection, JoinMode mode);
RecombineResult RecombineSelection(std::vector<AssDialogue *> selection);
std::pair<std::string, std::string> SplitTextAtPosition(std::string const& text, int pos);
std::optional<int> EstimateSplitTime(int start_ms, int end_ms, std::string const& first_text, std::string const& second_text);
std::string BuildTagOnlyText(AssDialogue const& line);
std::string ReplaceRangeWithText(std::string text, int start, int end, std::string const& replacement);
AutoCloseEdit BuildAutoCloseEdit(std::string_view text, int selection_start, int selection_end, AutoCloseKey key);

TextChangeRange FindMinimalTextChange(std::string_view old_text, std::string_view new_text);

/// Build the document shown while selected text is dragged over a new position.
TextDragPreview BuildTextDragPreview(
	std::string_view text,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy);

/// Map a position in a drag preview back to the unmodified source document.
int MapTextDragPreviewPosition(
	int preview_position,
	int text_length,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy,
	bool preview_changed);

/// Get the start position to move to when pressing Home at the given position.
/// Blocks are override tags ({...}), line breaks (\N, \n), and runs of text.
int GetPreviousBlockStart(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Get the end position to move to when pressing End at the given position.
/// Blocks are override tags ({...}), line breaks (\N, \n), and runs of text.
int GetNextBlockEnd(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Direction a block nudge travels.
enum class BlockMoveDirection {
	Left,
	Right
};

/// A block nudge as one text replacement, plus where the moved block was and
/// how far it travelled so the caller can carry the caret along with it.
struct BlockMoveEdit {
	bool handled = false;  ///< False when the caret has nothing movable under it
	int replace_start = 0; ///< Byte range of the passed text the replacement covers
	int replace_end = 0;
	std::string replacement;
	int block_start = 0;   ///< Byte range the moved block occupied before the move
	int block_end = 0;
	int delta = 0;         ///< Bytes the block travelled; negative moving left
};

/// Swap the block under the caret with the unit beside it, so a nudge walks an
/// override block or a \\N-style escape through the line one step at a time.
///
/// What moves is a whole brace block ({...}, tags or comment) or one two-byte
/// escape; plain text stays put and a caret in it yields an unhandled edit.
/// What the block steps over is one codepoint of plain text, or the
/// neighbouring block whole — landing a block between braces, or between the
/// two bytes of an escape, would not leave a line the next nudge could
/// continue from. Backslash runs travel with what they escape, so a nudge
/// cannot newly escape the block's '{' or unescape a literal one.
///
/// `tokens` must be the tokenization of `text`.
BlockMoveEdit MoveBlockAtPosition(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens,
	int pos,
	BlockMoveDirection direction);

/// Get the span to select when double-clicking inside an override block, as
/// {start, length} in bytes. Returns the whole tag (backslash, name and all of
/// its arguments, with balanced parens) when pos is on the backslash or the tag
/// name, just the argument when pos is on a value, and {0, 0} when pos is not
/// on a tag.
std::pair<int, int> GetBoundsOfTagAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Get the span of the override tag name at pos, excluding the leading
/// backslash, as {start, length} in bytes. Returns {0, 0} when pos is not on a
/// tag name.
std::pair<int, int> GetBoundsOfTagNameAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Plan the override-tag selection for a double-click. The first click on a
/// pos or move name selects just the name and arms it for expansion; repeating
/// the double-click on the armed name selects the whole tag.
TagDoubleClickPlan PlanTagDoubleClick(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens,
	int pos,
	std::pair<int, int> repeat_tag_name_bounds);

/// Get the span of the ASS text escape (\N, \n, or \h) at pos, as
/// {start, length} in bytes. Returns {0, 0} when pos is not on an escape.
std::pair<int, int> GetBoundsOfEscapeAtPosition(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Override tag names whose single parameter is a colour or alpha value,
/// without the leading backslash. Their agreement with the override proto
/// table in ass_override.cpp is enforced by the classification assertions in
/// tests/tests/color_span.cpp rather than by shared code.
inline constexpr std::string_view ColorTagNames[] = {
	"c",
	"1c",
	"2c",
	"3c",
	"4c",
	"alpha",
	"1a",
	"2a",
	"3a",
	"4a",
};

/// One colour- or alpha-valued override parameter in a dialogue body.
struct ColorSpan {
	int byte_start = 0;    ///< Byte offset of the parameter run
	int byte_length = 0;   ///< Byte length of the parameter run
	/// Value parsed by AssCompat::ParseOverrideColor. Read it through
	/// alpha_value()/rgb() rather than directly: for an is_alpha span the tag
	/// carries one byte, and ParseOverrideColor lands it in `.r` (which is what
	/// ParseOverrideAlpha reads back), so `.r` is opacity and not red there.
	agi::Color color;
	int slot = 0;          ///< Style slot 1-4; \c counts as 1, \alpha as 0 (all slots)
	bool is_alpha = false; ///< \alpha family rather than \c family
	bool nested = false;   ///< Inside a \t(...) transform

	/// Opacity byte of an \alpha-family span (0 opaque, 255 transparent).
	/// Meaningless on a \c-family span.
	int alpha_value() const { return color.r; }
	/// Renderable colour of a \c-family span. Meaningless on an \alpha span.
	agi::Color rgb() const { return agi::Color(color.r, color.g, color.b); }
};

/// Enumerate the colour/alpha override parameters of a tokenized dialogue
/// body in document order. A tag yields a span only when its parameter token
/// parses as an override colour — the same lenient &H-hex parse that
/// AssOverrideParameter::Get<agi::Color> and the renderers use — so the set
/// of spans matches the set of values the colour-editing path understands.
std::vector<ColorSpan> FindColorSpans(
	std::string_view text,
	std::vector<agi::ass::DialogueToken> const& tokens);

/// Byte range of just the hex digits of a span's parameter, as {start,
/// length}: skips the blanks and &/H sigils the value parse tolerates and
/// stops at the first non-hex byte. Swatches paint and accept clicks over
/// this range rather than the full parameter run.
std::pair<int, int> GetColorValueBounds(std::string_view text, ColorSpan const& span);

/// Byte offset, in the serialized line text, just past the override tag named
/// `tag_name` or `alt_name` (both WITH the leading backslash, e.g. "\\3c"; an
/// empty `alt_name` matches nothing) in the override block that a raw caret
/// position reads from — the same block resolution the edit commands' read
/// path uses. `blocks` must be the parse of the FINAL text, after the tag
/// write. If the resolved block is not an override block or holds no such
/// tag, the neighbouring blocks are tried once each to absorb a one-block
/// drift from re-serialization. Returns nullopt when no candidate block holds
/// the tag.
///
/// Prefer GetTagEndInWrittenBlock when the writer can report the block it
/// wrote into: this caret-based resolution can pick an earlier same-named
/// tag when a plain-text caret sits between two override blocks.
std::optional<int> GetTagEndInBlock(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	int caret_pos,
	std::string_view tag_name,
	std::string_view alt_name);

/// GetTagEndInBlock's lookup against one known block index (as reported by
/// the writer), skipping caret resolution and the neighbour fallback
/// entirely. Returns nullopt when the block is not an override block or does
/// not hold a tag named `tag_name`/`alt_name`.
std::optional<int> GetTagEndInWrittenBlock(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	int blockn,
	std::string_view tag_name,
	std::string_view alt_name);
}
