#include "visual_geometry_link.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_style.h"
#include "ass_tag_scanner.h"
#include "motion_track/geometry_tags.h"
#include "perspective_quad_geometry.h"
#include "translation_service.h"
#include "visual_ass_edit.h"

#include <libaegisub/of_type_adaptor.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

namespace aegisub::visual_geometry {
namespace {
enum class ValidationKind : std::uint8_t { None,
										   StaticPose,
										   UnsupportedSource };
struct State {
	Vector2D position, origin, scale;
	float rx = 0, ry = 0, rz = 0, fax = 0, fay = 0;
};
using param_vec = std::vector<AssOverrideParameter> const *;

static param_vec find_tag(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks, std::string const& tag_name) {
	for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		for (auto const& tag : ovr->Tags) {
			if (tag.Name == tag_name)
				return &tag.Params;
		}
	}

	return nullptr;
}

struct DefaultPositionLayout {
	std::array<int, 3> margins;
	int alignment;
};

static int ReadLineAlignment(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks) {
	int alignment = 0;
	if (auto tag = find_tag(blocks, "\\an"))
		alignment = (*tag)[0].Get<int>(0);
	else if (auto tag = find_tag(blocks, "\\a"))
		alignment = AssCompat::NormalizeLegacyAssAlignment((*tag)[0].Get<int>(0), 0);
	return alignment;
}

static DefaultPositionLayout ResolveDefaultPositionLayout(AssDialogue const& line, AssStyle const *style, int override_alignment) {
	DefaultPositionLayout layout{.margins = line.Margin, .alignment = style ? style->alignment : 2};
	if (style) {
		for (size_t i = 0; i < layout.margins.size(); ++i)
			if (layout.margins[i] == 0)
				layout.margins[i] = style->Margin[i];
	}
	if (override_alignment > 0 && override_alignment <= 9)
		layout.alignment = override_alignment;
	return layout;
}

}
struct Snapshot::Impl {
	AssDialogue source;
	State state;
	Vector2D script_size;
	motion_track::PreparedAffineGeometry geometry;
	std::string validation_error;
	ValidationKind validation_kind = ValidationKind::None;
	Impl(AssDialogue const& line, AssStyle const *style, Vector2D script_size);
};
Snapshot::Impl::Impl(AssDialogue const& line, AssStyle const *style, Vector2D script_size)
	: source(line), script_size(script_size), geometry(line.Text.get()) {
	source.Id = line.Id;
	source.Row = line.Row;
	auto const parsed = visual_edit::ReadLine(line, style, script_size);
	state = {.position = parsed.position, .origin = parsed.origin.value_or(parsed.position), .scale = parsed.scale, .rx = parsed.rotation_x, .ry = parsed.rotation_y, .rz = parsed.rotation_z, .fax = parsed.fax, .fay = parsed.fay};
	int const script_width = static_cast<int>(script_size.X()), script_height = static_cast<int>(script_size.Y());
	bool visible = false;
	bool animated_geometry = false;
	bool has_position = false;
	bool has_move = false;
	bool malformed_position = false;
	bool noncanonical_geometry = false;
	bool vertical_font = style && style->font.starts_with("@");
	bool const style_vertical_font = vertical_font;
	bool has_reset = false;
	size_t alignment_tags = 0;
	std::set<std::string_view> initial_pose_tags;
	auto const blocks = source.ParseTags();
	size_t last_visible = 0;
	for (size_t i = 0; i < blocks.size(); ++i) {
		if ((blocks[i]->GetType() == AssBlockType::PLAIN || blocks[i]->GetType() == AssBlockType::DRAWING) && !blocks[i]->GetRawText().empty())
			last_visible = i;
	}
	for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
		auto const& block = blocks[block_index];
		if (block->GetType() == AssBlockType::PLAIN || block->GetType() == AssBlockType::DRAWING) {
			visible |= !block->GetRawText().empty();
			if (block->GetType() == AssBlockType::PLAIN && !block->GetRawText().empty() && vertical_font)
				validation_error = _("Vertical fonts do not have a supported static clip transform in this phase.");
			continue;
		}
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto scan = [&](auto&& self, std::string_view body, bool animated, int depth) -> void {
			aegisub::ass_tag_scanner::ScanRawTags(body, [&](auto const& tag) {
				if (aegisub::ass_tag_scanner::NameHasPrefix(tag.name, "t") && tag.has_paren) {
					if (depth >= 32)
						animated_geometry = true;
					else
						self(self, tag.args, true, depth + 1);
					return;
				}
				for (std::string_view name : {"pos", "move", "org"}) {
					if (tag.name.starts_with(name) && (tag.name != name || tag.name.data() != tag.bytes.data() + 1))
						noncanonical_geometry = true;
				}
				if (!animated && tag.name.starts_with("fn")) {
					auto const font = tag.name.substr(2);
					vertical_font = font.empty() ? style_vertical_font : font.starts_with("@");
				}
				for (std::string_view name : {"fsc", "fscx", "fscy", "frz", "frx", "fry", "fr", "fax", "fay", "fs", "fsp", "pos", "move", "org", "clip", "iclip", "an", "a", "r", "pbo", "p"}) {
					bool const reset = name == "r" && tag.name.starts_with("r");
					if (!reset && !aegisub::ass_tag_scanner::NameIs(tag.name, name))
						continue;
					if (name != "clip" && name != "iclip" && tag.name.data() != tag.bytes.data() + 1)
						noncanonical_geometry = true;
					if (reset) {
						has_reset = true;
						validation_error = _("Style resets cannot be linked safely.");
					}
					if (!animated && (name == "an" || name == "a"))
						++alignment_tags;
					if (animated)
						animated_geometry = true;
					if (!animated && (name == "pos" || name == "move")) {
						auto const args = aegisub::ass_tag_scanner::SplitLibassArgs(tag.args);
						bool supported = tag.has_paren && tag.bytes.ends_with(')') && (name == "pos" ? args.size() == 2 : args.size() == 4 || args.size() == 6) && static_cast<size_t>(std::ranges::count(tag.args, ',')) + 1 == args.size();
						for (size_t i = 0; supported && i < args.size(); ++i) {
							auto number = args[i];
							if (number.starts_with('+'))
								number.remove_prefix(1);
							if (i >= 4) {
								int value = 0;
								auto const parsed = std::from_chars(number.data(), number.data() + number.size(), value);
								supported = parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size();
							}
							else {
								double value = 0;
								auto const parsed = std::from_chars(number.data(), number.data() + number.size(), value);
								supported = parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && std::isfinite(value);
							}
						}
						malformed_position |= !supported;
						has_position |= supported && name == "pos";
						has_move |= supported && name == "move";
					}
					if (name == "fsc")
						validation_error = _("Combined scale tags cannot be linked safely; use fscx and fscy.");
					if (visible && block_index < last_visible && name != "clip" && name != "iclip" && name != "org")
						validation_error = _("Visible runs do not have one static geometry transform.");
					if (!animated && !visible && (name == "fscx" || name == "fscy" || name == "frz" || name == "fr" || name == "frx" || name == "fry" || name == "fax" || name == "fay" || name == "r")) {
						if (!initial_pose_tags.insert(name == "fr" ? "frz" : name).second || name == "r")
							validation_error = _("Duplicate pose tags or style resets cannot be linked safely.");
					}
				}
			});
		};
		scan(scan, block->GetRawText(), false, 0);
	}
	if (!validation_error.empty())
		validation_kind = ValidationKind::StaticPose;
	if (animated_geometry) {
		validation_error = _("Animated geometry cannot be linked. Disable geometry linking to edit this line.");
		validation_kind = ValidationKind::UnsupportedSource;
	}
	if (has_position && has_move) {
		validation_error = _("Conflicting position and move tags cannot be linked safely.");
		validation_kind = ValidationKind::UnsupportedSource;
	}
	if (!has_position && !has_move) {
		auto const layout = ResolveDefaultPositionLayout(line, style, ReadLineAlignment(blocks));
		int const horizontal = (layout.alignment - 1) % 3;
		int const vertical = (layout.alignment - 1) / 3;
		double const x = horizontal == 0 ? layout.margins[0] : horizontal == 1 ? (static_cast<double>(script_width) + layout.margins[0] - layout.margins[1]) / 2.0
																			   : static_cast<double>(script_width) - layout.margins[1];
		double const y = vertical == 0   ? static_cast<double>(script_height) - layout.margins[2]
						 : vertical == 1 ? script_height / 2.0
										 : layout.margins[2];
		if (!style || x != std::trunc(x) || y != std::trunc(y) || x != state.position.X() || y != state.position.Y()) {
			validation_error = _("The implicit default position cannot be linked consistently with the renderer. Set an explicit pos or disable geometry linking.");
			validation_kind = ValidationKind::UnsupportedSource;
		}
		if (has_reset || alignment_tags > 1) {
			validation_error = _("Implicit positioning with style resets or ambiguous alignment cannot be linked safely. Set an explicit pos or disable geometry linking.");
			validation_kind = ValidationKind::UnsupportedSource;
		}
	}
	if (malformed_position) {
		validation_error = _("Malformed position or move parameters cannot be linked safely. Use complete canonical pos or move parameters, or disable geometry linking.");
		validation_kind = ValidationKind::UnsupportedSource;
	}
	if (noncanonical_geometry) {
		validation_error = _("Noncanonical geometry tags are parsed differently by visual tools and renderers. Normalize their spelling before linking.");
		validation_kind = ValidationKind::UnsupportedSource;
	}
	auto effect = line.Effect.get();
	std::ranges::transform(effect, effect.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (effect.starts_with("banner;") || effect.starts_with("scroll up;") || effect.starts_with("scroll down;")) {
		validation_error = _("Scrolling dialogue effects do not have a supported geometry transform.");
		validation_kind = ValidationKind::UnsupportedSource;
	}
}

Snapshot::Snapshot(AssDialogue const& source, AssStyle const *style, Vector2D script_size)
	: impl(std::make_unique<Impl>(source, style, script_size)) {}
Snapshot::~Snapshot() = default;
Snapshot::Snapshot(Snapshot&&) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;
AssDialogue const& Snapshot::Source() const { return impl->source; }
bool Snapshot::HasClip() const { return impl->geometry.HasClip(); }
bool Snapshot::HasOrigin() const { return impl->geometry.HasOrigin(); }

PlanResult Plan(std::span<Request const> requests, Vector2D layout_size, bool restore_source) {
	PlanResult result;
	result.edits.reserve(requests.size());
	auto reject = [](std::string message) { return PlanResult{.edits = {}, .error = std::move(message)}; };
	for (auto const& request : requests) {
		if (!request.source)
			throw std::logic_error("Geometry linking requires a captured source");
		auto const& snapshot = *request.source->impl;
		auto const& source = snapshot.source;
		if (restore_source || request.restore_source || request.edits.empty()) {
			result.edits.push_back({.id = source.Id, .row = source.Row, .original_text = source.Text.get(), .text = source.Text.get()});
			continue;
		}
		auto const edits = request.edits;
		auto kind = edits.front().kind;
		auto translation = edits.front().translation;
		bool mixed = false;
		for (auto const& edit : edits) {
			if (edit.kind == EditKind::Translation) {
				kind = EditKind::Translation;
				translation = edit.translation;
			}
		}
		if (std::ranges::any_of(edits, [](auto const& edit) { return edit.kind == EditKind::Endpoint; }))
			kind = EditKind::Endpoint;
		for (auto const& edit : edits) {
			if (edit.kind == kind) {
				if (kind == EditKind::Translation && (edit.translation - translation).SquareLen() > 0.000001f)
					mixed = true;
				continue;
			}
			if (kind == EditKind::Translation && edit.tag == "\\org") {
				AssOverrideTag parsed(edit.tag + edit.value);
				Vector2D const target(parsed.Params[0].Get<float>(), parsed.Params[1].Get<float>());
				if ((target - snapshot.state.origin - translation).SquareLen() > 0.000001f)
					mixed = true;
			}
			else
				mixed = true;
		}
		bool const linked_object = kind == EditKind::Translation || kind == EditKind::Endpoint
									   ? snapshot.geometry.HasClip() || snapshot.geometry.HasOrigin()
									   : snapshot.geometry.HasClip();
		AssDialogue candidate(snapshot.source);
		if (linked_object) {
			if (mixed)
				return reject(_("Selected features request incompatible geometry edits on the same line. Select a position or one origin, or disable geometry linking."));
			if (!snapshot.validation_error.empty() && !(kind == EditKind::Translation && snapshot.validation_kind == ValidationKind::StaticPose))
				return reject(snapshot.validation_error);
			if (kind == EditKind::Endpoint)
				return reject(_("Editing a single move endpoint or its time cannot preserve attached geometry over the complete trajectory. Use Drag double-click to translate the complete trajectory, or disable geometry linking."));
			aegisub::motion_track::TrackTransform transform;
			if (kind == EditKind::Translation)
				transform.matrix = {1, 0, translation.X(), 0, 1, translation.Y(), 0, 0, 1};
			else {
				State next = snapshot.state;
				for (auto const& edit : edits) {
					AssOverrideTag parsed(edit.tag + edit.value);
					if (edit.tag == "\\fscx")
						next.scale = Vector2D(parsed.Params[0].Get<float>(), next.scale.Y());
					if (edit.tag == "\\fscy")
						next.scale = Vector2D(next.scale.X(), parsed.Params[0].Get<float>());
					if (edit.tag == "\\frz")
						next.rz = parsed.Params[0].Get<float>();
					if (edit.tag == "\\org")
						next.origin = Vector2D(parsed.Params[0].Get<float>(), parsed.Params[1].Get<float>());
					if (edit.tag == "\\frx")
						next.rx = parsed.Params[0].Get<float>();
					if (edit.tag == "\\fry")
						next.ry = parsed.Params[0].Get<float>();
				}
				if (kind == EditKind::Perspective || snapshot.state.rx != 0 || snapshot.state.ry != 0 || next.rx != 0 || next.ry != 0)
					return reject(_("Perspective clip linking is not supported in this phase. Disable geometry linking to edit this pose."));
				if (visual_edit::ReadLine(snapshot.source, nullptr, snapshot.script_size).move)
					return reject(_("A moving line has no single static clip transform. Translate its complete trajectory, or disable geometry linking."));
				if (snapshot.state.scale.X() <= 0 || snapshot.state.scale.Y() <= 0 || next.scale.X() <= 0 || next.scale.Y() <= 0)
					return reject(_("Non-positive scales cannot be linked. Use positive X and Y scales."));
				double const aspect = (layout_size.X() / snapshot.script_size.X()) / (layout_size.Y() / snapshot.script_size.Y());
				auto matrix = [&](State const& state) {
					double const theta = -state.rz * std::numbers::pi / 180;
					double const cosine = std::cos(theta), sine = std::sin(theta);
					double const sx = state.scale.X() / 100.0, sy = state.scale.Y() / 100.0;
					double const a = cosine * sx - sine * sy * state.fay;
					double const b = (cosine * sx * state.fax - sine * sy) / aspect;
					double const d = (sine * sx + cosine * sy * state.fay) * aspect;
					double const e = sine * sx * state.fax + cosine * sy;
					auto const offset = state.position - state.origin;
					double const tx = state.origin.X() + cosine * offset.X() - sine * offset.Y() / aspect;
					double const ty = state.origin.Y() + sine * offset.X() * aspect + cosine * offset.Y();
					return perspective::Matrix3({a, b, tx, d, e, ty, 0, 0, 1});
				};
				auto const before = matrix(snapshot.state), after = matrix(next);
				auto inverse = before.Inverse();
				if (!inverse || !after.Inverse())
					return reject(_("Zero scale or singular shear cannot be linked. Use nonzero scales and a nonsingular pose."));
				transform.matrix = (after * *inverse).Values();
				transform.matrix[6] = transform.matrix[7] = 0;
				transform.matrix[8] = 1;
			}
			auto linked = snapshot.geometry.Transform(transform, kind == EditKind::Translation);
			if (!linked.success)
				return reject(_(linked.error));
			candidate.Text = linked.text;
		}
		for (auto const& edit : edits)
			aegisub::visual_edit::SetOverride(candidate, edit.tag, edit.value);
		result.edits.push_back({.id = source.Id, .row = source.Row, .original_text = source.Text.get(), .text = candidate.Text.get()});
	}
	return result;
}
}
