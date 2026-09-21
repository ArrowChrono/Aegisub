#include "apply_plan.h"
#include "compact_geometry.h"
#include "geometry_apply.h"

#include "../align_video_fade.h"
#include "../ass_tag_scanner.h"
#include "../perspective_ass_state.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"

#include <libaegisub/ass/time.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisub::motion_track {

namespace {

using Blocks = std::vector<std::unique_ptr<AssDialogueBlock>>;

struct PositionInfo {
	bool has_pos = false;
	double pos_x = 0.0, pos_y = 0.0;
	bool has_move = false;
	double move_x1 = 0.0, move_y1 = 0.0, move_x2 = 0.0, move_y2 = 0.0;
	int move_t1 = 0, move_t2 = 0;
	// The \move spelled 6 arguments: t1/t2 are an explicit window even when
	// one bound is 0 (libass falls back to the whole event only when BOTH
	// are non-positive). A 4-arg \move leaves this false.
	bool has_move_window = false;
	// Presence of absolute geometry in the raw tag bytes. The full geometry
	// planner validates and transforms these tags when applying the motion.
	bool has_org = false;
	bool has_clip = false;
	int alignment_override = 0; // \\an / \\a (already converted to \\an space)
	// Whether any \an or \a was seen at all. A legacy \a with an out-of-range
	// value converts to 0 yet still consumes libass's PARSED_A slot (the
	// style alignment renders), so "converted value == 0" cannot double as
	// "not seen".
	bool has_alignment = false;
	// Inline transform tags (last occurrence wins, matching ASS override
	// semantics). Used as the similarity apply base so user styling survives.
	bool has_frz = false;
	double frz = 0.0;
	bool has_fscx = false;
	double fscx = 100.0;
	bool has_fscy = false;
	double fscy = 100.0;
	// Border/shadow/blur family, same last-wins rule. Growth compensation
	// composes these with the style base (croni growth semantics).
	bool has_bord = false;
	double bord = 0.0;
	bool has_xbord = false;
	double xbord = 0.0;
	bool has_ybord = false;
	double ybord = 0.0;
	bool has_shad = false;
	double shad = 0.0;
	bool has_xshad = false;
	double xshad = 0.0;
	bool has_yshad = false;
	double yshad = 0.0;
	bool has_blur = false;
	double blur = 0.0;
};

// --- raw \pos / \move / \an / \a scanning ---------------------------------
//
// Classification rules, all relative to the shared scanner's libass-faithful
// cut (aegisub::ass_tag_scanner: spaces/tabs after '\' skipped so
// "{\ pos(10,20)}" is a real tag, names are prefixes (mystrcmp), arguments
// end at the first ')' unless a '\' swallows through the next one):
//   * pos/move require nargs == 2 and nargs == 4 or 6 after dropping
//     empty/whitespace arguments; extra arguments (3-arg \pos, 7-arg
//     \move) are ignored and do not occupy the slot;
//   * unconvertible numeric arguments become 0 (argtod/argtoi32 ignore
//     conversion failure) and still occupy libass's EVENT_POSITIONED /
//     PARSED_A slots, so a later well-formed instance does not win;
//   * \t argument regions are parsed as tag lists again; other tags are not.

struct RawPositionState {
	bool has_pos = false;
	double pos_x = 0.0, pos_y = 0.0;
	bool has_move = false;
	double move_x1 = 0.0, move_y1 = 0.0, move_x2 = 0.0, move_y2 = 0.0;
	int move_t1 = 0, move_t2 = 0;
	// The \move spelled 6 arguments, so t1/t2 are an explicit window even
	// when one bound is 0. The 4-arg form leaves this false; both forms can
	// carry move_t1 = move_t2 = 0, and only the explicit one distinguishes
	// "window [0, 0]" from "no window given".
	bool has_move_window = false;
	// The event carries \org (libass's arity-2 first-wins rotation origin)
	// or \clip/\iclip in any argument form libass accepts, including
	// \t-animated instances. These pin absolute script-space geometry the
	// tracked rewrite cannot follow; the plan flags such lines for manual
	// review instead of trying to rewrite them.
	bool has_org = false;
	bool has_clip = false;
	bool has_alignment = false;
	int alignment_override = 0;
};

void ConsumeAlignment(RawPositionState& state, bool legacy_a, int value) {
	if (state.has_alignment)
		return;
	state.has_alignment = true;
	if (legacy_a)
		state.alignment_override = AssCompat::NormalizeLegacyAssAlignment(value, 0);
	else
		state.alignment_override = (value >= 1 && value <= 9) ? value : 0;
}

// Scans one raw override-block body in textual order; call it for every
// override block of the event in order so the state carries across blocks.
// pos and move share one event-level slot (libass's EVENT_POSITIONED): the
// first arity-valid instance of either kind blocks later instances of both.
// \t regions recurse; the depth cap keeps pathological nesting from
// exhausting the stack (libass itself does not bound the depth, but 32
// levels exceed any real event and match perspective_ass_state.cpp).
void ScanRawPositions(std::string_view body, RawPositionState& state,
					  int depth) {
	if (depth >= 32)
		return;
	ass_tag_scanner::ScanRawTags(body, [&](ass_tag_scanner::RawTag const& tag) {
		if (!tag.has_paren) {
			// "\a6" / "\an7" must not steal "\alpha": the scanner classifies
			// a handful of tags, so the short-tag rule stands in for libass's
			// chain order (alpha tested before a/an).
			if (ass_tag_scanner::NameIs(tag.name, "an"))
				ConsumeAlignment(state, false,
								 ass_tag_scanner::ArgToInt(tag.name.substr(2)));
			else if (ass_tag_scanner::NameIs(tag.name, "a"))
				ConsumeAlignment(state, true,
								 ass_tag_scanner::ArgToInt(tag.name.substr(1)));
			return;
		}
		auto const parts = ass_tag_scanner::SplitLibassArgs(tag.args);
		if (ass_tag_scanner::NameHasPrefix(tag.name, "t")) {
			ScanRawPositions(tag.args, state, depth + 1);
		}
		else if (ass_tag_scanner::NameHasPrefix(tag.name, "pos") && !state.has_pos && !state.has_move) {
			if (parts.size() == 2) {
				state.has_pos = true;
				state.pos_x = ass_tag_scanner::ArgToDouble(parts[0]);
				state.pos_y = ass_tag_scanner::ArgToDouble(parts[1]);
			}
		}
		else if (ass_tag_scanner::NameHasPrefix(tag.name, "move") && !state.has_pos && !state.has_move) {
			if (parts.size() == 4 || parts.size() == 6) {
				state.has_move = true;
				state.move_x1 = ass_tag_scanner::ArgToDouble(parts[0]);
				state.move_y1 = ass_tag_scanner::ArgToDouble(parts[1]);
				state.move_x2 = ass_tag_scanner::ArgToDouble(parts[2]);
				state.move_y2 = ass_tag_scanner::ArgToDouble(parts[3]);
				state.move_t1 = 0;
				state.move_t2 = 0;
				if (parts.size() == 6) {
					state.has_move_window = true;
					state.move_t1 = ass_tag_scanner::ArgToInt(parts[4]);
					state.move_t2 = ass_tag_scanner::ArgToInt(parts[5]);
					// libass swaps a reversed window unconditionally, before
					// the both-non-positive fallback check, so a reversed
					// explicit window renders as [t2, t1] and never falls
					// back and never fails.
					if (state.move_t1 > state.move_t2)
						std::swap(state.move_t1, state.move_t2);
				}
			}
		}
		else if (ass_tag_scanner::NameIs(tag.name, "an")) {
			int const value = parts.empty()
				? ass_tag_scanner::ArgToInt(tag.name.substr(2))
				: ass_tag_scanner::ArgToInt(parts[0]);
			ConsumeAlignment(state, false, value);
		}
		else if (ass_tag_scanner::NameIs(tag.name, "a")) {
			int const value = parts.empty()
				? ass_tag_scanner::ArgToInt(tag.name.substr(1))
				: ass_tag_scanner::ArgToInt(parts[0]);
			ConsumeAlignment(state, true, value);
		}
		else if (ass_tag_scanner::NameHasPrefix(tag.name, "org")) {
			// libass honors \org only with exactly two arguments, first-wins
			// per event; a bare \org renders nothing and needs no review.
			if (parts.size() == 2)
				state.has_org = true;
		}
		else if (ass_tag_scanner::NameHasPrefix(tag.name, "clip")
			|| ass_tag_scanner::NameHasPrefix(tag.name, "iclip")) {
			// Rectangles (4 args) and vector drawings (scale + drawing text)
			// both clip; any accepted argument form pins script-space
			// geometry, and \t-animated instances count through the
			// recursion above.
			if (!parts.empty())
				state.has_clip = true;
		}
	});
}

// \frz and the bare \fr alias are two proto entries for the same z-axis
// rotation (\fr30 parses to the name "\fr"), so both spellings feed the
// inline base and both are dropped when the base is re-emitted.
bool IsRotationZTag(std::string const& name) {
	return name == "\\frz" || name == "\\fr";
}

PositionInfo ExtractPositionInfo(AssDialogue const& line) {
	PositionInfo info;
	auto blocks = line.ParseTags();
	RawPositionState raw;
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto const *ov = static_cast<AssDialogueBlockOverride const *>(block.get());
		// pos/move/alignment all come from the raw scan above: it is a
		// superset of the parsed tag list and visits every instance in
		// textual order, so first-wins resolves exactly like libass does.
		// The parsed loop only collects the transform families, which are
		// last-wins per family.
		ScanRawPositions(ov->GetRawText(), raw, 0);
		for (auto const& tag : ov->Tags) {
			if (IsRotationZTag(tag.Name) && tag.Params.size() >= 1) {
				info.has_frz = true;
				info.frz = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\fscx" && tag.Params.size() >= 1) {
				info.has_fscx = true;
				info.fscx = tag.Params[0].Get<double>(100.0);
			}
			else if (tag.Name == "\\fscy" && tag.Params.size() >= 1) {
				info.has_fscy = true;
				info.fscy = tag.Params[0].Get<double>(100.0);
			}
			else if (tag.Name == "\\bord" && tag.Params.size() >= 1) {
				info.has_bord = true;
				info.bord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\xbord" && tag.Params.size() >= 1) {
				info.has_xbord = true;
				info.xbord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\ybord" && tag.Params.size() >= 1) {
				info.has_ybord = true;
				info.ybord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\shad" && tag.Params.size() >= 1) {
				info.has_shad = true;
				info.shad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\xshad" && tag.Params.size() >= 1) {
				info.has_xshad = true;
				info.xshad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\yshad" && tag.Params.size() >= 1) {
				info.has_yshad = true;
				info.yshad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\blur" && tag.Params.size() >= 1) {
				info.has_blur = true;
				info.blur = tag.Params[0].Get<double>(0.0);
			}
		}
	}
	info.has_pos = raw.has_pos;
	info.pos_x = raw.pos_x;
	info.pos_y = raw.pos_y;
	info.has_move = raw.has_move;
	info.move_x1 = raw.move_x1;
	info.move_y1 = raw.move_y1;
	info.move_x2 = raw.move_x2;
	info.move_y2 = raw.move_y2;
	info.move_t1 = raw.move_t1;
	info.move_t2 = raw.move_t2;
	info.has_move_window = raw.has_move_window;
	if (raw.has_alignment) {
		info.has_alignment = true;
		info.alignment_override = raw.alignment_override;
	}
	info.has_org = raw.has_org;
	info.has_clip = raw.has_clip;
	return info;
}

// The info-consuming half of ResolveDialogueOrigin. BuildApplyPlan calls
// this with the info it already extracted, so a planned line's tags are
// parsed exactly once.
bool ResolveDialogueOriginInfo(AssFile const& file, PositionInfo const& info,
							   AssDialogue const& line, int seed_time_ms,
							   int script_width, int script_height,
							   double& out_x, double& out_y) {
	if (info.has_pos) {
		out_x = info.pos_x;
		out_y = info.pos_y;
		return true;
	}

	if (info.has_move) {
		// libass (ass_parse.c, complex_tag("move")): a 6-arg \move swaps
		// t1 > t2 at parse time and falls back to the whole-event window
		// only when BOTH bounds are non-positive, so a window with any
		// positive bound -- including t1 = 0 -- is honored as written. The
		// scanner applied the swap, so w1 <= w2 holds for explicit windows.
		int const rel = seed_time_ms - int(line.Start);
		int w1 = info.move_t1;
		int w2 = info.move_t2;
		if (!info.has_move_window || (w1 <= 0 && w2 <= 0)) {
			w1 = 0;
			w2 = int(line.End) - int(line.Start);
		}
		if (rel <= w1) {
			out_x = info.move_x1;
			out_y = info.move_y1;
		}
		else if (rel >= w2) {
			out_x = info.move_x2;
			out_y = info.move_y2;
		}
		else {
			double const t = double(rel - w1) / double(w2 - w1);
			out_x = info.move_x1 + (info.move_x2 - info.move_x1) * t;
			out_y = info.move_y1 + (info.move_y2 - info.move_y1) * t;
		}
		return true;
	}

	// Style / margins / alignment fallback (mirrors VisualToolBase::
	// GetLinePosition without the wx-bound context lookups).
	auto margin = line.Margin;
	int align = 2;
	if (AssStyle const *style = const_cast<AssFile&>(file).GetStyle(line.Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; ++i)
			if (margin[i] == 0)
				margin[i] = style->Margin[i];
	}
	if (info.alignment_override > 0 && info.alignment_override <= 9)
		align = info.alignment_override;

	int const hor = (align - 1) % 3;
	int const vert = (align - 1) / 3;

	if (hor == 0)
		out_x = margin[0];
	else if (hor == 1)
		out_x = (script_width + margin[0] - margin[1]) / 2.0;
	else
		out_x = script_width - margin[1];

	if (vert == 0)
		out_y = script_height - margin[2];
	else if (vert == 1)
		out_y = script_height / 2.0;
	else
		out_y = margin[2];

	return true;
}

} // namespace

bool ResolveDialogueOrigin(AssFile const& file, AssDialogue const& line,
						   int seed_time_ms, int script_width,
						   int script_height, double& out_x, double& out_y) {
	return ResolveDialogueOriginInfo(file, ExtractPositionInfo(line), line,
									 seed_time_ms, script_width, script_height,
									 out_x, out_y);
}

void FillApplyInputScriptResolution(ApplyPlanInput& input, AssFile const& file) {
	int w = 0, h = 0;
	file.GetResolution(ScriptResolutionType::PlayRes, w, h);
	input.script_width = std::max(1, w);
	input.script_height = std::max(1, h);
}

namespace {

MotionTrackLineFingerprint FingerprintLine(AssFile const& file,
										   AssDialogue const& line) {
	MotionTrackLineFingerprint fp;
	fp.identity = reinterpret_cast<std::uintptr_t>(&line);
	fp.id = line.Id;
	fp.row = line.Row;
	fp.comment = line.Comment;
	fp.layer = line.Layer;
	fp.margins = line.Margin;
	fp.start_ms = int(line.Start);
	fp.end_ms = int(line.End);
	fp.style = line.Style.get();
	fp.actor = line.Actor.get();
	fp.effect = line.Effect.get();
	fp.extradata_ids = line.ExtradataIds.get();
	fp.text = line.Text.get();
	if (AssStyle const *style = const_cast<AssFile&>(file).GetStyle(line.Style))
		fp.style_entry = style->GetEntryData();
	return fp;
}

bool SameLineFingerprint(AssDialogue const& line,
						 MotionTrackLineFingerprint const& expected,
						 AssFile const& file) {
	if (reinterpret_cast<std::uintptr_t>(&line) != expected.identity || line.Id != expected.id || line.Row != expected.row || line.Comment != expected.comment || line.Layer != expected.layer || line.Margin != expected.margins || int(line.Start) != expected.start_ms || int(line.End) != expected.end_ms || line.Style.get() != expected.style || line.Actor.get() != expected.actor || line.Effect.get() != expected.effect || line.ExtradataIds.get() != expected.extradata_ids || line.Text.get() != expected.text)
		return false;
	std::string style_entry;
	if (AssStyle const *style = const_cast<AssFile&>(file).GetStyle(line.Style))
		style_entry = style->GetEntryData();
	return style_entry == expected.style_entry;
}

bool SameTimecodes(agi::vfr::Framerate const& left,
				   agi::vfr::Framerate const& right, int frame_count) {
	// IsVFR is the source representation (CFR vs a v2 table), not the
	// consumed frame timeline. A constant-rate file and a uniform v2 table
	// with the same boundaries must keep the session valid. FPSFraction is
	// kept so two rates that happen to agree on the sampled frames but
	// differ in the average used to extrapolate still mismatch.
	if (left.IsLoaded() != right.IsLoaded() || left.FPSFraction() != right.FPSFraction())
		return false;
	int const n = std::max(0, frame_count);
	for (int f = 0; f < n; ++f) {
		if (left.TimeAtFrame(f) != right.TimeAtFrame(f))
			return false;
	}
	// The final frame's end boundary: Apply reads TimeAtFrame(last, END) for
	// the domain end and the fade-out anchor, and that value is exactly
	// START(n) (identical midpoint formula in Framerate::TimeAtFrame), which
	// the loop above never reaches. Without this comparison a changed final
	// frame duration kept a stale session alive with mixed timelines.
	return left.TimeAtFrame(n, agi::vfr::Time::START) == right.TimeAtFrame(n, agi::vfr::Time::START);
}

std::vector<std::string> ScriptInfoEntries(AssFile const& file) {
	std::vector<std::string> entries;
	entries.reserve(file.Info.size());
	for (auto const& info : file.Info)
		entries.push_back(info.GetEntryData());
	return entries;
}

} // namespace

MotionTrackSourceSnapshot CaptureMotionTrackSource(
	AssFile const& file,
	std::vector<AssDialogue *> const& lines,
	agi::vfr::Framerate const& timecodes,
	int video_frame_count) {
	MotionTrackSourceSnapshot snap;
	snap.file_identity = reinterpret_cast<std::uintptr_t>(&file);
	snap.script_info_entries = ScriptInfoEntries(file);
	snap.timecodes = timecodes;
	snap.video_frame_count = video_frame_count;
	snap.lines.reserve(lines.size());
	for (auto *line : lines) {
		if (!line)
			continue;
		snap.lines.push_back(FingerprintLine(file, *line));
	}
	return snap;
}

bool MotionTrackSourceIsCurrent(
	AssFile const& file,
	MotionTrackSourceSnapshot const& expected,
	agi::vfr::Framerate const& timecodes) {
	if (reinterpret_cast<std::uintptr_t>(&file) != expected.file_identity)
		return false;
	if (ScriptInfoEntries(file) != expected.script_info_entries)
		return false;
	if (!SameTimecodes(expected.timecodes, timecodes, expected.video_frame_count))
		return false;
	std::string unused;
	return !ResolveMotionTrackSourceLines(const_cast<AssFile&>(file), expected,
										  unused)
				.empty();
}

std::vector<AssDialogue *> ResolveMotionTrackSourceLines(
	AssFile& file,
	MotionTrackSourceSnapshot const& expected,
	std::string& message) {
	std::vector<AssDialogue *> resolved;
	if (reinterpret_cast<std::uintptr_t>(&file) != expected.file_identity) {
		message = "subtitle file changed since Analyze";
		return {};
	}
	if (expected.lines.empty()) {
		message = "no target lines were captured for this session";
		return {};
	}
	for (auto const& fp : expected.lines) {
		AssDialogue *found = nullptr;
		for (auto& event : file.Events) {
			if (reinterpret_cast<std::uintptr_t>(&event) == fp.identity && event.Id == fp.id) {
				found = &event;
				break;
			}
		}
		if (!found) {
			message = "a tracked line is no longer in the file; re-run Analyze";
			return {};
		}
		if (!SameLineFingerprint(*found, fp, file)) {
			message = "a tracked line changed since Analyze; re-run Analyze";
			return {};
		}
		if (std::find(resolved.begin(), resolved.end(), found) != resolved.end()) {
			message = "duplicate target line in the Analyze snapshot";
			return {};
		}
		resolved.push_back(found);
	}
	return resolved;
}

bool MotionTrackContinueTargetsMatch(
	MotionTrackSourceSnapshot const& expected,
	std::vector<AssDialogue *> const& targets) {
	if (expected.lines.size() != targets.size())
		return false;
	for (size_t i = 0; i < targets.size(); ++i) {
		auto *line = targets[i];
		auto const& fp = expected.lines[i];
		if (!line
		    || reinterpret_cast<std::uintptr_t>(line) != fp.identity
		    || line->Id != fp.id
		    || int(line->Start) != fp.start_ms
		    || int(line->End) != fp.end_ms)
			return false;
	}
	return true;
}

bool MotionTrackTimecodesMatch(
	MotionTrackSourceSnapshot const& expected,
	agi::vfr::Framerate const& timecodes) {
	return SameTimecodes(expected.timecodes, timecodes,
	                     expected.video_frame_count);
}

namespace {

bool ShouldDropOverrideTag(std::string const& name, bool drop_transform_tags,
						   std::vector<std::string_view> const& extra_drops) {
	if (name == "\\pos" || name == "\\move")
		return true;
	// The whole inline rotation/scale family drops when the planner
	// re-emits it: leaving any member behind would win last-wins over the
	// appended compensation.
	if (drop_transform_tags &&
		(IsRotationZTag(name) || name == "\\fscx" || name == "\\fscy"))
		return true;
	for (auto extra : extra_drops)
		if (name == extra)
			return true;
	return false;
}

// True when `kept` block bytes still contain a \pos/\move a renderer would
// honor. Uses the raw scan, so the spellings the drop pass cannot classify
// ("\ pos(...)" or an instance nested in \t) count too.
bool KeptStillHasPosition(std::string const& kept) {
	RawPositionState scan;
	ScanRawPositions(kept, scan, 0);
	return scan.has_pos || scan.has_move;
}

// Drops matching top-level override tags and splices `tag` raw into the
// first override block. Kept tags are copied from the original text
// byte-for-byte: re-serializing through
// AssDialogueBlockOverride::GetText() would normalize libass-compatible
// input the planner never touches (extra \fad parameters beyond the two in
// the proto table, style-name spacing after \r, ...), so Apply must not
// round-trip through the parser.
//
// `insert_first` marks `tag` as position-sensitive (libass resolves pos/move
// first-wins per event). The replacement then leads the kept bytes when —
// and only when — the kept bytes of the insertion block still carry a
// position the drop pass could not remove (a "\ pos(...)" spelling or one
// nested in \t). Leading unconditionally would needlessly reorder clean
// lines and let a surviving last-wins tag override appended transform
// components — the drop pass removes the whole inline rotation/scale family
// (\frz, its \fr alias, \fscx/\fscy) for exactly that reason — so the
// common case keeps its historical byte layout.
std::string ReplaceTagsDropping(std::string const& text,
								std::string const& tag,
								bool drop_transform_tags,
								bool insert_first,
								std::vector<std::string_view> extra_drops = {}) {
	AssDialogue line;
	line.Text = text;
	auto blocks = line.ParseTags();

	std::string out;
	out.reserve(text.size() + tag.size() + 4);
	bool inserted = false;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) {
			out += block->GetText();
			continue;
		}

		// Same span split as AssDialogueBlockOverride::ParseTags, including
		// its starting index of 1: the first body byte is never examined, so
		// it can neither start a tag nor open a paren region — a body like
		// "(\pos(10,20))\p1" splits into the junk span "(" plus the
		// recognized \pos span, exactly the way the parser sees it. (A ')' at
		// depth 0 is ignored, so the tracked depth never goes below zero.)
		// Backslashes inside parentheses belong to a nested \t block and
		// never start a top-level tag.
		std::string const& body = block->GetRawText();
		std::string kept;
		kept.reserve(body.size());
		int depth = 0;
		size_t start = 0;
		auto flush_span = [&](size_t end) {
			std::string_view const span(
				body.data() + start, end == std::string::npos ? body.size() - start : end - start);
			if (span.empty())
				return;
			AssOverrideTag const parsed{std::string(span)};
			bool drop = ShouldDropOverrideTag(parsed.Name, drop_transform_tags, extra_drops);
			if (drop_transform_tags && !drop)
				ass_tag_scanner::ScanRawTags(span, [&](auto const& raw) {
					AssOverrideTag const canonical("\\" + std::string(raw.name));
					drop = drop || IsRotationZTag(canonical.Name) || canonical.Name == "\\fscx" || canonical.Name == "\\fscy";
				});
			if (!drop)
				kept += span;
		};
		for (size_t i = 1; i < body.size(); ++i) {
			char const c = body[i];
			if (depth > 0) {
				if (c == '(')
					++depth;
				else if (c == ')')
					--depth;
			}
			else if (c == '\\') {
				if (i > start)
					flush_span(i);
				start = i;
			}
			else if (c == '(')
				++depth;
		}
		flush_span(std::string::npos);

		if (!inserted) {
			// libass resolves pos/move first-wins per event: when the kept
			// bytes still hold a position spelling the drop pass cannot see,
			// the replacement must precede them to take effect.
			bool const lead = insert_first && KeptStillHasPosition(kept);
			out += '{';
			if (lead)
				out += tag;
			out += kept;
			if (!lead)
				out += tag;
			out += '}';
			inserted = true;
		}
		else if (!kept.empty())
			out += '{' + kept + '}';
	}
	if (!inserted)
		return "{" + tag + "}" + out;
	return out;
}

// Appends `tag` to the end of the first override block, keeping every
// existing byte as-is. Used for the last-wins transform components after the
// position pass has already dropped the tags it replaces, so nothing here
// needs dropping -- not even \pos, which ReplaceTagsDropping always strips.
std::string AppendTagToFirstBlock(std::string const& text,
								  std::string const& tag,
								  bool repeat_after_reset = false) {
	AssDialogue line;
	line.Text = text;
	auto blocks = line.ParseTags();
	std::string out;
	bool appended = false;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) {
			out += block->GetText();
			continue;
		}
		out += '{';
		out += block->GetRawText();
		bool reset = false;
		if (repeat_after_reset)
			ass_tag_scanner::ScanRawTags(block->GetRawText(), [&](auto const& raw) {
				// Reset style names are part of the raw name, so use the
				// renderer's prefix rule rather than requiring a numeric suffix.
				reset = reset || ass_tag_scanner::NameHasPrefix(raw.name, "r");
			});
		if (!appended || reset) {
			out += tag;
			appended = true;
		}
		out += '}';
	}
	if (!appended)
		return "{" + tag + "}" + text;
	return out;
}
}

std::string ReplacePositionTag(std::string const& text, std::string const& tag) {
	// pos/move are first-wins in libass: lead the kept bytes whenever a
	// stale position survives the drop pass (see ReplaceTagsDropping).
	return ReplaceTagsDropping(text, tag, false, true);
}

namespace {

// Inserts `tag` at the head of the text's first override block, creating a
// leading block when the text starts with plain text -- the same placement
// convention ApplyAssFade uses for its fade tags. The per-part \fade
// emission below runs after every part's stale fade representation has been
// stripped (align_video_fade::StripClaimableFade), so the inserted tag is
// the only fade representation the part carries.
std::string InsertTagAtBlockStart(std::string const& text,
								  std::string const& tag) {
	if (!text.empty() && text.front() == '{')
		return "{" + tag + text.substr(1);
	return "{" + tag + "}" + text;
}

struct ResolvedPoint {
	int frame = -1;
	double x = 0.0; // script px
	double y = 0.0;
	bool held = false;
	// Similarity only: emitted \frz / \fscx / \fscy values for this frame
	// (already composed with the style base). Identity defaults.
	double rot_deg = 0.0;
	double scale_pct_x = 100.0;
	double scale_pct_y = 100.0;
	// Similarity only: local stretch the frame's transform applies along the
	// script x / y axes at the line anchor (|M * e_x| and |M * e_y|). Uniform
	// 1.0 for translation. Drives border/shadow/blur growth compensation.
	double growth_x = 1.0;
	double growth_y = 1.0;
};

std::string FormatCoord(double v, int decimals) {
	// Avoid serializing a rounded value as "-0"; ASS accepts it, but it is
	// noisy and can make generated motion tags needlessly hard to inspect.
	double const quantum = std::pow(10.0, -std::max(0, decimals));
	if (std::abs(v) < quantum * 0.5)
		v = 0.0;
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
	return buf;
}

// An uncovered prefix/suffix whose boundaries collapse onto the same ASS
// centisecond serializes as a zero-duration event carrying the original text,
// so the planner must not emit it. Skipping is seamless by construction: the
// adjacent covered part's boundary already rounds onto the line's own
// centisecond, so nothing renderable is lost. Mirrors the rounding of
// agi::Time::operator int(), which drives event serialization.
int CentisecondRounded(int ms) {
	return int(agi::Time(ms));
}

struct SourceTimeTag {
	enum class Kind : std::uint8_t { Fade,
									 Transform,
									 Move } kind;
	std::string bytes;
	std::array<std::int64_t, 7> values{};
	std::string animated;
	std::string acceleration = "1";
	std::array<std::string, 4> coordinates;
};

bool AnimationInteger(std::string_view text, std::int64_t& value) {
	if (!text.empty() && text.front() == '+')
		text.remove_prefix(1);
	int parsed = 0;
	auto const result = std::from_chars(text.data(), text.data() + text.size(), parsed);
	value = parsed;
	return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ColorAlphaTransform(std::string_view body) {
	bool found = false, valid = true;
	ass_tag_scanner::ScanRawTags(body, [&](auto const& tag) {
		found = true;
		bool color = false;
		for (auto const name : {"c", "1c", "2c", "3c", "4c", "alpha", "1a", "2a", "3a", "4a"})
			color = color || ass_tag_scanner::NameIs(tag.name, name);
		valid = valid && color && !tag.has_paren;
	});
	return found && valid;
}

// Preserve source event-relative effects in every emitted event. The limited
// supported transform bundle is deliberate: arbitrary ASS geometry animation,
// nested transforms and karaoke require more than a change of event origin.
std::string CollectSourceTimeTags(AssDialogue const& line, std::vector<SourceTimeTag>& tags) {
	using ass_tag_scanner::NameHasPrefix;
	std::string error;
	std::int64_t const duration = static_cast<int>(line.End) - static_cast<int>(line.Start);
	for (auto const& block : line.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		ass_tag_scanner::ScanRawTags(block->GetRawText(), [&](auto const& raw) {
			if (!error.empty())
				return;
			if (NameHasPrefix(raw.name, "k") || NameHasPrefix(raw.name, "K")) {
				error = "make karaoke static before applying motion tracking";
				return;
			}
			bool const fade = NameHasPrefix(raw.name, "fad");
			bool const transform = NameHasPrefix(raw.name, "t");
			bool const move = NameHasPrefix(raw.name, "move");
			if (!fade && !transform && !move)
				return;
			auto const args = ass_tag_scanner::SplitLibassArgs(raw.args);
			SourceTimeTag tag{.kind = SourceTimeTag::Kind::Fade, .bytes = std::string(raw.bytes)};
			bool valid = raw.has_paren && raw.bytes.back() == ')';
			if (fade) {
				if (args.size() == 2) {
					tag.values = {255, 0, 255, 0, 0, 0, duration};
					valid = valid && AnimationInteger(args[0], tag.values[4]) && AnimationInteger(args[1], tag.values[5]);
					valid = valid && tag.values[4] >= 0 && tag.values[5] >= 0;
					tag.values[5] = duration - tag.values[5];
				}
				else if (args.size() == 7) {
					for (size_t i = 0; i < args.size(); ++i)
						valid = AnimationInteger(args[i], tag.values[i]) && valid;
					if (tag.values[3] == -1 && tag.values[6] == -1) {
						tag.values[3] = 0;
						tag.values[6] = duration;
						tag.values[5] = duration - tag.values[5];
					}
					valid = valid && tag.values[3] < tag.values[6];
					for (size_t i = 0; i < 3; ++i)
						valid = valid && tag.values[i] >= 0 && tag.values[i] <= 255;
				}
				else
					valid = false;
			}
			else if (transform) {
				tag.kind = SourceTimeTag::Kind::Transform;
				valid = valid && !args.empty() && args.size() <= 4 && ColorAlphaTransform(args.back());
				if (valid) {
					tag.animated = args.back();
					tag.values[0] = 0;
					tag.values[1] = duration;
					if (args.size() >= 3) {
						valid = AnimationInteger(args[0], tag.values[0]) && AnimationInteger(args[1], tag.values[1]);
						if (tag.values[1] == 0)
							tag.values[1] = duration;
					}
					if (args.size() == 2 || args.size() == 4) {
						tag.acceleration = args[args.size() - 2];
						double acceleration = 0;
						auto const& value = tag.acceleration;
						auto const parsed = std::from_chars(value.data(), value.data() + value.size(), acceleration);
						valid = valid && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
								std::isfinite(acceleration) && acceleration > 0;
					}
					valid = valid && tag.values[0] <= tag.values[1];
				}
			}
			else {
				tag.kind = SourceTimeTag::Kind::Move;
				valid = valid && (args.size() == 4 || args.size() == 6);
				if (valid) {
					for (size_t i = 0; i < 4; ++i)
						tag.coordinates[i] = args[i];
					tag.values[1] = duration;
					if (args.size() == 6) {
						valid = AnimationInteger(args[4], tag.values[0]) && AnimationInteger(args[5], tag.values[1]);
						if (tag.values[0] > tag.values[1])
							std::swap(tag.values[0], tag.values[1]);
						if (tag.values[0] <= 0 && tag.values[1] <= 0) {
							tag.values[0] = 0;
							tag.values[1] = duration;
						}
					}
				}
			}
			if (!valid)
				error = "unsupported event animation; motion tracking supports standard fades and color/alpha transforms";
			else
				tags.push_back(std::move(tag));
		});
	}
	return error;
}

std::string RetimeSourceTags(std::string_view text, std::vector<SourceTimeTag> const& tags,
							 std::int64_t offset, bool covered) {
	std::string output;
	size_t consumed = 0;
	for (size_t open = 0; (open = text.find('{', open)) != std::string_view::npos;) {
		size_t const end = text.find('}', ++open);
		if (end == std::string_view::npos)
			break;
		ass_tag_scanner::ScanRawTags(text.substr(open, end - open), [&](auto const& raw) {
			auto const found = std::ranges::find_if(tags, [&](auto const& tag) {
				return tag.bytes == raw.bytes && (!covered || tag.kind != SourceTimeTag::Kind::Move);
			});
			if (found == tags.end())
				return;
			auto const& tag = *found;
			std::string replacement;
			if (tag.kind == SourceTimeTag::Kind::Fade) {
				replacement = "\\fade(";
				for (size_t i = 0; i < 7; ++i) {
					if (i)
						replacement += ',';
					replacement += std::to_string(tag.values[i] - (i >= 3 ? offset : 0));
				}
				replacement += ')';
			}
			else if (tag.kind == SourceTimeTag::Kind::Transform) {
				// A zero end time means the new event duration to ASS, so an
				// already-completed transform must become its static target.
				if (tag.values[1] <= offset)
					replacement = tag.animated;
				else
					replacement = "\\t(" + std::to_string(tag.values[0] - offset) + "," +
								  std::to_string(tag.values[1] - offset) + "," + tag.acceleration + "," + tag.animated + ")";
			}
			else {
				auto const& p = tag.coordinates;
				if (tag.values[1] <= offset)
					replacement = "\\pos(" + p[2] + "," + p[3] + ")";
				else
					replacement = "\\move(" + p[0] + "," + p[1] + "," + p[2] + "," + p[3] + "," +
								  std::to_string(tag.values[0] - offset) + "," + std::to_string(tag.values[1] - offset) + ")";
			}
			auto const start = static_cast<size_t>(raw.bytes.data() - text.data());
			output.append(text.substr(consumed, start - consumed));
			output += replacement;
			consumed = start + raw.bytes.size();
		});
		open = end + 1;
	}
	output.append(text.substr(consumed));
	return output;
}

// Only the channels flattened by the simple Similarity writer need to agree
// between visible runs. Font, size, weight and color runs retain their scope.
std::string ResolveSimilarityBase(AssFile const& file, AssDialogue const& line,
								  ApplyPlanInput const& input, PositionInfo& info) {
	std::string geometry;
	bool reset = false;
	for (auto const& block : line.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) {
			if (block->GetType() != AssBlockType::COMMENT && !block->GetRawText().empty())
				geometry += 'x';
			continue;
		}
		std::string tags;
		bool missing_style = false;
		ass_tag_scanner::ScanRawTags(block->GetRawText(), [&](auto const& raw) {
			AssOverrideTag const tag("\\" + std::string(raw.name) + (raw.has_paren ? "(" + std::string(raw.args) + ")" : ""));
			if (IsRotationZTag(tag.Name) || tag.Name == "\\fscx" || tag.Name == "\\fscy") {
				tags += raw.bytes;
				info.has_frz = info.has_frz || IsRotationZTag(tag.Name);
				info.has_fscx = info.has_fscx || tag.Name == "\\fscx";
				info.has_fscy = info.has_fscy || tag.Name == "\\fscy";
			}
			else if (tag.Name == "\\r") {
				reset = true;
				std::string const name = tag.Params.empty() ? line.Style.get() : tag.Params[0].Get<std::string>(line.Style.get());
				auto const *style = const_cast<AssFile&>(file).GetStyle(name.empty() ? line.Style.get() : name);
				if (!style)
					missing_style = true;
				else
					tags += "\\frz" + FormatCoord(style->angle, 6) + "\\fscx" + FormatCoord(style->scalex, 6) + "\\fscy" + FormatCoord(style->scaley, 6);
			}
		});
		if (missing_style)
			return "Similarity cannot resolve a reset style";
		if (!tags.empty())
			geometry += "{" + tags + "}";
	}
	AssDialogue scoped(line);
	scoped.Text = geometry;
	auto const state = perspective::EvaluateEffectiveAssState({.file = &file, .line = &scoped, .play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)}, .capture_time_ms = std::clamp(input.seed_time_ms, line.Start.GetMillisecond(), line.End.GetMillisecond() - 1)});
	if (!state)
		return "Similarity cannot flatten different rotation or scale between text runs";
	info.has_frz = info.has_frz || reset;
	info.has_fscx = info.has_fscx || reset;
	info.has_fscy = info.has_fscy || reset;
	info.frz = state.value.transform.rotation_z;
	info.fscx = state.value.transform.scale_x;
	info.fscy = state.value.transform.scale_y;
	return {};
}

TrackSample const *FindOkAt(std::vector<TrackSample> const& samples, int frame) {
	auto const *s = FindSample(samples, frame);
	return s && s->status == TrackStatus::Ok ? s : nullptr;
}

// Nearest Ok sample scanning away from `from` within decode bounds; nullptr
// when none exists on that side.
TrackSample const *FindNearestOk(std::vector<TrackSample> const& samples,
								 int from, int step, FrameInterval const& decode) {
	for (int g = from; g >= decode.first && g <= decode.last; g += step) {
		if (auto const *ok = FindOkAt(samples, g))
			return ok;
	}
	return nullptr;
}

// Local-linear (order-1 Savitzky-Golay) smoothing over a ±half_window frame
// window, applied inside one run so hold boundaries never mix tracked motion
// into a hold (or vice versa). Constant-velocity input reproduces exactly;
// white per-frame jitter shrinks by roughly 1/sqrt(window). Frames inside a
// run are contiguous integers, so the window is frame-indexed.
void SmoothRun(std::vector<ResolvedPoint>& pts, size_t i0, size_t i1,
			   int half_window) {
	if (half_window <= 0 || i1 - i0 + 1 < 3)
		return;
	int const f_first = pts[i0].frame;
	int const f_last = pts[i1].frame;
	std::vector<double> sx(i1 - i0 + 1), sy(i1 - i0 + 1);
	for (size_t i = i0; i <= i1; ++i) {
		int const f = pts[i].frame;
		int const lo = std::max(f - half_window, f_first);
		int const hi = std::min(f + half_window, f_last);
		// Least-squares line v = a + b*(g-f) over the window, evaluated at
		// f itself; the centred abscissa keeps the normal matrix small and
		// means a is already the wanted value.
		double n = 0, sg = 0, sgg = 0, sxs = 0, sys = 0, sgx = 0, sgy = 0;
		for (int g = lo; g <= hi; ++g) {
			ResolvedPoint const& p = pts[i0 + size_t(g - f_first)];
			double const dg = g - f;
			n += 1;
			sg += dg;
			sgg += dg * dg;
			sxs += p.x;
			sys += p.y;
			sgx += dg * p.x;
			sgy += dg * p.y;
		}
		double const den = n * sgg - sg * sg;
		double const bx = den > 1e-12 ? (n * sgx - sg * sxs) / den : 0.0;
		double const by = den > 1e-12 ? (n * sgy - sg * sys) / den : 0.0;
		sx[i - i0] = (sxs - bx * sg) / n;
		sy[i - i0] = (sys - by * sg) / n;
	}
	for (size_t i = i0; i <= i1; ++i) {
		if (pts[i].held)
			continue; // holds are constant by construction
		pts[i].x = sx[i - i0];
		pts[i].y = sy[i - i0];
	}
}

// Trajectory stabilization chain.
//
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license) -- StabilisedPlanar: the
// upstream post-pass over tracked planar observations. Ported as pure
// functions over one hold-separated run of values:
//   1. angle unwrap (adjacent differences normalized into (-180, 180]),
//   2. median-of-3 smoothing (clamped edges; exact for monotone input),
//   3. [1,2,3,2,1]-weighted smoothing, applied only where the full symmetric
//      window exists so constant-velocity input passes through exactly at the
//      run ends too,
//   4. seed-frame pinning (the reference observation is restored verbatim),
//   5. noise-floor flattening: a run whose total excursion and deviation from
//      the reference stay below the floor is pinned to the reference as pure
//      sensor noise.
// The existing local-linear SmoothRun / Savitzky-Golay pass is a separate
// feature (options.smooth_frames, Translation positions); this chain is
// additive and gated by options.stabilization, and deliberately leaves the
// Compact translation fit untouched.

// Step 1: returns the sequence with adjacent jumps of exactly +-360 deg
// removed so smoothing sees a continuous ramp.
std::vector<double> UnwrapDegrees(std::vector<double> const& v) {
	std::vector<double> out(v.size(), 0.0);
	double offset = 0.0;
	for (size_t i = 0; i < v.size(); ++i) {
		if (i > 0) {
			double const d = v[i] + offset - out[i - 1];
			if (d > 180.0)
				offset -= 360.0;
			else if (d < -180.0)
				offset += 360.0;
		}
		out[i] = v[i] + offset;
	}
	return out;
}

// Step 2: median of the clamped neighbourhood {i-1, i, i+1}.
std::vector<double> MedianSmooth3(std::vector<double> const& v) {
	if (v.size() < 3)
		return v;
	std::vector<double> out(v.size(), 0.0);
	for (size_t i = 0; i < v.size(); ++i) {
		double const a = v[i > 0 ? i - 1 : i];
		double const b = v[i];
		double const c = v[i + 1 < v.size() ? i + 1 : i];
		out[i] = std::max(std::min(a, b), std::min(std::max(a, b), c));
	}
	return out;
}

// Step 3: [1,2,3,2,1]/9 kernel where the full window fits; run ends keep
// their median-smoothed values. Symmetric weights reproduce any linear
// sequence exactly, which is what keeps perfectly linear input unchanged.
std::vector<double> WeightedSmooth512(std::vector<double> const& v) {
	if (v.size() < 5)
		return v;
	constexpr int w[5] = {1, 2, 3, 2, 1};
	std::vector<double> out = v;
	for (size_t i = 2; i + 2 < v.size(); ++i) {
		double sum = 0.0;
		for (int k = -2; k <= 2; ++k)
			sum += w[k + 2] * v[size_t(int(i) + k)];
		out[i] = sum / 9.0;
	}
	return out;
}

// Steps 4 + 5 applied to one run: `seed_index` is the run-local index of the
// seed frame (-1 when the seed frame is outside this run). `floor` is in the
// value's own units; flattening compares relative deviation against the
// reference for ratio-like quantities (floor_is_ratio).
void StabilizeRun(std::vector<double>& v, int seed_index, double floor,
				  bool floor_is_ratio) {
	if (v.empty())
		return;
	int const ref_index = seed_index >= 0 && size_t(seed_index) < v.size() ? seed_index : 0;
	double const reference = v[size_t(ref_index)];
	// Ratio floors are meaningless against a zero reference (degenerate
	// style); fall back to absolute deviation there.
	bool const ratio = floor_is_ratio && std::abs(reference) > 1e-9;

	std::vector<double> const original = v;
	v = WeightedSmooth512(MedianSmooth3(v));

	// Pin the seed frame back to its exact observation: the seed pose is the
	// anchor everything else was tracked relative to.
	if (seed_index >= 0 && size_t(seed_index) < v.size())
		v[size_t(seed_index)] = original[size_t(seed_index)];

	// Noise floor: total excursion and deviation from the reference both
	// below the floor means the run is a noisy hold of the reference. For
	// ratio floors the excursion is a ratio too — an absolute range on a
	// ~100%-scale channel would never compare against a 0.012-style floor.
	auto const bounds = std::minmax_element(v.begin(), v.end());
	double const range = *bounds.second - *bounds.first;
	double const effective_range =
		ratio ? range / std::abs(reference) : range;
	double const ref_dev = ratio
							   ? std::max(std::abs(*bounds.first / reference - 1.0),
										  std::abs(*bounds.second / reference - 1.0))
							   : std::max(std::abs(*bounds.first - reference),
										  std::abs(*bounds.second - reference));
	if (effective_range < floor && ref_dev < floor)
		std::fill(v.begin(), v.end(), reference);
}

// Extracts one channel of a hold-separated run as a value sequence.
std::vector<double> ChannelOf(std::vector<ResolvedPoint> const& points,
							  std::pair<size_t, size_t> const& run,
							  double ResolvedPoint::*field) {
	std::vector<double> v(run.second - run.first + 1);
	for (size_t i = run.first; i <= run.second; ++i)
		v[i - run.first] = points[i].*field;
	return v;
}

// One emitted segment of the Compact fit: knots are sample indices, and the
// coordinates are FITTED knot positions (which adjacent pieces share exactly),
// not the sample values.
struct FitPiece {
	size_t i0 = 0, i1 = 0;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// Keep the existing pose accuracy independently of the position pixel budget.
// Pose curvature can require more transforms, but does not require more events:
// ASS supports a sequence of transforms alongside a single move.
constexpr double kCompactPoseAngleEpsilon = 0.05;
constexpr double kCompactPoseScaleEpsilon = 0.05;

double InterpolateByTime(double a, double b, int time_a, int time_b,
						 int time) {
	if (time_b <= time_a) {
		return a;
	}
	double const u = std::clamp(
		static_cast<double>(time - time_a) /
			static_cast<double>(time_b - time_a),
		0.0, 1.0);
	return a + ((b - a) * u);
}

// Unwrap the emitted angle sequence before Compact fitting. ASS renders
// angles modulo 360, but a raw -179 -> 179 pair would otherwise make a
// transform take the long way around.
void UnwrapCompactAngles(std::vector<ResolvedPoint>& points) {
	if (points.size() < 2) {
		return;
	}
	for (size_t i = 1; i < points.size(); ++i) {
		double delta = points[i].rot_deg - points[i - 1].rot_deg;
		while (delta > 180.0) {
			points[i].rot_deg -= 360.0;
			delta -= 360.0;
		}
		while (delta < -180.0) {
			points[i].rot_deg += 360.0;
			delta += 360.0;
		}
	}
}

struct PosePiece {
	size_t i0 = 0, i1 = 0;
	double acceleration = 1.0;
};

constexpr std::array<double ResolvedPoint::*, 3> pose_channels{
	&ResolvedPoint::rot_deg, &ResolvedPoint::scale_pct_x, &ResolvedPoint::scale_pct_y};
constexpr std::array<double, 3> pose_tolerances{
	kCompactPoseAngleEpsilon, kCompactPoseScaleEpsilon, kCompactPoseScaleEpsilon};

double RoundedPose(double value) {
	auto const text = FormatCoord(value, 2);
	double rounded = 0.0;
	std::from_chars(text.data(), text.data() + text.size(), rounded);
	return rounded;
}

// Fixed, serialized endpoints make each frame's error constraint an interval
// of permitted accelerations. Intersect those intervals across every channel;
// prefer linear interpolation whenever possible. No search grid or relaxed
// pose tolerance is needed to recognize an accelerated ASS transform.
std::optional<double> FitPoseAcceleration(std::vector<ResolvedPoint> const& points,
										  size_t first, size_t last, agi::vfr::Framerate const& timecodes) {
	int const ta = timecodes.TimeAtFrame(points[first].frame);
	int const tb = timecodes.TimeAtFrame(points[last].frame);
	if (last <= first + 1 || tb <= ta)
		return 1.0;
	std::array<double, 3> starts{}, ends{};
	for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
		starts[channel] = RoundedPose(points[first].*pose_channels[channel]);
		ends[channel] = RoundedPose(points[last].*pose_channels[channel]);
	}
	double lower = 0.0, upper = std::numeric_limits<double>::infinity();
	for (size_t i = first + 1; i < last; ++i) {
		double const u = static_cast<double>(timecodes.TimeAtFrame(points[i].frame) - ta) / (tb - ta);
		for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
			auto const field = pose_channels[channel];
			double const a = starts[channel];
			double const b = ends[channel];
			double const value = points[i].*field;
			double const tolerance = pose_tolerances[channel];
			if (a == b || u <= 0.0 || u >= 1.0) {
				if (std::abs(value - (u >= 1.0 ? b : a)) > tolerance)
					return std::nullopt;
				continue;
			}
			double lo = (value - tolerance - a) / (b - a);
			double hi = (value + tolerance - a) / (b - a);
			if (lo > hi)
				std::swap(lo, hi);
			if (hi <= 0.0 || lo >= 1.0)
				return std::nullopt;
			double const log_u = std::log(u);
			if (hi < 1.0)
				lower = std::max(lower, std::log(hi) / log_u);
			if (lo > 0.0)
				upper = std::min(upper, std::log(lo) / log_u);
			if (lower > upper)
				return std::nullopt;
		}
	}
	double const candidate = lower <= 1.0 && upper >= 1.0
								 ? 1.0
							 : std::isfinite(upper) ? lower + (upper - lower) * 0.5
													: lower + 1.0;
	double const acceleration = std::round(candidate * 1e6) / 1e6;
	if (!std::isfinite(acceleration) || acceleration <= 0.0)
		return std::nullopt;
	// Rounding the exponent must not push a candidate outside the error budget.
	for (size_t i = first + 1; i < last; ++i) {
		double const u = std::clamp(static_cast<double>(timecodes.TimeAtFrame(points[i].frame) - ta) / (tb - ta), 0.0, 1.0);
		double const power = std::pow(u, acceleration);
		for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
			auto const field = pose_channels[channel];
			double const a = starts[channel];
			double const b = ends[channel];
			if (std::abs(a + (b - a) * power - points[i].*field) > pose_tolerances[channel])
				return std::nullopt;
		}
	}
	return acceleration;
}

std::vector<PosePiece> FitPosePieces(std::vector<ResolvedPoint> const& points,
									 FitPiece const& position, agi::vfr::Framerate const& timecodes) {
	std::vector<PosePiece> fitted;
	std::vector<PosePiece> pending{{.i0 = position.i0, .i1 = position.i1}};
	while (!pending.empty()) {
		auto piece = pending.back();
		pending.pop_back();
		if (auto acceleration = FitPoseAcceleration(points, piece.i0, piece.i1, timecodes)) {
			piece.acceleration = *acceleration;
			fitted.push_back(piece);
			continue;
		}
		int const ta = timecodes.TimeAtFrame(points[piece.i0].frame);
		int const tb = timecodes.TimeAtFrame(points[piece.i1].frame);
		size_t worst = piece.i0 + 1;
		double worst_error = -1.0;
		for (size_t i = piece.i0 + 1; i < piece.i1; ++i) {
			int const time = timecodes.TimeAtFrame(points[i].frame);
			for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
				auto const field = pose_channels[channel];
				double const expected = InterpolateByTime(RoundedPose(points[piece.i0].*field),
														  RoundedPose(points[piece.i1].*field), ta, tb, time);
				double const error = std::abs(points[i].*field - expected) / pose_tolerances[channel];
				if (error > worst_error) {
					worst = i;
					worst_error = error;
				}
			}
		}
		pending.push_back({.i0 = worst, .i1 = piece.i1});
		pending.push_back({.i0 = piece.i0, .i1 = worst});
	}
	// A split chosen for the whole interval need not remain necessary after its
	// neighbours have been fitted. Prune transforms, never position boundaries.
	for (size_t i = 0; i + 1 < fitted.size();) {
		if (auto acceleration = FitPoseAcceleration(points, fitted[i].i0, fitted[i + 1].i1, timecodes)) {
			fitted[i].i1 = fitted[i + 1].i1;
			fitted[i].acceleration = *acceleration;
			fitted.erase(fitted.begin() + i + 1);
			if (i > 0)
				--i;
		}
		else
			++i;
	}
	return fitted;
}

struct SinglePoseFit {
	std::array<double, 3> first{}, last{};
	double acceleration = 1.0;
};

// This is an optional simplification of an already feasible scalar plan. A
// font/layout we cannot measure keeps the old plan, as does a curve which no
// tested single transform represents within the combined video-pixel budget.
std::optional<SinglePoseFit> FitSinglePoseByGeometry(
	AssFile const& file, std::vector<ResolvedPoint> const& points,
	FitPiece const& piece, ApplyPlanInput const& input,
	perspective::ForwardInput const& geometry, std::string const& position_tag,
	int event_start, int event_end) {
	int const first_time = input.timecodes.TimeAtFrame(points[piece.i0].frame);
	int const last_time = input.timecodes.TimeAtFrame(points[piece.i1].frame);
	if (last_time <= first_time || last_time <= event_start)
		return std::nullopt;
	struct Frame {
		double progress;
		perspective::Vec2 position;
		perspective::Quad target;
	};
	std::vector<Frame> frames;
	frames.reserve(piece.i1 - piece.i0 + 1);
	AssDialogue emitted;
	emitted.Start = event_start;
	emitted.End = event_end;
	emitted.Text = "{" + position_tag + "}x";
	auto const position_info = ExtractPositionInfo(emitted);
	auto sample_time = [&](ResolvedPoint const& point) {
		return input.timecodes.TimeAtFrame(point.frame);
	};
	auto const visible_begin = std::ranges::lower_bound(points, event_start, {}, sample_time);
	auto const visible_end = std::ranges::lower_bound(points, event_end, {}, sample_time);
	// At high frame rates, centisecond event rounding can give this event
	// samples outside its original knot indices. Validate all visible frames.
	for (auto it = visible_begin; it != visible_end; ++it) {
		auto const& point = *it;
		int const time = input.timecodes.TimeAtFrame(point.frame);
		auto state = geometry.state;
		state.position = {.x = point.x, .y = point.y};
		state.rotation_z = point.rot_deg;
		state.scale_x = point.scale_pct_x;
		state.scale_y = point.scale_pct_y;
		auto const target = perspective::ForwardQuad(geometry, state);
		if (!target)
			return std::nullopt;
		double x = 0.0, y = 0.0;
		if (!ResolveDialogueOriginInfo(file, position_info, emitted, time,
									   input.script_width, input.script_height, x, y))
			return std::nullopt;
		frames.push_back({.progress = std::clamp(static_cast<double>(time - first_time) / (last_time - first_time), 0.0, 1.0),
						  .position = {.x = x, .y = y},
						  .target = target.quad});
	}
	if (frames.empty())
		return std::nullopt;

	perspective::ForwardInput candidate_geometry = geometry;
	perspective::OutputCoordinateMapping const mapping{
		.scale_x = static_cast<double>(input.storage_width) / input.script_width,
		.scale_y = static_cast<double>(input.storage_height) / input.script_height};
	auto error = [&](SinglePoseFit const& fit) {
		double worst = 0.0;
		for (auto const& frame : frames) {
			double const u = std::pow(frame.progress, fit.acceleration);
			auto& state = candidate_geometry.state;
			state.position = frame.position;
			state.rotation_z = fit.first[0] + (fit.last[0] - fit.first[0]) * u;
			state.scale_x = fit.first[1] + (fit.last[1] - fit.first[1]) * u;
			state.scale_y = fit.first[2] + (fit.last[2] - fit.first[2]) * u;
			auto const residual = perspective::MeasurePerspectiveResidual(candidate_geometry, frame.target, mapping);
			if (!residual)
				return std::numeric_limits<double>::infinity();
			worst = std::max(worst, residual.max_error);
		}
		return worst;
	};
	SinglePoseFit endpoints;
	for (size_t c = 0; c < pose_channels.size(); ++c) {
		endpoints.first[c] = RoundedPose(points[piece.i0].*pose_channels[c]);
		endpoints.last[c] = RoundedPose(points[piece.i1].*pose_channels[c]);
	}
	auto constant = endpoints;
	constant.last = constant.first;
	if (error(constant) <= input.options.compact_epsilon)
		return constant;

	// Keep nearly constant channels at the reference pose instead of animating
	// tiny measured rotations alongside a real zoom. The geometry check, not
	// this scalar proposal, decides whether that omission is actually safe.
	auto simplified = endpoints;
	for (size_t c = 0; c < pose_channels.size(); ++c) {
		bool flat = true;
		for (size_t i = piece.i0; i <= piece.i1; ++i)
			flat = flat && std::abs(points[i].*pose_channels[c] - endpoints.first[c]) <= pose_tolerances[c];
		if (flat)
			simplified.last[c] = simplified.first[c];
	}
	if (error(simplified) <= input.options.compact_epsilon)
		return simplified;
	if (simplified.last != endpoints.last && error(endpoints) <= input.options.compact_epsilon)
		return endpoints;

	for (int attempt = 0; attempt < 2; ++attempt) {
		if (attempt == 1 && simplified.last == endpoints.last)
			break;
		auto fit = attempt == 0 ? simplified : endpoints;
		// A log-space regression proposes the common exponent. Weight by each
		// channel's excursion so a near-zero angle cannot dominate a real zoom.
		double numerator = 0.0, denominator = 0.0;
		for (size_t i = piece.i0 + 1; i < piece.i1; ++i) {
			double const u = static_cast<double>(input.timecodes.TimeAtFrame(points[i].frame) - first_time) / (last_time - first_time);
			if (u <= 0.0 || u >= 1.0)
				continue;
			double const log_u = std::log(u);
			for (size_t c = 0; c < pose_channels.size(); ++c) {
				double const delta = fit.last[c] - fit.first[c];
				if (delta == 0.0)
					continue;
				double const v = (points[i].*pose_channels[c] - fit.first[c]) / delta;
				if (v <= 0.0 || v >= 1.0)
					continue;
				numerator += delta * delta * log_u * std::log(v);
				denominator += delta * delta * log_u * log_u;
			}
		}
		if (denominator == 0.0)
			continue;
		double const estimate = numerator / denominator;
		if (!std::isfinite(estimate) || estimate <= 0.0)
			continue;
		auto trial = [&](double log_acceleration) {
			fit.acceleration = std::round(std::exp(log_acceleration) * 1e6) / 1e6;
			return fit.acceleration > 0.0 && std::isfinite(fit.acceleration)
					   ? error(fit)
					   : std::numeric_limits<double>::infinity();
		};
		double const center = std::log(estimate);
		if (trial(center) <= input.options.compact_epsilon)
			return fit;
		// Bounded local minimax refinement; failure simply keeps the proven
		// multi-transform plan. No accuracy contract depends on convergence.
		double lo = center - std::numbers::ln2, hi = center + std::numbers::ln2;
		constexpr double ratio = 0.6180339887498949;
		double left = hi - ratio * (hi - lo), right = lo + ratio * (hi - lo);
		double left_error = trial(left);
		if (left_error <= input.options.compact_epsilon)
			return fit;
		double right_error = trial(right);
		if (right_error <= input.options.compact_epsilon)
			return fit;
		for (int iteration = 0; iteration < 24; ++iteration) {
			if (left_error < right_error) {
				hi = right;
				right = left;
				right_error = left_error;
				left = hi - ratio * (hi - lo);
				left_error = trial(left);
			}
			else {
				lo = left;
				left = right;
				left_error = right_error;
				right = lo + ratio * (hi - lo);
				right_error = trial(right);
			}
			if (std::min(left_error, right_error) <= input.options.compact_epsilon)
				return fit;
		}
	}
	return std::nullopt;
}

std::string SinglePoseTags(SinglePoseFit const& fit, int first_time, int last_time,
						   std::array<double, 3> const& style, bool inline_rotation, bool inline_scale) {
	constexpr std::array<std::string_view, 3> names{"\\frz", "\\fscx", "\\fscy"};
	std::string tags, animated;
	for (size_t c = 0; c < names.size(); ++c) {
		bool const explicit_base = c == 0 ? inline_rotation : inline_scale;
		if (explicit_base || fit.first[c] != style[c])
			tags += std::string(names[c]) + FormatCoord(fit.first[c], 2);
		if (fit.last[c] != fit.first[c])
			animated += std::string(names[c]) + FormatCoord(fit.last[c], 2);
	}
	if (!animated.empty()) {
		tags += "\\t(" + std::to_string(first_time) + "," + std::to_string(last_time) + ",";
		if (fit.acceleration != 1.0)
			tags += FormatCoord(fit.acceleration, 6) + ",";
		tags += animated + ")";
	}
	return tags;
}

std::string CompactPoseTags(std::vector<ResolvedPoint> const& points,
							FitPiece const& position, agi::vfr::Framerate const& timecodes,
							int event_start, int event_end, std::array<double, 3> const& style, bool inline_rotation, bool inline_scale,
							AssFile const& file, ApplyPlanInput const& input, std::optional<perspective::ForwardInput> const& geometry,
							std::string const& position_tag) {
	auto const pieces = FitPosePieces(points, position, timecodes);
	if (pieces.size() > 1 && geometry) {
		if (auto const fit = FitSinglePoseByGeometry(file, points, position, input, *geometry,
													 position_tag, event_start, event_end))
			return SinglePoseTags(*fit, timecodes.TimeAtFrame(points[position.i0].frame) - event_start,
								  timecodes.TimeAtFrame(points[position.i1].frame) - event_start, style, inline_rotation, inline_scale);
	}
	size_t initial = position.i0;
	for (auto const& piece : pieces) {
		if (timecodes.TimeAtFrame(points[piece.i1].frame) > event_start)
			break;
		initial = piece.i1;
	}
	constexpr std::array<std::string_view, 3> names{"\\frz", "\\fscx", "\\fscy"};
	std::array<double, 3> current{};
	std::string tags;
	for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
		current[channel] = RoundedPose(points[initial].*pose_channels[channel]);
		bool const explicit_base = channel == 0 ? inline_rotation : inline_scale;
		if (explicit_base || current[channel] != style[channel])
			tags += std::string(names[channel]) + FormatCoord(current[channel], 2);
	}
	for (auto const& piece : pieces) {
		int const t2 = timecodes.TimeAtFrame(points[piece.i1].frame) - event_start;
		if (t2 <= 0 || piece.i0 == piece.i1)
			continue; // t2=0 means the full ASS event, not an already completed ramp.
		std::string targets;
		for (size_t channel = 0; channel < pose_channels.size(); ++channel) {
			double const target = RoundedPose(points[piece.i1].*pose_channels[channel]);
			if (target != current[channel])
				targets += std::string(names[channel]) + FormatCoord(target, 2);
			current[channel] = target;
		}
		if (targets.empty())
			continue;
		// Keep the original clock even when this event starts after the knot.
		// Rebasing a power curve to a new value at t=0 changes its acceleration.
		int const t1 = timecodes.TimeAtFrame(points[piece.i0].frame) - event_start;
		tags += "\\t(" + std::to_string(t1) + "," + std::to_string(t2) + ",";
		if (piece.acceleration != 1.0)
			tags += FormatCoord(piece.acceleration, 6) + ",";
		tags += targets + ")";
	}
	return tags;
}

struct FittedKnots {
	std::vector<double> x, y;
};

// Solves both axes of the continuous piecewise-linear least-squares fit.
// `knots` are ascending sample indices; the unknowns are the knot values, the
// basis functions are the hats over adjacent knot times, so the normal
// matrix is tridiagonal (symmetric positive definite; Thomas, no pivoting)
// and position continuity between segments holds by construction. Timecodes
// are cached once per run, and both axes share assembly and factorization.
FittedKnots FitKnotPositions(std::vector<ResolvedPoint> const& points,
							 std::vector<size_t> const& knots, std::vector<int> const& times) {
	size_t const K = knots.size();
	std::vector<double> diag(K, 0.0), off(K - 1, 0.0);
	FittedKnots fit{.x = std::vector<double>(K), .y = std::vector<double>(K)};
	size_t seg = 0;
	for (size_t s = knots.front(); s <= knots.back(); ++s) {
		while (knots[seg + 1] < s)
			++seg;
		int const time = times[s - knots.front()];
		int const ta = times[knots[seg] - knots.front()];
		int const tb = times[knots[seg + 1] - knots.front()];
		double wa = 1.0, wb = 0.0;
		if (tb > ta) {
			wb = static_cast<double>(time - ta) / static_cast<double>(tb - ta);
			wa = 1.0 - wb;
		}
		diag[seg] += wa * wa;
		diag[seg + 1] += wb * wb;
		off[seg] += wa * wb;
		fit.x[seg] += wa * points[s].x;
		fit.x[seg + 1] += wb * points[s].x;
		fit.y[seg] += wa * points[s].y;
		fit.y[seg + 1] += wb * points[s].y;
	}
	for (size_t i = 1; i < K; ++i) {
		double const m = off[i - 1] / diag[i - 1];
		diag[i] -= m * off[i - 1];
		fit.x[i] -= m * fit.x[i - 1];
		fit.y[i] -= m * fit.y[i - 1];
	}
	fit.x[K - 1] /= diag[K - 1];
	fit.y[K - 1] /= diag[K - 1];
	for (size_t i = K - 1; i-- > 0;) {
		fit.x[i] = (fit.x[i] - off[i] * fit.x[i + 1]) / diag[i];
		fit.y[i] = (fit.y[i] - off[i] * fit.y[i + 1]) / diag[i];
	}
	return fit;
}

// Max storage-space deviation of the samples from the fitted spline over
// `knots`; optionally reports the worst sample index.
double MaxDeviation(std::vector<ResolvedPoint> const& points,
					std::vector<size_t> const& knots,
					FittedKnots const& fit, std::vector<int> const& times,
					double inv_scale_x, double inv_scale_y,
					size_t *worst = nullptr) {
	double worst_d = -1.0;
	size_t worst_s = knots.front();
	size_t seg = 0;
	for (size_t s = knots.front(); s <= knots.back(); ++s) {
		while (knots[seg + 1] < s)
			++seg;
		int const time = times[s - knots.front()];
		int const ta = times[knots[seg] - knots.front()];
		int const tb = times[knots[seg + 1] - knots.front()];
		double wa = 1.0, wb = 0.0;
		if (tb > ta) {
			wb = static_cast<double>(time - ta) / static_cast<double>(tb - ta);
			wa = 1.0 - wb;
		}
		double const ex = fit.x[seg] * wa + fit.x[seg + 1] * wb;
		double const ey = fit.y[seg] * wa + fit.y[seg + 1] * wb;
		double const d = std::hypot(
			(points[s].x - ex) * inv_scale_x,
			(points[s].y - ey) * inv_scale_y);
		if (d > worst_d) {
			worst_d = d;
			worst_s = s;
		}
	}
	if (worst)
		*worst = worst_s;
	return worst_d;
}

// Least squares can fail a maximum-error budget even when another single
// segment fits. Project its endpoints onto each sample's storage-pixel error
// disk before adding knots. This bounded search only proposes a candidate;
// failure to converge leaves the existing partitioner unchanged.
std::optional<FittedKnots> FitSinglePositionCandidate(
	std::vector<ResolvedPoint> const& points, std::vector<size_t> const& knots,
	FittedKnots const& least_squares, std::vector<int> const& times,
	double eps_storage, double inv_scale_x, double inv_scale_y) {
	double const duration = static_cast<double>(times.back()) - times.front();
	if (eps_storage <= 0.0 || duration <= 0.0)
		return std::nullopt;
	// Any feasible line differs from the chord through the observed endpoints
	// by at most epsilon. A sample more than 2*epsilon from that chord therefore
	// proves this run needs more than one segment, avoiding futile iterations.
	for (size_t s = knots.front() + 1; s < knots.back(); ++s) {
		double const u = (times[s - knots.front()] - times.front()) / duration;
		double const dx = (points[s].x - (1.0 - u) * points[knots.front()].x - u * points[knots.back()].x) * inv_scale_x;
		double const dy = (points[s].y - (1.0 - u) * points[knots.front()].y - u * points[knots.back()].y) * inv_scale_y;
		if (std::hypot(dx, dy) > 2.0 * eps_storage)
			return std::nullopt;
	}
	auto candidate = least_squares;
	// Project just inside the budget so convergence at a disk boundary does
	// not depend on floating-point equality. Acceptance still uses the exact
	// original budget, including the caller's coordinate-rounding reserve.
	double const radius = eps_storage * (1.0 - 1e-6);
	for (int iteration = 0; iteration < 128; ++iteration) {
		for (size_t s = knots.front(); s <= knots.back(); ++s) {
			double const wb = (times[s - knots.front()] - times.front()) / duration;
			double const wa = 1.0 - wb;
			double const dx = (wa * candidate.x[0] + wb * candidate.x[1] - points[s].x) * inv_scale_x;
			double const dy = (wa * candidate.y[0] + wb * candidate.y[1] - points[s].y) * inv_scale_y;
			double const distance = std::hypot(dx, dy);
			if (distance <= radius)
				continue;
			double const correction = (1.0 - radius / distance) / (wa * wa + wb * wb);
			candidate.x[0] -= wa * correction * dx / inv_scale_x;
			candidate.x[1] -= wb * correction * dx / inv_scale_x;
			candidate.y[0] -= wa * correction * dy / inv_scale_y;
			candidate.y[1] -= wb * correction * dy / inv_scale_y;
		}
		if (MaxDeviation(points, knots, candidate, times, inv_scale_x, inv_scale_y) <= eps_storage)
			return candidate;
	}
	return std::nullopt;
}

// An endpoint interpolant supplies a feasible starting partition. Splitting
// all curved pieces before the first spline refit avoids repeatedly solving
// the whole run as one knot at a time is added to a long curved trajectory.
std::vector<size_t> InitialCompactKnots(
	std::vector<ResolvedPoint> const& points,
	std::pair<size_t, size_t> const& run, std::vector<int> const& times,
	double eps_storage, double inv_scale_x, double inv_scale_y) {
	std::vector<size_t> knots{run.first};
	std::vector<std::pair<size_t, size_t>> pending{run};
	while (!pending.empty()) {
		auto const [first, last] = pending.back();
		pending.pop_back();
		double worst_error = eps_storage;
		size_t worst = first;
		for (size_t i = first + 1; i < last; ++i) {
			double const x = InterpolateByTime(points[first].x, points[last].x,
											   times[first - run.first], times[last - run.first],
											   times[i - run.first]);
			double const y = InterpolateByTime(points[first].y, points[last].y,
											   times[first - run.first], times[last - run.first],
											   times[i - run.first]);
			double const error = std::hypot((points[i].x - x) * inv_scale_x,
											(points[i].y - y) * inv_scale_y);
			if (error > worst_error) {
				worst = i;
				worst_error = error;
			}
		}
		if (worst == first) {
			knots.push_back(last);
		}
		else {
			pending.emplace_back(worst, last);
			pending.emplace_back(first, worst);
		}
	}
	return knots;
}

// Adaptive fit over one forced run: start with a single segment, solve for
// knot values, and add knots at the frame of maximum deviation — measured in
// storage pixels, so the threshold means on-screen error — until EVERY frame
// is within `eps_storage` or every frame is a knot (the exact interpolant,
// which the loop reaches first for finite data). When the worst frame is
// already a knot, the nearest non-knot neighbour is added instead:
// least-squares residuals peak at knots, and only added freedom next to them
// can shrink those.
//
// If both the least-squares line and a bounded single-segment feasibility
// search fail, seed the refinement with a feasible interpolant
// instead of growing every curved region serially. Keep that interpolant as
// a fallback: least-squares minimizes total error, not the maximum, so fitting
// and pruning must never leave more events than that already feasible fit.
// A backward-elimination pass then greedily drops every interior knot whose
// removal keeps all frames within eps. The forward loop alone over-splits
// badly on curved paths (residuals shuffle around as knots are added, so it
// keeps adding until nearly every frame is a knot); pruning that feasible
// set reduces unnecessary knots while retaining the per-frame epsilon
// guarantee. The endpoint fallback also keeps that guarantee by construction.
std::vector<FitPiece> FitRunPieces(std::vector<ResolvedPoint> const& points,
								   std::pair<size_t, size_t> const& run,
								   double eps_storage, double inv_scale_x,
								   double inv_scale_y,
								   agi::vfr::Framerate const& timecodes) {
	std::vector<FitPiece> pieces;
	size_t const n = run.second - run.first + 1;
	if (n < 2) {
		FitPiece solo;
		solo.i0 = solo.i1 = run.first;
		solo.x0 = solo.x1 = points[run.first].x;
		solo.y0 = solo.y1 = points[run.first].y;
		pieces.push_back(solo);
		return pieces;
	}

	std::vector<int> times(n);
	for (size_t i = run.first; i <= run.second; ++i)
		times[i - run.first] = timecodes.TimeAtFrame(points[i].frame);
	std::vector<size_t> knots{run.first, run.second};
	std::vector<size_t> initial_knots;
	FittedKnots fit;
	auto is_knot = [&](size_t s) {
		return std::ranges::binary_search(knots, s);
	};
	while (true) {
		fit = FitKnotPositions(points, knots, times);
		size_t worst = run.first;
		double const worst_d =
			MaxDeviation(points, knots, fit, times, inv_scale_x, inv_scale_y,
						 &worst);
		if (worst_d <= eps_storage || knots.size() >= n)
			break;
		if (initial_knots.empty()) {
			if (auto single = FitSinglePositionCandidate(points, knots, fit, times,
														 eps_storage, inv_scale_x, inv_scale_y)) {
				fit = std::move(*single);
				break;
			}
			initial_knots = InitialCompactKnots(points, run, times,
												eps_storage, inv_scale_x, inv_scale_y);
			if (initial_knots.size() > knots.size()) {
				knots = initial_knots;
				continue;
			}
		}
		size_t add = worst;
		if (is_knot(worst)) {
			// The worst frame is itself a knot: its least-squares residual
			// can only shrink by giving the fit more freedom next to it, so
			// add the nearest frame that is not a knot yet.
			add = SIZE_MAX;
			for (size_t r = 1; r <= n; ++r) {
				if (worst >= run.first + r && !is_knot(worst - r)) {
					add = worst - r;
					break;
				}
				if (worst + r <= run.second && !is_knot(worst + r)) {
					add = worst + r;
					break;
				}
			}
			if (add == SIZE_MAX)
				break; // every frame is a knot already
		}
		knots.insert(std::ranges::lower_bound(knots, add), add);
	}

	bool removed_any = true;
	while (removed_any && knots.size() > 2) {
		removed_any = false;
		size_t k = 1;
		while (k + 1 < knots.size()) {
			std::vector<size_t> cand(knots);
			cand.erase(cand.begin() + k);
			auto candidate_fit = FitKnotPositions(points, cand, times);
			if (MaxDeviation(points, cand, candidate_fit, times, inv_scale_x,
							 inv_scale_y) <= eps_storage) {
				knots = std::move(cand);
				fit = std::move(candidate_fit);
				removed_any = true;
				// A new knot slides into slot k; try it too.
			}
			else {
				++k;
			}
		}
	}

	if (!initial_knots.empty() && initial_knots.size() < knots.size()) {
		knots = std::move(initial_knots);
		fit.x.resize(knots.size());
		fit.y.resize(knots.size());
		for (size_t k = 0; k < knots.size(); ++k) {
			fit.x[k] = points[knots[k]].x;
			fit.y[k] = points[knots[k]].y;
		}
	}

	for (size_t k = 0; k + 1 < knots.size(); ++k) {
		FitPiece p;
		p.i0 = knots[k];
		p.i1 = knots[k + 1];
		p.x0 = fit.x[k];
		p.y0 = fit.y[k];
		p.x1 = fit.x[k + 1];
		p.y1 = fit.y[k + 1];
		pieces.push_back(p);
	}
	return pieces;
}

} // namespace

MotionTrackApplyPlan BuildPositionApplyPlan(
	AssFile const& file,
	std::vector<AssDialogue *> const& targets,
	ApplyPlanInput const& input, bool frame_parts) {
	MotionTrackApplyPlan plan;

	if (input.model != TrackModel::Translation && input.model != TrackModel::Similarity) {
		plan.status = ApplyPlanStatus::UnsupportedModel;
		plan.message = "only Translation and Similarity trajectories can be "
					   "applied";
		return plan;
	}
	if (input.video_frame_count <= 0) {
		plan.status = ApplyPlanStatus::InvalidInput;
		plan.message = "no video frames";
		return plan;
	}
	if (input.storage_width <= 0 || input.storage_height <= 0 || input.script_width <= 0 || input.script_height <= 0) {
		plan.status = ApplyPlanStatus::InvalidInput;
		plan.message = "missing resolution information";
		return plan;
	}

	double const scale_x =
		double(input.script_width) / double(input.storage_width);
	double const scale_y =
		double(input.script_height) / double(input.storage_height);
	// Compact's deviation threshold is defined in storage pixels; deviations
	// are measured in script space, so convert per axis for the comparison.
	double const inv_scale_x = 1.0 / scale_x;
	double const inv_scale_y = 1.0 / scale_y;

	// \fad composition (options.apply_fad): project the session-detected
	// interval's full-visibility boundaries once per plan, with the event
	// semantics BuildAssFadeTiming uses for its event bounds -- a frame is
	// fully displayed over its [START, END) window, so the visibility
	// plateau is exactly [START(fade-in full), END(fade-out full)]. Each
	// applied line then clamps these global anchors into its OWN [Start,
	// End] (below): the old composition derived the durations from the
	// fitted event span while Start/End stayed the source line's, so a line
	// shorter than / offset from the fitted ramp got wrong, even spurious,
	// fades.
	struct FadeAnchors {
		bool has_in = false;
		int in_full_ms = 0; // visibility reaches the plateau at this time
		bool has_out = false;
		int out_full_ms = 0; // visibility starts dropping at this time
	};
	FadeAnchors fade_anchors;
	bool const fade_active =
		input.options.apply_fad && input.fade_interval && (input.fade_interval->fade_in.detected || input.fade_interval->fade_out.detected);
	if (fade_active) {
		auto const& interval = *input.fade_interval;
		if (interval.fade_in.detected) {
			fade_anchors.has_in = true;
			fade_anchors.in_full_ms = input.timecodes.TimeAtFrame(
				interval.fade_in.full_visibility_frame,
				agi::vfr::Time::START);
		}
		if (interval.fade_out.detected) {
			fade_anchors.has_out = true;
			fade_anchors.out_full_ms = input.timecodes.TimeAtFrame(
				interval.fade_out.full_visibility_frame,
				agi::vfr::Time::END);
		}
	}

	std::vector<PlannedLine> planned_lines;
	std::vector<LineUncovered> uncovered_lines;
	size_t event_count = 0;
	bool any_uncovered = false;

	for (AssDialogue *line : targets) {
		if (!line || line->Comment)
			continue;

		auto const line_start = int(line->Start);
		auto const line_end = int(line->End);
		if (line_end <= line_start)
			continue;

		int const lf = std::clamp(
			input.timecodes.FrameAtTime(line_start, agi::vfr::Time::START),
			0, input.video_frame_count - 1);
		int const ll = std::clamp(
			input.timecodes.FrameAtTime(line_end, agi::vfr::Time::END),
			0, input.video_frame_count - 1);

		FrameInterval domain;
		domain.first = std::max(lf, input.direction_domain.first);
		domain.last = std::min(ll, input.direction_domain.last);
		if (domain.first > domain.last)
			continue;

		int const dom_start_ms = std::max(
			line_start,
			input.timecodes.TimeAtFrame(domain.first, agi::vfr::Time::START));
		int const dom_end_ms = std::min(
			line_end,
			input.timecodes.TimeAtFrame(domain.last, agi::vfr::Time::END));
		if (dom_end_ms <= dom_start_ms)
			continue;

		std::vector<SourceTimeTag> source_time_tags;
		std::string animation_error = CollectSourceTimeTags(*line, source_time_tags);
		PositionInfo info = ExtractPositionInfo(*line);
		if (animation_error.empty() && input.model == TrackModel::Similarity)
			animation_error = ResolveSimilarityBase(file, *line, input, info);
		if (!animation_error.empty()) {
			plan.status = ApplyPlanStatus::InvalidInput;
			plan.message = std::move(animation_error);
			return plan;
		}
		// The full geometry writer handles origins and clips after this
		// planner has resolved temporal coverage and per-frame boundaries.

		double origin_x = 0.0, origin_y = 0.0;
		if (!ResolveDialogueOriginInfo(file, info, *line, input.seed_time_ms,
									   input.script_width, input.script_height,
									   origin_x, origin_y)) {
			// Every \move spelling resolves (reversed windows are swapped
			// in the scanner, and the whole-event fallback covers
			// everything else), so this is an invariant guard rather than
			// a per-line rejection path: if it ever fires, bail loudly
			// instead of applying from a defaulted origin.
			plan.status = ApplyPlanStatus::InvalidInput;
			plan.message = "line position could not be resolved";
			return plan;
		}

		// Base for similarity tag emission: \frz/\fscx/\fscy REPLACE the
		// style values, so the styled look must be composed in. Inline tags
		// override the style and become the effective base, so user styling
		// survives the apply.
		double style_scale_x = 100.0, style_scale_y = 100.0, style_angle = 0.0;
		double base_angle = 0.0, base_scale_x = 100.0, base_scale_y = 100.0;
		bool inline_frz = false, inline_scale = false;
		double style_outline_w = 0.0, style_shadow_w = 0.0;
		PositionInfo pose_info;
		if (input.model == TrackModel::Similarity) {
			pose_info = info;
			inline_frz = pose_info.has_frz;
			inline_scale = pose_info.has_fscx || pose_info.has_fscy;
			if (AssStyle const *st =
					const_cast<AssFile&>(file).GetStyle(line->Style)) {
				style_scale_x = st->scalex;
				style_scale_y = st->scaley;
				style_angle = st->angle;
				style_outline_w = st->outline_w;
				style_shadow_w = st->shadow_w;
			}
			base_angle = pose_info.has_frz ? pose_info.frz : style_angle;
			base_scale_x = pose_info.has_fscx ? pose_info.fscx : style_scale_x;
			base_scale_y = pose_info.has_fscy ? pose_info.fscy : style_scale_y;
		}

		// Resolves one sample into the line anchor's script-space position
		// (and, for similarity, the emitted transform values). Translation
		// moves the anchor by the trajectory delta; similarity carries the
		// anchor with the full pose — its offset from the ROI seed center is
		// rotated and scaled about the current center, so a subtitle placed
		// off-center on the object stays glued to it as the object turns.
		auto resolve_point = [&](TrackSample const& s, int frame,
								 bool held) {
			ResolvedPoint p;
			p.frame = frame;
			p.held = held;
			if (input.model == TrackModel::Translation) {
				p.x = origin_x + (s.center_x - input.origin_center_x) * scale_x;
				p.y = origin_y + (s.center_y - input.origin_center_y) * scale_y;
				return p;
			}
			double const m00 = s.transform.matrix[0];
			double const m01 = s.transform.matrix[1];
			double const m10 = s.transform.matrix[3];
			double const m11 = s.transform.matrix[4];
			double const ox = origin_x / scale_x - input.origin_center_x;
			double const oy = origin_y / scale_y - input.origin_center_y;
			p.x = (s.center_x + m00 * ox + m01 * oy) * scale_x;
			p.y = (s.center_y + m10 * ox + m11 * oy) * scale_y;
			double const theta_cw = std::atan2(m10, m00);
			double const scale = std::hypot(m00, m10);
			// \frz and the style Angle are counterclockwise-positive degrees;
			// theta is clockwise-positive in the Y-down image space, so they
			// compose with opposite signs.
			p.rot_deg = base_angle - theta_cw * 180.0 / std::numbers::pi;
			p.scale_pct_x = base_scale_x * scale;
			p.scale_pct_y = base_scale_y * scale;
			// Local stretch the transform applies along the script axes at
			// the anchor: |M * e_x| and |M * e_y| of the seed-relative linear
			// part. Uniform (== scale) for a similarity pose; carried per
			// axis so border/shadow/blur growth composes with anisotropic
			// bases.
			// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
			// (ISC license) -- growth.
			p.growth_x = std::hypot(m00, m10);
			p.growth_y = std::hypot(m01, m11);
			return p;
		};

		// Resolve every frame in the domain; interior Failed gaps hold the
		// previous Ok pose only when bounded by Ok on both sides.
		std::vector<ResolvedPoint> points;
		std::vector<UncoveredRange> uncovered;
		for (int f = domain.first; f <= domain.last; ++f) {
			if (auto const *ok = FindOkAt(input.samples, f)) {
				points.push_back(resolve_point(*ok, f, false));
				continue;
			}
			auto const *s = FindSample(input.samples, f);
			if (s && s->status == TrackStatus::Failed) {
				auto const *before = FindNearestOk(
					input.samples, f - 1, -1, input.decode_interval);
				auto const *after = FindNearestOk(
					input.samples, f + 1, +1, input.decode_interval);
				if (before && after) {
					points.push_back(resolve_point(*before, f, true));
					continue;
				}
			}
			if (!uncovered.empty() && uncovered.back().last == f - 1)
				uncovered.back().last = f;
			else
				uncovered.push_back(UncoveredRange{f, f});
		}

		if (!uncovered.empty()) {
			any_uncovered = true;
			uncovered_lines.push_back(LineUncovered{line, std::move(uncovered)});
			continue;
		}
		if (points.empty())
			continue;

		// Split points into forced runs at hold boundaries (held <-> tracked
		// transitions): segments must never interpolate across a gap hold.
		std::vector<std::pair<size_t, size_t>> runs;
		size_t run_start = 0;
		for (size_t i = 1; i < points.size(); ++i) {
			bool const boundary =
				points[i].held != points[i - 1].held;
			if (boundary) {
				runs.emplace_back(run_start, i - 1);
				run_start = i;
			}
		}
		runs.emplace_back(run_start, points.size() - 1);

		// Optional local-linear smoothing inside each run, before any
		// simplification sees the points. Position-only by construction —
		// smoothing a similarity pose would desynchronize position from
		// rotation/scale, so it stays a Translation feature.
		if (input.model == TrackModel::Translation)
			for (auto const& run : runs)
				SmoothRun(points, run.first, run.second,
						  input.options.smooth_frames);

		// Optional stabilization chain (see StabilizeRun): similarity pose
		// channels always, positions in Exact mode. Never touches held runs
		// (constant by construction) and never crosses hold boundaries; the
		// Compact translation fit is deliberately left to its own machinery.
		if (input.options.stabilization.enable) {
			auto const& stab = input.options.stabilization;
			int const seed_frame = std::clamp(
				input.timecodes.FrameAtTime(input.seed_time_ms),
				domain.first, domain.last);
			for (auto const& run : runs) {
				if (points[run.first].held)
					continue;
				size_t const n = run.second - run.first + 1;
				if (n < 2)
					continue;
				int seed_index = -1;
				for (size_t i = run.first; i <= run.second; ++i)
					if (points[i].frame == seed_frame)
						seed_index = int(i - run.first);

				// Unwrap-then-stabilize for the angle channel; the emitted
				// \frz stays rendering-equivalent modulo 360 deg.
				auto stabilize_channel = [&](double ResolvedPoint::*field,
											 double floor, bool ratio) {
					std::vector<double> v = ChannelOf(points, run, field);
					if (field == &ResolvedPoint::rot_deg)
						v = UnwrapDegrees(v);
					StabilizeRun(v, seed_index, floor, ratio);
					for (size_t i = run.first; i <= run.second; ++i)
						points[i].*field = v[i - run.first];
				};

				if (input.model == TrackModel::Similarity) {
					stabilize_channel(&ResolvedPoint::rot_deg,
									  stab.angle_floor_deg, false);
					stabilize_channel(&ResolvedPoint::scale_pct_x,
									  stab.scale_floor, true);
					stabilize_channel(&ResolvedPoint::scale_pct_y,
									  stab.scale_floor, true);
				}
				if (input.options.mode == ApplyMode::Exact) {
					stabilize_channel(&ResolvedPoint::x,
									  stab.position_floor_storage_px * scale_x,
									  false);
					stabilize_channel(&ResolvedPoint::y,
									  stab.position_floor_storage_px * scale_y,
									  false);
				}
			}
		}
		if (input.model == TrackModel::Similarity &&
			input.options.mode == ApplyMode::Compact)
			UnwrapCompactAngles(points);

		std::vector<FitPiece> pieces;
		if (input.options.mode == ApplyMode::Exact) {
			// Merge consecutive frames whose rounded values coincide, but
			// never across a hold boundary: the forced runs are hard cut
			// points for fitting, so nothing interpolates across a gap hold.
			// (A post-pass below still folds adjacent parts whose emitted
			// text is identical, which renders the same at every frame
			// time.)
			// Similarity groups additionally require the rounded pose
			// (\frz/\fscx/\fscy) to match.
			auto rounded2 = [](double v) {
				return std::round(v * 100.0) / 100.0;
			};
			auto same_rounded = [&](size_t a, size_t b) {
				if (frame_parts)
					return false;
				int const decimals = input.options.position_decimals;
				bool const same_pos =
					FormatCoord(points[a].x, decimals) == FormatCoord(points[b].x, decimals) &&
					FormatCoord(points[a].y, decimals) == FormatCoord(points[b].y, decimals);
				if (!same_pos || input.model != TrackModel::Similarity)
					return same_pos;
				return rounded2(points[a].rot_deg) == rounded2(points[b].rot_deg) && rounded2(points[a].scale_pct_x) == rounded2(points[b].scale_pct_x) && rounded2(points[a].scale_pct_y) == rounded2(points[b].scale_pct_y);
			};
			for (auto const& run : runs) {
				size_t group_start = run.first;
				for (size_t i = run.first + 1; i <= run.second; ++i) {
					if (!same_rounded(i, i - 1)) {
						FitPiece p;
						p.i0 = group_start;
						p.i1 = i - 1;
						p.x0 = p.x1 = points[group_start].x;
						p.y0 = p.y1 = points[group_start].y;
						pieces.push_back(p);
						group_start = i;
					}
				}
				FitPiece p;
				p.i0 = group_start;
				p.i1 = run.second;
				p.x0 = p.x1 = points[group_start].x;
				p.y0 = p.y1 = points[group_start].y;
				pieces.push_back(p);
			}
		}
		else {
			// Continuous piecewise-linear least-squares fit per forced run:
			// each segment is uniform-velocity, adjacent segments share their
			// boundary position exactly (no artificial velocity steps at
			// simplification vertices), and knots are added until the
			// storage-space deviation is within the threshold.
			// Each rounded endpoint can move by half a coordinate quantum on
			// both axes. Reserve that storage-space error before fitting; when
			// it consumes the budget, try an exact fit and validate its emitted
			// coordinates below, since integral positions may still be exact.
			double const rounding_error = 0.5 * std::pow(10.0, -input.options.position_decimals) *
										  std::hypot(inv_scale_x, inv_scale_y);
			double const fit_epsilon = std::max(0.0, input.options.compact_epsilon - rounding_error);
			for (auto const& run : runs) {
				auto run_pieces = FitRunPieces(
					points, run, fit_epsilon,
					inv_scale_x, inv_scale_y, input.timecodes);
				pieces.insert(pieces.end(),
							  std::make_move_iterator(run_pieces.begin()),
							  std::make_move_iterator(run_pieces.end()));
			}
		}

		std::optional<perspective::ForwardInput> compact_geometry;
		if (input.model == TrackModel::Similarity && input.options.mode == ApplyMode::Compact && !frame_parts) {
			double max_scale_x = 0.0;
			for (auto const& point : points)
				max_scale_x = std::max({max_scale_x, point.scale_pct_x, RoundedPose(point.scale_pct_x)});
			compact_geometry = PrepareCompactGeometry(file, *line, input, max_scale_x);
		}

		// Assemble parts: optional preserved prefix, covered pieces, optional
		// preserved suffix.
		PlannedLine pl;
		pl.source = line;

		int cursor = dom_start_ms;
		if (CentisecondRounded(dom_start_ms) > CentisecondRounded(line_start)) {
			PlannedLinePart prefix;
			prefix.start_ms = line_start;
			prefix.end_ms = dom_start_ms;
			prefix.text = line->Text;
			prefix.covered = false;
			pl.parts.push_back(std::move(prefix));
		}

		for (size_t p = 0; p < pieces.size(); ++p) {
			auto const& piece = pieces[p];
			PlannedLinePart part;
			part.start_ms = p == 0 ? dom_start_ms
								   : input.timecodes.TimeAtFrame(points[piece.i0].frame,
																 agi::vfr::Time::START);
			part.end_ms = p + 1 == pieces.size()
							  ? dom_end_ms
							  : input.timecodes.TimeAtFrame(points[piece.i1].frame + 1,
															agi::vfr::Time::START);
			part.start_ms = std::clamp(part.start_ms, cursor, dom_end_ms);
			part.end_ms = std::clamp(part.end_ms, part.start_ms, dom_end_ms);
			cursor = part.end_ms;
			// Preserve the plan's raw frame boundaries while using ASS's
			// centisecond event clock for Compact windows and knot rebasing.
			bool const compact = input.options.mode == ApplyMode::Compact && !frame_parts;
			int const event_start_ms = compact ? CentisecondRounded(part.start_ms) : part.start_ms;
			int const event_end_ms = compact ? CentisecondRounded(part.end_ms) : part.end_ms;

			if (input.model == TrackModel::Similarity &&
				input.options.mode == ApplyMode::Compact) {
				// Position curvature alone determines event boundaries. Pose may
				// use one accelerated transform or a sequence within this event.
				ResolvedPoint const& first = points[piece.i0];
				ResolvedPoint const& last = points[piece.i1];
				int const ta = input.timecodes.TimeAtFrame(first.frame);
				int const tb = input.timecodes.TimeAtFrame(last.frame);
				int const dur = event_end_ms - event_start_ms;
				int const t1 = std::clamp(
					ta - event_start_ms, 0, std::max(0, dur - 1));
				int const t2 = std::clamp(
					tb - event_start_ms, t1 + 1, std::max(t1 + 1, dur));

				int const dec = input.options.position_decimals;
				double const ex0 = InterpolateByTime(
					piece.x0, piece.x1, ta, tb, event_start_ms);
				double const ey0 = InterpolateByTime(
					piece.y0, piece.y1, ta, tb, event_start_ms);
				double const ex1 = piece.x1;
				double const ey1 = piece.y1;
				std::string position_tag;
				if (FormatCoord(ex0, dec) == FormatCoord(ex1, dec) &&
					FormatCoord(ey0, dec) == FormatCoord(ey1, dec)) {
					position_tag = "\\pos(" + FormatCoord(ex0, dec) + "," +
								   FormatCoord(ey0, dec) + ")";
				}
				else {
					position_tag = "\\move(" + FormatCoord(ex0, dec) + "," +
								   FormatCoord(ey0, dec) + "," + FormatCoord(ex1, dec) +
								   "," + FormatCoord(ey1, dec) + "," + std::to_string(t1) +
								   "," + std::to_string(t2) + ")";
				}

				part.text = ReplaceTagsDropping(line->Text, position_tag, true, true);
				auto const pose_tags = CompactPoseTags(points, piece, input.timecodes,
													   event_start_ms, event_end_ms, {style_angle, style_scale_x, style_scale_y}, inline_frz, inline_scale,
													   file, input, compact_geometry, position_tag);
				if (!pose_tags.empty())
					part.text = AppendTagToFirstBlock(part.text, pose_tags, true);
				part.covered = true;
				part.x0 = ex0;
				part.y0 = ey0;
				part.x1 = ex1;
				part.y1 = ey1;
				pl.parts.push_back(std::move(part));
				continue;
			}

			if (input.model == TrackModel::Similarity) {
				// Exact parts are static by construction: one constant pose
				// per part. Transform tags are emitted only when they carry
				// information — identity tracked pose with no inline base
				// renders correctly from the style alone, so a pure
				// translation yields plain \pos parts (the inline tags were
				// stripped, so an inline base is always re-emitted).
				ResolvedPoint const& pt = points[piece.i0];
				auto rounded2 = [](double v) {
					return std::round(v * 100.0) / 100.0;
				};
				bool const emit_frz =
					inline_frz || rounded2(pt.rot_deg) != rounded2(style_angle);
				bool const emit_scale =
					inline_scale || rounded2(pt.scale_pct_x) != rounded2(style_scale_x) || rounded2(pt.scale_pct_y) != rounded2(style_scale_y);
				int const dec_s = input.options.position_decimals;
				// The position and transform components are spliced
				// separately: \pos must be able to lead the kept bytes
				// (libass pos/move first-wins), while the transform tags
				// keep their historical append-after placement (last-wins;
				// a leading transform would let a surviving kept tag
				// override it, and the drop pass already removes the whole
				// inline \frz/\fr/\fscx/\fscy family so none can).
				std::string const pos_tag =
					"\\pos(" + FormatCoord(pt.x, dec_s) + "," + FormatCoord(pt.y, dec_s) + ")";
				std::string transforms;
				if (emit_frz)
					transforms += "\\frz" + FormatCoord(pt.rot_deg, 2);
				if (emit_scale)
					transforms += "\\fscx" + FormatCoord(pt.scale_pct_x, 2) + "\\fscy" + FormatCoord(pt.scale_pct_y, 2);

				// Border/shadow/blur growth compensation: scale the composed
				// base (style + inline override) with the transform's local
				// stretch so outline width, shadow distance and blur radius
				// follow the object as it zooms. Per-axis tags when the user
				// styled the axes separately, the geometric mean otherwise.
				// A family is emitted unless the rounded compensated value
				// equals both the composed base and the style default:
				// identity transforms and zero bases emit nothing (the
				// pre-feature output is unchanged), while a value that came
				// from an inline override is always re-emitted compensated,
				// so a leftover override can never swallow the compensation.
				// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
				// (ISC license) --
				// growth compensation.
				std::vector<std::string_view> growth_drops;
				double const growth_geo = std::sqrt(
					std::max(0.0, pt.growth_x * pt.growth_y));
				auto scaled_differs = [](double base, double growth,
										 double style_default) {
					return FormatCoord(base * growth, 2) != FormatCoord(base, 2) || FormatCoord(base, 2) != FormatCoord(style_default, 2);
				};
				if (input.options.scale_border) {
					double const base =
						pose_info.has_bord ? pose_info.bord : style_outline_w;
					if (pose_info.has_xbord || pose_info.has_ybord) {
						double const bx =
							pose_info.has_xbord ? pose_info.xbord : base;
						double const by =
							pose_info.has_ybord ? pose_info.ybord : base;
						bool const emit_x =
							scaled_differs(bx, pt.growth_x, style_outline_w);
						bool const emit_y =
							scaled_differs(by, pt.growth_y, style_outline_w);
						if (emit_x || emit_y) {
							if (emit_x) {
								transforms += "\\xbord" + FormatCoord(bx * pt.growth_x, 2);
								growth_drops.emplace_back("\\xbord");
							}
							if (emit_y) {
								transforms += "\\ybord" + FormatCoord(by * pt.growth_y, 2);
								growth_drops.emplace_back("\\ybord");
							}
							// A uniform inline \bord would override the
							// per-axis values; it is absorbed into the base.
							growth_drops.emplace_back("\\bord");
						}
					}
					else if (scaled_differs(base, growth_geo,
											style_outline_w)) {
						transforms += "\\bord" + FormatCoord(base * growth_geo, 2);
						growth_drops.emplace_back("\\bord");
					}
				}
				if (input.options.scale_shadow) {
					double const base =
						pose_info.has_shad ? pose_info.shad : style_shadow_w;
					if (pose_info.has_xshad || pose_info.has_yshad) {
						double const sx =
							pose_info.has_xshad ? pose_info.xshad : base;
						double const sy =
							pose_info.has_yshad ? pose_info.yshad : base;
						bool const emit_x =
							scaled_differs(sx, pt.growth_x, style_shadow_w);
						bool const emit_y =
							scaled_differs(sy, pt.growth_y, style_shadow_w);
						if (emit_x || emit_y) {
							if (emit_x) {
								transforms += "\\xshad" + FormatCoord(sx * pt.growth_x, 2);
								growth_drops.emplace_back("\\xshad");
							}
							if (emit_y) {
								transforms += "\\yshad" + FormatCoord(sy * pt.growth_y, 2);
								growth_drops.emplace_back("\\yshad");
							}
							growth_drops.emplace_back("\\shad");
						}
					}
					else if (scaled_differs(base, growth_geo,
											style_shadow_w)) {
						transforms += "\\shad" + FormatCoord(base * growth_geo, 2);
						growth_drops.emplace_back("\\shad");
					}
				}
				if (input.options.scale_blur) {
					// \blur has no style counterpart; the base is the inline
					// value or 0, and 0 never emits.
					double const base = pose_info.has_blur ? pose_info.blur : 0.0;
					if (scaled_differs(base, growth_geo, 0.0)) {
						transforms += "\\blur" + FormatCoord(base * growth_geo, 2);
						growth_drops.emplace_back("\\blur");
					}
				}

				// The position component goes in ahead of any surviving
				// stale position (first-wins); the transform components
				// append after the kept bytes so they stay last-wins.
				part.text = ReplaceTagsDropping(line->Text, pos_tag, true,
												true, growth_drops);
				if (!transforms.empty())
					part.text = AppendTagToFirstBlock(part.text, transforms, true);
				part.x0 = part.x1 = pt.x;
				part.y0 = part.y1 = pt.y;
				part.covered = true;
				pl.parts.push_back(std::move(part));
				continue;
			}

			// The tag endpoints are the fitted spline evaluated at the part's
			// own time boundaries. The previous part owns the shared knot's
			// frame (rendering its position at that frame's timecode), so
			// this part starts one frame boundary later and must begin from
			// the spline value THERE — starting from the knot value would lag
			// the trajectory by a full frame at every part boundary.
			int const ta = input.timecodes.TimeAtFrame(points[piece.i0].frame);
			int const tb = input.timecodes.TimeAtFrame(points[piece.i1].frame);
			auto spline_at = [&](int t_ms, double ka, double kb) {
				double const u = tb > ta
									 ? std::clamp(double(t_ms - ta) / double(tb - ta), 0.0, 1.0)
									 : 0.0;
				return ka + (kb - ka) * u;
			};
			double const ex0 = spline_at(event_start_ms, piece.x0, piece.x1);
			double const ey0 = spline_at(event_start_ms, piece.y0, piece.y1);
			double const ex1 = piece.x1; // t2 below anchors on tb exactly
			double const ey1 = piece.y1;

			int const dec = input.options.position_decimals;
			std::string const x0s = FormatCoord(ex0, dec);
			std::string const y0s = FormatCoord(ey0, dec);
			std::string tag;
			if (input.options.mode == ApplyMode::Exact || (FormatCoord(ex0, dec) == FormatCoord(ex1, dec) && FormatCoord(ey0, dec) == FormatCoord(ey1, dec))) {
				tag = "\\pos(" + x0s + "," + y0s + ")";
			}
			else {
				// The endpoint anchors land on the frames the endpoints were
				// sampled at (a frame renders at its EXACT timecode), with an
				// explicit window: libass -- and ResolveDialogueOrigin above
				// -- reads a window with ANY positive bound as explicit, so
				// t1 = 0 with a positive t2 is honored and emitted verbatim.
				// Only a window with BOTH bounds non-positive would degrade
				// to the whole event.
				int const dur = event_end_ms - event_start_ms;
				int const t1 = std::clamp(
					ta - event_start_ms, 0, std::max(0, dur - 1));
				int const t2 = std::clamp(
					tb - event_start_ms, t1 + 1, std::max(t1 + 1, dur));
				tag = "\\move(" + x0s + "," + y0s + "," + FormatCoord(ex1, dec) + "," + FormatCoord(ey1, dec) + "," + std::to_string(t1) + "," + std::to_string(t2) + ")";
			}
			part.text = ReplacePositionTag(line->Text, tag);
			part.covered = true;
			part.x0 = ex0;
			part.y0 = ey0;
			part.x1 = ex1;
			part.y1 = ey1;
			pl.parts.push_back(std::move(part));
		}

		// Output-side merge for Exact mode: adjacent covered parts whose
		// emitted text is byte-identical become one part spanning both.
		// Hold-separated runs stay hard cut points for FITTING (a piece
		// must never interpolate across a gap hold), but the static parts
		// they produce can still coincide — most often a pose held through
		// an interior Failed gap that matches the tracked pose either side
		// — and a duplicate event there changes nothing a renderer shows:
		// both parts render the same constant tags, so the merged part
		// renders identically at every frame time. The previous part keeps
		// owning the shared knot's frame; the merged part just extends to
		// the second part's end. Time adjacency is the exact tiling the
		// part loop above already guarantees.
		// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
		// (ISC license) -- extending the previous output event when the
		// next frame's generated line is identical.
		if (input.options.mode == ApplyMode::Exact && !frame_parts) {
			size_t write = 1;
			for (size_t read = 1; read < pl.parts.size(); ++read) {
				PlannedLinePart& prev = pl.parts[write - 1];
				if (prev.covered && pl.parts[read].covered && prev.end_ms == pl.parts[read].start_ms && prev.text == pl.parts[read].text) {
					prev.end_ms = pl.parts[read].end_ms;
					prev.x1 = pl.parts[read].x1;
					prev.y1 = pl.parts[read].y1;
					continue;
				}
				if (write != read)
					pl.parts[write] = std::move(pl.parts[read]);
				++write;
			}
			pl.parts.resize(write);
		}

		// Events are the covered parts that survive the merge.
		for (auto const& part : pl.parts)
			if (part.covered)
				++event_count;

		if (CentisecondRounded(dom_end_ms) > CentisecondRounded(cursor)) {
			// Defensive: rounding clamps should make this unreachable, but a
			// trailing filler keeps the timeline seamless. A sub-centisecond
			// filler is dropped for the same reason as the prefix/suffix
			// slivers.
			PlannedLinePart tail;
			tail.start_ms = cursor;
			tail.end_ms = dom_end_ms;
			tail.text = line->Text;
			tail.covered = false;
			pl.parts.push_back(std::move(tail));
		}
		if (CentisecondRounded(line_end) > CentisecondRounded(dom_end_ms)) {
			PlannedLinePart suffix;
			suffix.start_ms = dom_end_ms;
			suffix.end_ms = line_end;
			suffix.text = line->Text;
			suffix.covered = false;
			pl.parts.push_back(std::move(suffix));
		}

		// Claim the original fade representation before retiming can turn a
		// completed alpha transform into a static tag which is no longer
		// recognizable as the source fade. Uncovered parts are stripped too.
		if (fade_active)
			for (auto& part : pl.parts)
				part.text = align_video_fade::StripClaimableFade(part.text);

		// Remaining source effects use the source event clock, even in
		// uncovered parts. The session fade written below is already local.
		if (!source_time_tags.empty())
			for (auto& part : pl.parts)
				part.text = RetimeSourceTags(part.text, source_time_tags,
											 CentisecondRounded(part.start_ms) - line_start, part.covered);

		// Per-part fade slicing (options.apply_fad): the dialog turns each
		// planned part into a SEPARATE ASS event, and an event's fade
		// windows are relative to its own duration -- so a \fad composed
		// against the whole line span would land its fade-out inside the
		// first part's short window while the later parts pop back in fully
		// opaque. Instead the interval's ramps, clamped into the LINE's own
		// [Start, End] (never moved), define one global alpha curve, and
		// every covered part receives the \fade that reproduces that curve
		// over the part's local time [0, part_end - part_start].
		if (fade_active && !pl.parts.empty()) {
			// Line-level durations: the global anchors clamped into the
			// line's span and rounded with RoundAssFadeTimingToCentisecond's
			// conventions (which also enforce fade_in + fade_out <= the
			// line duration, so the two ramps can never overlap).
			auto const line_timing =
				align_video_fade::RoundAssFadeTimingToCentiseconds(
					align_video_fade::AssFadeTiming{
						line_start,
						line_end,
						fade_anchors.has_in
							? fade_anchors.in_full_ms - line_start
							: 0,
						fade_anchors.has_out
							? line_end - fade_anchors.out_full_ms
							: 0});
			int const fade_in_ms = line_timing.fade_in_ms;
			int const fade_out_ms = line_timing.fade_out_ms;
			int const ramp_in_end = line_start + fade_in_ms;
			int const ramp_out_start = line_end - fade_out_ms;

			// Global curve alpha (ASS units: 0 opaque .. 255 transparent) at
			// absolute time t: 0 (or the curve's mid-ramp value) while the
			// fade-in ramp climbs to full visibility, fully opaque through
			// the plateau, then down to fully transparent at line End.
			auto alpha_at = [&](int t) {
				double visibility = 1.0;
				if (fade_in_ms > 0 && t < ramp_in_end)
					visibility = double(t - line_start) / fade_in_ms;
				else if (fade_out_ms > 0 && t > ramp_out_start)
					visibility = double(line_end - t) / fade_out_ms;
				return int(std::lround((1.0 - visibility) * 255.0));
			};

			for (auto& part : pl.parts) {
				// Original fades were stripped above; only covered parts
				// receive a new slice of the detected session fade.
				if (!part.covered)
					continue;

				int const dur = part.end_ms - part.start_ms;
				if (dur <= 0)
					continue;
				// Ramp breakpoints clamped into the part's local window and
				// rounded with the same centisecond conventions (the
				// normalization inside clamps them into [0, dur] and keeps
				// t2 <= t3). t1 is always 0 and t4 always the duration: the
				// ramps are clamped into the line's span and the covered
				// parts tile a subset of it.
				auto const part_timing =
					align_video_fade::RoundAssFadeTimingToCentiseconds(
						align_video_fade::AssFadeTiming{
							part.start_ms,
							part.end_ms,
							ramp_in_end - part.start_ms,
							part.end_ms - ramp_out_start});
				int const dur_cs =
					part_timing.end_ms - part_timing.start_ms;
				int const t2 = part_timing.fade_in_ms;
				int const t3 = dur_cs - part_timing.fade_out_ms;

				int a1 = alpha_at(part.start_ms);
				int a3 = alpha_at(part.end_ms);
				int a2 = 0; // the fully-visible plateau level
				if (t2 >= dur_cs)
					a2 = a3; // window entirely inside the fade-in ramp: the
							 // rendered start->plateau ramp must end at the
							 // curve's own value at the part end
				else if (t3 <= 0)
					a2 = a1; // window entirely inside the fade-out ramp: the
							 // rendered plateau->end ramp must start at the
							 // curve's own value at the part start
				if (a1 == 0 && a2 == 0 && a3 == 0)
					continue; // plateau-only part: nothing to reproduce

				// Endpoints no renderer can observe (a1 only renders before
				// t2, a3 only after t3) are normalized to the canonical
				// fully-transparent 255, so a window covering the line's
				// whole curve collapses to the simple \fad(in,out) form.
				if (t2 <= 0)
					a1 = 255;
				if (t3 >= dur_cs)
					a3 = 255;

				std::string tag;
				if (a1 == 255 && a2 == 0 && a3 == 255) {
					tag = "\\fad(" + std::to_string(t2) + "," + std::to_string(dur_cs - t3) + ")";
				}
				else {
					tag = "\\fade(" + std::to_string(a1) + "," + std::to_string(a2) + "," + std::to_string(a3) + ",0," + std::to_string(t2) + "," + std::to_string(t3) + "," + std::to_string(dur_cs) + ")";
				}
				part.text = InsertTagAtBlockStart(part.text, tag);
			}
		}

		if (input.options.mode == ApplyMode::Compact && !frame_parts) {
			// Validate what ASS actually renders after fitting, splitting,
			// time quantization and coordinate serialization. Walk both sorted
			// sequences together and parse each emitted part only once.
			size_t part_index = 0;
			bool parsed_part = false;
			AssDialogue emitted(*line);
			PositionInfo emitted_info;
			int const rendered_start = CentisecondRounded(dom_start_ms);
			int const rendered_end = CentisecondRounded(dom_end_ms);
			for (auto const& point : points) {
				int const time = input.timecodes.TimeAtFrame(point.frame);
				if (time < rendered_start || time >= rendered_end)
					continue;
				while (part_index < pl.parts.size() && time >= CentisecondRounded(pl.parts[part_index].end_ms)) {
					++part_index;
					parsed_part = false;
				}
				if (part_index >= pl.parts.size() || !pl.parts[part_index].covered ||
					time < CentisecondRounded(pl.parts[part_index].start_ms)) {
					plan.status = ApplyPlanStatus::InvalidInput;
					plan.message = "Compact event timing cannot cover every tracked frame after ASS rounding";
					return plan;
				}
				if (!parsed_part) {
					auto const& part = pl.parts[part_index];
					emitted.Start = CentisecondRounded(part.start_ms);
					emitted.End = CentisecondRounded(part.end_ms);
					emitted.Text = part.text;
					emitted_info = ExtractPositionInfo(emitted);
					parsed_part = true;
				}
				double x = 0, y = 0;
				bool const resolved = ResolveDialogueOriginInfo(file, emitted_info, emitted,
																time, input.script_width, input.script_height, x, y);
				double const error = std::hypot((x - point.x) * inv_scale_x,
												(y - point.y) * inv_scale_y);
				if (!resolved || !std::isfinite(error) || error > input.options.compact_epsilon) {
					plan.status = ApplyPlanStatus::InvalidInput;
					plan.message = "Compact output exceeds the pixel error budget; increase Position decimals or Compact error";
					return plan;
				}
			}
		}

		planned_lines.push_back(std::move(pl));
	}

	if (any_uncovered) {
		plan.status = ApplyPlanStatus::IncompleteCoverage;
		plan.message = "trajectory does not cover every target line's apply "
					   "domain";
		plan.uncovered = std::move(uncovered_lines);
		return plan;
	}
	plan.status = event_count > 100 ? ApplyPlanStatus::NeedsConfirmation
									: ApplyPlanStatus::Ok;
	plan.event_count = event_count;
	plan.lines = std::move(planned_lines);
	return plan;
}

MotionTrackApplyPlan BuildApplyPlan(AssFile const& file,
									std::vector<AssDialogue *> const& targets, ApplyPlanInput const& input) {
	auto invalid = [](std::string message) {
		MotionTrackApplyPlan result;
		result.status = ApplyPlanStatus::InvalidInput;
		result.message = std::move(message);
		return result;
	};
	if (input.model != TrackModel::Translation && input.model != TrackModel::Similarity &&
		input.model != TrackModel::Affine && input.model != TrackModel::Homography) {
		MotionTrackApplyPlan result;
		result.status = ApplyPlanStatus::UnsupportedModel;
		result.message = "unknown motion tracking model";
		return result;
	}
	if (input.video_frame_count <= 0 || !input.timecodes.IsLoaded())
		return invalid("motion tracking requires video frames and loaded timecodes");
	if (input.storage_width <= 0 || input.storage_height <= 0 ||
		input.script_width <= 0 || input.script_height <= 0)
		return invalid("missing resolution information");
	if (!std::isfinite(input.origin_center_x) || !std::isfinite(input.origin_center_y))
		return invalid("motion tracking origin must be finite");
	if (input.options.mode != ApplyMode::Compact && input.options.mode != ApplyMode::Exact)
		return invalid("unknown motion tracking apply mode");
	if (input.options.position_decimals < 0 || input.options.position_decimals > 6)
		return invalid("position precision must be between zero and six decimal places");
	if (input.options.mode == ApplyMode::Compact &&
		(!std::isfinite(input.options.compact_epsilon) || input.options.compact_epsilon < 0))
		return invalid("Compact error must be finite and non-negative");
	MotionTrackApplyPlan result;
	result.status = ApplyPlanStatus::Ok;
	for (auto *line : targets) {
		if (!line || line->Comment)
			continue;
		int const line_start = static_cast<int>(line->Start);
		int const line_end = static_cast<int>(line->End);
		if (input.options.mode == ApplyMode::Compact && line_end > line_start) {
			int const first = std::max({0, input.direction_domain.first,
										input.timecodes.FrameAtTime(line_start, agi::vfr::Time::START)});
			int const last = std::min({input.video_frame_count - 1, input.direction_domain.last,
									   input.timecodes.FrameAtTime(line_end, agi::vfr::Time::END)});
			for (int frame = first; frame < last; ++frame) {
				if (input.timecodes.TimeAtFrame(frame + 1) <= input.timecodes.TimeAtFrame(frame))
					return invalid("Compact requires strictly increasing frame times in the applied range");
			}
		}
		auto part = NeedsGeometryApply(*line, input.model)
						? BuildGeometryApplyPlan(file, line, input)
						: BuildPositionApplyPlan(file, {line}, input);
		if (part.status == ApplyPlanStatus::IncompleteCoverage) {
			result.uncovered.insert(result.uncovered.end(), part.uncovered.begin(), part.uncovered.end());
			continue;
		}
		if (part.status != ApplyPlanStatus::Ok && part.status != ApplyPlanStatus::NeedsConfirmation)
			return part;
		result.event_count += part.event_count;
		result.lines.insert(result.lines.end(), std::make_move_iterator(part.lines.begin()), std::make_move_iterator(part.lines.end()));
	}
	if (!result.uncovered.empty()) {
		result.status = ApplyPlanStatus::IncompleteCoverage;
		result.message = "trajectory does not cover every target line's apply domain";
		result.lines.clear();
		result.event_count = 0;
	}
	else if (result.event_count > 100)
		result.status = ApplyPlanStatus::NeedsConfirmation;
	return result;
}

} // namespace aegisub::motion_track
