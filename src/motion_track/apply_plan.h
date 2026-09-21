#pragma once

// Takes the published trajectory plus validated session inputs and produces a
// typed mutation plan; it never mutates the file and never touches wx.
// Semantics: full-domain coverage validation before any emission (zero
// mutations on any uncovered frame), interior Failed gaps hold the previous
// Ok pose only when bounded by Ok on both sides in time-ascending order,
// out-of-domain prefixes/suffixes preserve the source behavior (event-relative
// animation clocks are rebased), Comment lines are
// skipped. All four models support Exact and Compact. Similarity
// Compact uses \move for the fitted position and prefers one linear or
// accelerated \t for the \frz/\fscx/\fscy channels. Additional pose ramps
// stay in the same event; only position/coverage boundaries split events.
// When reliable bounds are available, multi-ramp poses can simplify to one
// transform if the combined position/pose geometry meets compact_epsilon.
// Exact writes one static pose per part. In Exact
// mode, adjacent covered parts whose
// emitted text is identical (e.g. a pose held through a Failed gap that
// matches the tracked pose) merge into one event that renders the same at
// every frame time.
// Affine, Homography and existing absolute/perspective tags use the full
// geometry writer, which checks the rendered projection at every frame.
// Source fades and nonnested color/alpha transforms retain their original
// timing across parts. Unsupported animations/karaoke and incompatible
// Similarity rotation/scale runs reject the complete plan.

#include "types.h"
#include "../perspective_ass_bounds.h"

#include <libaegisub/vfr.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class AssDialogue;
class AssFile;

namespace aegisub::motion_track {

/// Fitted fade interval for one direction of the tracked range, derived by
/// the session from the published visibility curve (see session.cpp, which
/// reuses align_video_fade's plateau/curve fitting with dialog_align's
/// parameter conventions). All fields are absolute decode frame indices.
struct FadeIntervalEdge {
	bool detected = false;
	/// Outermost frame of the fade ramp: the first persistently visible
	/// sample for a fade-in, the last visible one for a fade-out
	/// (FitVisibilityCurve's outer_index). Informational for applies --
	/// the \fad composition anchors its ramps on the full-visibility
	/// frames, and event Start/End are never moved.
	int outer_frame = -1;
	/// Fitted full-visibility boundary: the ramp-side edge of the confirmed
	/// fully-visible plateau (the first fully-visible frame of a fade-in /
	/// the last one of a fade-out). Its frame-boundary timecode (START for
	/// the fade-in completion, END for the fade-out start -- the window the
	/// frame is fully displayed over) anchors the \fad composition's ramps.
	int full_visibility_frame = -1;
	/// FitVisibilityCurve's confidence in [0, 1]; display/diagnostics only.
	double confidence = 0.0;
};

struct FadeInterval {
	FadeIntervalEdge fade_in;  // backward arm: ramp before the seed
	FadeIntervalEdge fade_out; // forward arm: ramp after the seed
};

struct ApplyPlanOptions {
	ApplyMode mode = ApplyMode::Compact;
	/// Max trajectory deviation, in storage (video) pixels. Deviations are
	/// measured in storage space, so the on-screen error means the same thing
	/// at every PlayRes/LayoutRes combination.
	double compact_epsilon = 1.0; // storage px
	int position_decimals = 2;
	/// Half-window (frames) for local-linear trajectory smoothing before
	/// planning; 0 disables. Constant-velocity input passes through exactly.
	int smooth_frames = 0;

	/// Trajectory stabilization chain (angle unwrap, median-3 + [1,2,3,2,1]
	/// smoothing, noise-floor flattening, seed-frame pinning), applied inside
	/// each hold-separated run. Off by default: the chain is exact-preserving
	/// for perfectly linear/static input. Toggle in the apply dialog
	/// ("Stabilize trajectory"); the floors are fixed measured constants.
	/// See apply_plan.cpp for the semantics and the upstream provenance.
	/// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	/// (ISC license) -- StabilisedPlanar.
	struct Stabilization {
		bool enable = false;
		/// Flattening thresholds (croni's measured values): a run whose total
		/// excursion stays below the floor, within the floor of its reference
		/// (seed-frame) value, is pinned to that value as pure sensor noise.
		double position_floor_storage_px = 0.35;
		double scale_floor = 0.012; // fraction of the composed base scale
		double angle_floor_deg = 0.75;
	};
	Stabilization stabilization;

	/// Compose fade tags from the session-detected fade interval onto every
	/// applied line. The interval's full-visibility boundaries are projected
	/// with BuildAssFadeTiming's event-boundary conventions (a frame is
	/// fully displayed over its [START, END) window) and then clamped into
	/// each line's OWN [Start, End] (which are never moved):
	/// fade_in_ms = clamp(START(fade-in full-visibility frame) - Start,
	/// 0, line duration), symmetrically fade_out_ms with END(fade-out
	/// full-visibility frame) and line End, both rounded with
	/// RoundAssFadeTimingToCentiseconds -- so a line shorter than / offset
	/// from the fitted ramp still gets its own correctly-clamped fade
	/// instead of one projected from the tracked event span. The clamped
	/// ramps define one global alpha curve for the line, and because the
	/// apply dialog turns every planned part into a SEPARATE ASS event (an
	/// event's \fad windows are relative to its own duration, not the
	/// line's), each covered part receives the 7-arg
	/// \fade(a1,a2,a3,t1,t2,t3,t4) reproducing that curve over the part's
	/// local window: a1/a3 are the curve's alpha at the part's own
	/// start/end (a part starting mid-ramp gets t1=0 with a1 already
	/// mid-ramp), t2/t3 are the ramp breakpoints clamped into the window,
	/// all control points rounded to centiseconds with
	/// RoundAssFadeTimingToCentiseconds conventions. A part lying entirely
	/// in the fully-visible plateau gets no tag, and a window covering the
	/// line's whole curve collapses to the simple \fad(in,out). This
	/// per-part slicing deliberately goes beyond ApplyAssFade's
	/// single-event shape (align_video_fade::StripClaimableFade strips the
	/// representations ApplyAssFade would claim -- \fad/\fade everywhere
	/// plus the leading overall-alpha \t form -- from every part, covered
	/// or not; uncovered prefix/suffix parts never receive a new tag: they
	/// render outside the tracked domain). Applied mode-agnostically (Exact
	/// and Compact alike): the fade describes the event, not the
	/// trajectory fit. When detection says no fade, or the option is off,
	/// or the input carries no interval, output is byte-identical to the
	/// pre-feature planner. Off by default; toggle in the apply dialog
	/// ("Write \fad from fade").
	bool apply_fad = false;

	/// Scale \bord (and \xbord/\ybord), \shad (and \xshad/\yshad) and \blur
	/// with the tracked transform's local stretch in similarity Exact
	/// applies, so outline width, shadow distance and blur radius follow the
	/// object as it zooms. Fields whose composed base value is zero are never
	/// emitted, and an identity (translation-only) transform emits nothing.
	/// Off by default: compensation rewrites styling values the user chose,
	/// so it is an explicit opt-in (the apply dialog's "Scale
	/// border/shadow/blur" checkbox, enabled for similarity Exact applies).
	/// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
	/// (ISC license) -- growth.
	bool scale_border = false;
	bool scale_shadow = false;
	bool scale_blur = false;
};

struct ApplyPlanInput {
	std::vector<TrackSample> samples; // ascending by frame (published copy)
	TrackModel model = TrackModel::Translation;
	double origin_center_x = 0.0; // absolute storage px of the origin seed
	double origin_center_y = 0.0;
	FrameInterval decode_interval;  // inclusive; anchors may come from here
	FrameInterval direction_domain; // inclusive; intersected per line
	int storage_width = 0;
	int storage_height = 0;
	int script_width = 0;  // PlayRes: libass maps \pos/\move with PlayRes
	int script_height = 0; // LayoutRes is not a \pos coordinate space
	agi::vfr::Framerate timecodes{};
	int video_frame_count = 0;
	int seed_time_ms = 0;
	/// Session-detected fade interval (the snapshot's fade_interval);
	/// nullopt (or an all-undetected interval) means no fade data, and the
	/// planner output is byte-identical to the pre-feature planner.
	std::optional<FadeInterval> fade_interval;
	ApplyPlanOptions options;
	// GUI supplies the platform font measurer; headless callers may use the
	// deterministic bounds approximation provided by the perspective module.
	perspective::AssTextExtentsProvider text_extents = nullptr;
};

enum class ApplyPlanStatus {
	Ok,
	NeedsConfirmation,  // plan is complete; it would create >100 events
	IncompleteCoverage, // zero mutations; see uncovered
	UnsupportedModel,   // unknown trajectory model
	UnsupportedMode,    // reserved for an unsupported apply-mode combination
	InvalidInput,       // malformed inputs: no frames, missing resolution
};

struct PlannedLinePart {
	// Raw millisecond boundaries; ASS serialization rounds them to centiseconds.
	int start_ms = 0;
	int end_ms = 0;
	std::string text;
	bool covered = true; // false for preserved out-of-domain prefix/suffix
	// Position endpoints in script px; equal for \pos parts. Compact may
	// reach the second endpoint before the event ends. Only meaningful while covered.
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

struct PlannedLine {
	AssDialogue *source = nullptr;
	std::vector<PlannedLinePart> parts; // replaces source entirely when applied
};

struct UncoveredRange {
	int first = -1; // inclusive frame indices within the line's apply domain
	int last = -1;
};

struct LineUncovered {
	AssDialogue *source = nullptr;
	std::vector<UncoveredRange> ranges;
};

struct MotionTrackApplyPlan {
	ApplyPlanStatus status = ApplyPlanStatus::InvalidInput;
	std::string message;
	std::vector<PlannedLine> lines;
	std::vector<LineUncovered> uncovered;
	size_t event_count = 0; // new events the covered parts introduce

	// Reserved diagnostics for lines requiring manual review. Supported
	// absolute geometry follows the trajectory; unsupported geometry rejects
	// the complete plan instead of leaving a partially transformed line.
	std::vector<AssDialogue *> needs_manual_review;

	bool needs_confirmation() const {
		return status == ApplyPlanStatus::NeedsConfirmation;
	}
	bool has_mutations() const {
		return (status == ApplyPlanStatus::Ok || status == ApplyPlanStatus::NeedsConfirmation) && !lines.empty();
	}
};

// targets: selected dialogue lines (non-comment ones are planned; comment
// lines are skipped silently). All pointers must remain valid while the plan
// is applied.
MotionTrackApplyPlan BuildApplyPlan(
	AssFile const& file,
	std::vector<AssDialogue *> const& targets,
	ApplyPlanInput const& input);

// Resolves where the line sits at seed_time_ms in script pixels: explicit
// \pos wins, then \move interpolated at the seed instant with libass's
// window semantics (a 6-arg \move with any positive bound is an explicit
// window, and a reversed one is swapped; a 4-arg \move or a window with
// both bounds non-positive spans the whole event), then style/\an/\a/
// margin fallback. Every spelling resolves to a position, so the bool
// return is an invariant for callers rather than a per-line rejection.
bool ResolveDialogueOrigin(AssFile const& file, AssDialogue const& line,
						   int seed_time_ms, int script_width,
						   int script_height, double& out_x, double& out_y);

// PlayRes is the \pos/\move coordinate space libass actually renders.
// LayoutRes only participates in blur/3D/border scaling and must not be
// copied into script_width/height.
void FillApplyInputScriptResolution(ApplyPlanInput& input, AssFile const& file);

// Identity tokens are compared only and never turned back into pointers.
struct MotionTrackLineFingerprint {
	std::uintptr_t identity = 0;
	int id = 0;
	int row = -1;
	bool comment = false;
	int layer = 0;
	std::array<int, 3> margins{};
	int start_ms = 0;
	int end_ms = 0;
	std::string style;
	std::string actor;
	std::string effect;
	std::vector<std::uint32_t> extradata_ids;
	std::string text;
	std::string style_entry;
};

struct MotionTrackSourceSnapshot {
	std::uintptr_t file_identity = 0;
	std::vector<MotionTrackLineFingerprint> lines;
	std::vector<std::string> script_info_entries;
	agi::vfr::Framerate timecodes;
	int video_frame_count = 0;
};

MotionTrackSourceSnapshot CaptureMotionTrackSource(
	AssFile const& file,
	std::vector<AssDialogue *> const& lines,
	agi::vfr::Framerate const& timecodes,
	int video_frame_count);

// True when every captured line is still the same object with the same
// dialogue/style/script-info fields and the timecodes mapping is unchanged.
bool MotionTrackSourceIsCurrent(
	AssFile const& file,
	MotionTrackSourceSnapshot const& expected,
	agi::vfr::Framerate const& timecodes);

// Reconstructs live pointers in capture order. Empty with `message` set when
// any line is missing, duplicated, or stale.
std::vector<AssDialogue *> ResolveMotionTrackSourceLines(
	AssFile& file,
	MotionTrackSourceSnapshot const& expected,
	std::string& message);

// Continue keys: same objects and Ids with unchanged Start/End. Timing
// edits keep the pointer/Id but invalidate the session's decode domain.
bool MotionTrackContinueTargetsMatch(
	MotionTrackSourceSnapshot const& expected,
	std::vector<AssDialogue *> const& targets);

// True when the snapshot's stored frame→time mapping still matches `timecodes`.
bool MotionTrackTimecodesMatch(
	MotionTrackSourceSnapshot const& expected,
	agi::vfr::Framerate const& timecodes);

// Strips existing \pos/\move tags from every override block and inserts the
// new tag into the first block (creating one when absent). Nested \t
// transforms are left intact. libass resolves pos/move first-wins per event,
// so when the kept bytes still hold a position spelling the drop pass cannot
// see ("\ pos(...)" or one nested in \t), the new tag is placed in front of
// the kept bytes instead of after them.
std::string ReplacePositionTag(std::string const& text, std::string const& tag);

} // namespace aegisub::motion_track
