#include "../../src/font_variant_audit.h"

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_font_state.h"
#include "../../src/ass_style.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

FontVariantOutcome outcome(
	FontVariantRole role,
	std::uint64_t entity,
	FontVariantStatus status = FontVariantStatus::Canonical,
	int weight = 400,
	bool italic = false) {
	FontVariantOutcome result;
	result.realized_weight = weight;
	result.realized_italic = italic;
	result.role = role;
	result.status = status;
	result.entity_token = entity;
	return result;
}

FontFamilyVariantProfile profile(
	FontVariantOutcome regular,
	FontVariantOutcome bold = {},
	FontVariantOutcome italic = {},
	FontVariantOutcome bold_italic = {}) {
	return BuildFontFamilyVariantProfile({
		std::move(regular),
		std::move(bold),
		std::move(italic),
		std::move(bold_italic),
	}, FontVariantBackend::VsFilterGdi,
	   FontSelectionEvidence::Observed, true);
}

FontFamilyRecord family(
	FontFamilyId id,
	std::string name,
	FontFamilyVariantProfile variant_profile) {
	FontFamilyRecord result;
	result.id = id;
	result.localized_family_name = name;
	result.english_win32_family_name = std::move(name);
	result.variant_profile = std::move(variant_profile);
	return result;
}

FontFamilyVariantProfile regular_profile(std::uint64_t base = 100) {
	return profile(
		outcome(FontVariantRole::Regular, base),
		outcome(FontVariantRole::Bold, base + 1, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Italic, base + 2, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::BoldItalic, base + 3, FontVariantStatus::Canonical, 700, true));
}

FontFamilyVariantProfile bold_only_profile(std::uint64_t entity = 10) {
	return profile(
		outcome(FontVariantRole::Bold, entity, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, entity, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic));
}

FontFamilyVariantProfile italic_only_profile(std::uint64_t entity = 20) {
	return profile(
		outcome(FontVariantRole::Italic, entity, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic),
		outcome(FontVariantRole::Italic, entity, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic));
}

AssStyle& add_style(
	AssFile& file,
	std::string name,
	std::string font,
	bool bold = false,
	bool italic = false) {
	auto* style = new AssStyle;
	style->name = std::move(name);
	style->font = std::move(font);
	style->bold = bold;
	style->italic = italic;
	style->UpdateData();
	file.Styles.push_back(*style);
	return *style;
}

AssDialogue& add_event(
	AssFile& file,
	std::string text,
	int row = 0,
	std::string style = "Default") {
	auto* event = new AssDialogue;
	event->Text = std::move(text);
	event->Style = std::move(style);
	event->Row = row;
	file.Events.push_back(*event);
	return *event;
}

struct AuditAssSnapshot {
	std::vector<std::string> styles;
	std::vector<std::string> events;

	bool operator==(AuditAssSnapshot const&) const = default;
};

AuditAssSnapshot snapshot_ass(AssFile const& file) {
	AuditAssSnapshot result;
	for (auto const& style : file.Styles) {
		result.styles.push_back(style.GetEntryData());
		AssStyle serialized(style);
		serialized.UpdateData();
		result.styles.push_back(serialized.GetEntryData());
	}
	for (auto const& event : file.Events)
		result.events.push_back(event.GetEntryData());
	return result;
}

struct RenderedAuditCharacter {
	std::size_t event_index;
	char character;
	std::string family;
	int charset;
	double height;
	std::uint64_t entity;
	FontVariantRole role;
	FontVariantStatus status;
	int weight;
	bool italic;

	bool operator==(RenderedAuditCharacter const&) const = default;
};

RenderedAuditCharacter rendered_character(
	std::size_t event_index,
	char character,
	std::string family_name,
	FontVariantOutcome const& selected) {
	return {
		.event_index = event_index,
		.character = character,
		.family = std::move(family_name),
		.charset = 1,
		.height = AssStyle::DefaultFontSize,
		.entity = selected.entity_token,
		.role = selected.role,
		.status = selected.status,
		.weight = selected.realized_weight,
		.italic = selected.realized_italic,
	};
}

std::vector<RenderedAuditCharacter> replay_rendered_text(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontVariantAuditProfileProvider const& provider = {}) {
	auto resolve_style = [&](std::string_view name)
		-> std::optional<aegisub::ass::AssFontStyleBaseline> {
		for (auto const& style : file.Styles) {
			if (style.name == name)
				return aegisub::ass::MakeAssFontStyleBaseline(style);
		}
		return std::nullopt;
	};
	std::vector<RenderedAuditCharacter> result;
	std::size_t event_index = 0;
	for (auto const& event : file.Events) {
		auto baseline = resolve_style(event.Style.get());
		if (!baseline)
			throw std::runtime_error("audit test event style is missing");
		aegisub::ass::AssFontStateEvaluator evaluator(*baseline);
		for (auto const& block : event.ParseTags()) {
			if (block->GetType() == AssBlockType::OVERRIDE) {
				evaluator.ApplyBlock(
					static_cast<AssDialogueBlockOverride const&>(*block), resolve_style);
				continue;
			}
			if (block->GetType() != AssBlockType::PLAIN)
				continue;
			auto const& request = evaluator.Request();
			if (!request.valid || (request.effective_weight != 400 && request.effective_weight != 700))
				throw std::runtime_error("audit test has an unsupported font request");
			std::optional<FontFamilyVariantProfile> selected_profile;
			if (provider)
				selected_profile = provider(request);
			else {
				auto resolution = catalog.Resolve(request.family);
				auto const *record = resolution.family ? catalog.Find(*resolution.family) : nullptr;
				if (record)
					selected_profile = record->variant_profile;
			}
			if (!selected_profile)
				throw std::runtime_error("audit test font profile is missing");
			auto const& selected = selected_profile->For(request.effective_weight == 700, request.italic);
			for (char character : block->GetRawText()) {
				auto rendered = rendered_character(event_index, character, request.family, selected);
				rendered.charset = request.charset;
				rendered.height = request.height;
				result.push_back(std::move(rendered));
			}
		}
		++event_index;
	}
	return result;
}

void expect_safe_apply(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	AuditAssSnapshot const& before_ass,
	std::vector<RenderedAuditCharacter> const& before_rendered,
	std::size_t selected_count,
	FontVariantAuditApplyResult const& applied,
	FontVariantAuditProfileProvider const& provider = {}) {
	if (applied.success) {
		EXPECT_TRUE(applied.error.empty()) << applied.error;
		EXPECT_EQ(selected_count, applied.applied_change_count);
		EXPECT_TRUE(applied.styles_changed || applied.dialogue_text_changed);
		EXPECT_NE(before_ass, snapshot_ass(file));
	}
	else {
		EXPECT_FALSE(applied.error.empty());
		EXPECT_FALSE(applied.styles_changed);
		EXPECT_FALSE(applied.dialogue_text_changed);
		EXPECT_EQ(0u, applied.applied_change_count);
		EXPECT_EQ(before_ass, snapshot_ass(file));
	}
	EXPECT_EQ(before_rendered, replay_rendered_text(file, catalog, provider));
}

void expect_rejected_apply(
	AssFile const& file,
	AuditAssSnapshot const& before_ass,
	FontVariantAuditApplyResult const& applied) {
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(applied.error.empty());
	EXPECT_FALSE(applied.styles_changed);
	EXPECT_FALSE(applied.dialogue_text_changed);
	EXPECT_EQ(0u, applied.applied_change_count);
	EXPECT_EQ(before_ass, snapshot_ass(file));
}

} // namespace

TEST(font_variant_audit, reports_and_applies_safe_style_fallbacks) {
	FontFamilyCatalog catalog({
		family(1, "Bold Only", bold_only_profile()),
		family(2, "Italic Only", italic_only_profile()),
	});
	AssFile file;
	auto& bold = add_style(file, "BoldStyle", "Bold Only");
	auto& italic = add_style(file, "ItalicStyle", "Italic Only");

	std::array<int, 2> source_lines{7, 8};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, source_lines);
	ASSERT_EQ(2u, plan.findings.size());
	EXPECT_EQ(2u, plan.scanned_style_count);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings[0].classification);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitItalicFallback,
	          plan.findings[1].classification);
	EXPECT_EQ(7, plan.findings[0].source.line);
	EXPECT_TRUE(plan.findings[0].safe_to_apply);
	EXPECT_TRUE(plan.findings[1].safe_to_apply);

	std::array<std::size_t, 2> selected{0, 1};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(applied.styles_changed);
	EXPECT_FALSE(applied.dialogue_text_changed);
	EXPECT_EQ(2u, applied.applied_change_count);
	EXPECT_TRUE(bold.bold);
	EXPECT_FALSE(bold.italic);
	EXPECT_FALSE(italic.bold);
	EXPECT_TRUE(italic.italic);
}

TEST(font_variant_audit, ordinary_rbiz_family_does_not_produce_a_finding) {
	FontFamilyCatalog catalog({family(1, "Complete", regular_profile())});
	AssFile file;
	add_style(file, "Default", "Complete");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	EXPECT_EQ(1u, plan.scanned_style_count);
	EXPECT_TRUE(plan.findings.empty());
}

TEST(font_variant_audit, classifies_noncanonical_synthetic_and_unknown_requests) {
	FontFamilyCatalog catalog({
		family(1, "Noncanonical", profile(outcome(
			FontVariantRole::Unknown, 11, FontVariantStatus::NonCanonical))),
		family(2, "Synthetic", profile(outcome(
			FontVariantRole::Unknown, 12, FontVariantStatus::Synthetic))),
		family(3, "No Profile", profile(outcome(
			FontVariantRole::Unknown, 0, FontVariantStatus::Unknown))),
	});
	AssFile file;
	add_style(file, "A", "Noncanonical");
	add_style(file, "B", "Synthetic");
	add_style(file, "C", "No Profile");
	add_style(file, "D", "Missing Family");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(4u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::NonCanonical,
	          plan.findings[0].classification);
	EXPECT_EQ(FontVariantAuditClassification::Synthetic,
	          plan.findings[1].classification);
	EXPECT_EQ(FontVariantAuditClassification::Unknown,
	          plan.findings[2].classification);
	EXPECT_EQ(FontVariantAuditClassification::Unknown,
	          plan.findings[3].classification);
	for (auto const& finding : plan.findings)
		EXPECT_FALSE(finding.safe_to_apply);
}

TEST(font_variant_audit, applies_an_explicit_family_fix_at_the_text_span) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only}text", 9);

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	auto const& finding = plan.findings.front();
	EXPECT_EQ(FontVariantAuditSourceKind::OverrideSpan, finding.source.kind);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          finding.classification);
	EXPECT_EQ(10, finding.source.line);
	EXPECT_TRUE(finding.safe_to_apply);
	EXPECT_FALSE(finding.has_explicit_bold);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(applied.dialogue_text_changed);
	EXPECT_EQ("{\\fnBold Only}{\\b1}text", event.Text.get());

	auto after = BuildFontVariantAuditPlan(file, catalog);
	EXPECT_TRUE(after.findings.empty());
}

TEST(font_variant_audit, coalesces_repairs_across_unrelated_override_blocks) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only}one{\\c&HFFFFFF&}two");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	EXPECT_EQ(plan.findings[0].source.font_state_revision,
	          plan.findings[1].source.font_state_revision);

	std::array<std::size_t, 2> selected{0, 1};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\fnBold Only}{\\b1}one{\\c&HFFFFFF&}two", event.Text.get());
	EXPECT_TRUE(BuildFontVariantAuditPlan(file, catalog).findings.empty());
}

TEST(font_variant_audit, explicit_variant_override_is_report_only_without_opt_in) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only\\b0}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings[0].classification);
	EXPECT_TRUE(plan.findings[0].requires_explicit_replacement);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);
	EXPECT_EQ("explicit_variant_override_preserved", plan.findings[0].reason_code);

	std::array<std::size_t, 1> selected{0};
	auto rejected = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(rejected.success);
	EXPECT_EQ("{\\fnBold Only\\b0}text", event.Text.get());

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\fnBold Only\\b1}text", event.Text.get());
}

TEST(font_variant_audit, build_option_marks_explicit_fix_safe) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Italic Only", italic_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\fnItalic Only\\i0}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_TRUE(plan.findings[0].requires_explicit_replacement);
	EXPECT_TRUE(plan.findings[0].safe_to_apply);
	EXPECT_TRUE(plan.findings[0].pin_italic);
}

TEST(font_variant_audit, replaces_explicit_variant_written_before_family) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\b0\\fnBold Only}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings[0].safe_to_apply);
	ASSERT_TRUE(plan.findings[0].requires_explicit_replacement);
	std::array<std::size_t, 1> selected{0};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\b0\\fnBold Only}{\\b1}text", event.Text.get());
	EXPECT_TRUE(BuildFontVariantAuditPlan(file, catalog).findings.empty());
}

TEST(font_variant_audit, numeric_weight_remains_report_only_even_with_opt_in) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only\\b600}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::NonCanonical,
	          plan.findings[0].classification);
	EXPECT_EQ("numeric_weight_report_only", plan.findings[0].reason_code);
	EXPECT_EQ("600", plan.findings[0].raw_bold_tag);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	EXPECT_FALSE(applied.success);
	EXPECT_EQ("{\\fnBold Only\\b600}text", event.Text.get());
}

TEST(font_variant_audit, transform_derived_variant_state_is_report_only) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\t(0,100,\\fnBold Only)}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_TRUE(plan.findings[0].source.in_transform);
	EXPECT_EQ("transform_variant_report_only", plan.findings[0].reason_code);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);
}

TEST(font_variant_audit, stale_event_rejects_every_selected_fix_before_style_mutation) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	auto& event = add_event(file, "{\\fnBold Only}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	event.Text = "{\\fnBold Only}changed";
	std::array<std::size_t, 2> selected{0, 1};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ("{\\fnBold Only}changed", event.Text.get());
}

TEST(font_variant_audit, stale_style_state_rejects_event_fix_without_mutation) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	auto& event = add_event(file, "{\\fnBold Only}text");
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	style.font = "Base";
	std::array<std::size_t, 2> selected{0, 1};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ("{\\fnBold Only}text", event.Text.get());
}

TEST(font_variant_audit, request_profile_preserves_charset_and_source_font_size) {
	FontFamilyCatalog catalog({family(1, "Dynamic", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Dynamic");
	style.encoding = 128;
	style.fontsize = 42.5;

	std::vector<aegisub::ass::AssFontRequest> requests;
	FontVariantAuditProfileProvider provider = [&](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		requests.push_back(request);
		return bold_only_profile(500);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, provider);

	ASSERT_EQ(1u, requests.size());
	EXPECT_EQ("Dynamic", requests.front().family);
	EXPECT_EQ(128, requests.front().charset);
	EXPECT_DOUBLE_EQ(42.5, requests.front().height);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_DOUBLE_EQ(42.5, plan.findings.front().height);
	EXPECT_EQ(FontSelectionEvidence::Observed,
	          plan.findings.front().profile_evidence);
	ASSERT_EQ(1u, plan.snapshots.size());
	EXPECT_DOUBLE_EQ(42.5, plan.snapshots.front().height);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings.front().classification);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {}, provider);
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(style.bold);
	ASSERT_EQ(2u, requests.size());
	EXPECT_EQ(128, requests.back().charset);
	EXPECT_DOUBLE_EQ(42.5, requests.back().height);
}

TEST(font_variant_audit, override_request_preserves_charset_and_source_font_size) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Dynamic", regular_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\fnDynamic\\fe128\\fs37}text");

	std::optional<aegisub::ass::AssFontRequest> dynamic_request;
	FontVariantAuditProfileProvider provider = [&](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		if (request.family == "Dynamic") {
			dynamic_request = request;
			return bold_only_profile(600);
		}
		return regular_profile();
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, provider);

	ASSERT_TRUE(dynamic_request.has_value());
	EXPECT_EQ(128, dynamic_request->charset);
	EXPECT_DOUBLE_EQ(37.0, dynamic_request->height);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(128, plan.findings.front().charset);
	EXPECT_DOUBLE_EQ(37.0, plan.findings.front().height);
}

TEST(font_variant_audit, stale_style_font_size_rejects_fix) {
	FontFamilyCatalog catalog({family(1, "Bold Only", bold_only_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	style.fontsize = 24.0;
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());

	style.fontsize = 36.0;
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
}

TEST(font_variant_audit, changed_inherited_font_size_rejects_override_fix) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Base");
	style.fontsize = 24.0;
	auto& event = add_event(file, "{\\fnBold Only}text");
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::OverrideSpan,
	          plan.findings.front().source.kind);

	style.fontsize = 36.0;
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_EQ("{\\fnBold Only}text", event.Text.get());
}

TEST(font_variant_audit, apply_rejects_when_live_target_no_longer_matches_entity) {
	FontFamilyCatalog catalog({family(1, "Bold Only", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	FontVariantAuditProfileProvider initial = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(700);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings.front().safe_to_apply);
	std::array<std::size_t, 1> selected{0};

	FontVariantAuditProfileProvider replaced_entity = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(702);
	};
	auto replaced = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, replaced_entity);
	EXPECT_FALSE(replaced.success);
	EXPECT_FALSE(style.bold);

	FontVariantAuditProfileProvider changed_target = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return profile(
			outcome(FontVariantRole::Bold, 700, FontVariantStatus::Canonical, 700),
			outcome(FontVariantRole::Bold, 701, FontVariantStatus::Canonical, 700));
	};
	auto applied = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, changed_target);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(
		"font variant audit live profile changed after the plan was built",
		applied.error);
}

TEST(font_variant_audit, apply_rejects_changed_backend_or_evidence) {
	FontFamilyCatalog catalog({family(1, "Bold Only", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	FontVariantAuditProfileProvider initial = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(800);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings.front().safe_to_apply);
	EXPECT_EQ(FontVariantBackend::VsFilterGdi,
	          plan.findings.front().profile_backend);
	EXPECT_EQ(FontSelectionEvidence::Observed,
	          plan.findings.front().profile_evidence);
	std::array<std::size_t, 1> selected{0};

	FontVariantAuditProfileProvider different_backend = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		auto profile = bold_only_profile(800);
		profile.backend = FontVariantBackend::CoreText;
		return profile;
	};
	auto different = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, different_backend);
	EXPECT_FALSE(different.success);
	EXPECT_FALSE(style.bold);

	FontVariantAuditProfileProvider algorithmic = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		auto profile = bold_only_profile(800);
		profile.evidence = FontSelectionEvidence::Algorithmic;
		profile.automatic_pinning_reliable = true;
		return profile;
	};
	auto applied = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, algorithmic);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(
		"font variant audit live profile changed after the plan was built",
		applied.error);
}

TEST(font_variant_audit, style_pin_preserves_inherited_text_after_family_switch) {
	FontFamilyCatalog catalog({
		family(1, "BoldOnly", bold_only_profile()),
		family(2, "CompleteRBIZ", regular_profile()),
	});
	AssFile file;
	add_style(file, "Default", "BoldOnly");
	add_event(file, "A{\\fnCompleteRBIZ}B");
	add_event(file, "C{\\fnCompleteRBIZ}D", 1);
	auto before_ass = snapshot_ass(file);
	auto before_rendered = replay_rendered_text(file, catalog);
	std::vector<RenderedAuditCharacter> expected{
		rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		rendered_character(0, 'B', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
		rendered_character(1, 'C', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		rendered_character(1, 'D', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
	};
	ASSERT_EQ(expected, before_rendered);

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
	EXPECT_EQ("BoldOnly", plan.findings[0].family);
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
}

TEST(font_variant_audit, span_pin_does_not_leak_into_following_family) {
	FontFamilyCatalog catalog({
		family(1, "CompleteRBIZ", regular_profile()),
		family(2, "BoldOnly", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "CompleteRBIZ");
	add_event(file, R"({\fnBoldOnly}A{\fnCompleteRBIZ}B{\r}C)");
	auto before_ass = snapshot_ass(file);
	auto before_rendered = replay_rendered_text(file, catalog);
	std::vector<RenderedAuditCharacter> expected{
		rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		rendered_character(0, 'B', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
		rendered_character(0, 'C', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
	};
	ASSERT_EQ(expected, before_rendered);

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::OverrideSpan, plan.findings[0].source.kind);
	EXPECT_EQ("BoldOnly", plan.findings[0].family);
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
}

TEST(font_variant_audit, style_pin_preserves_named_reset_and_event_style_inheritance) {
	FontFamilyCatalog catalog({
		family(1, "CompleteRBIZ", regular_profile()),
		family(2, "BoldOnly", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "CompleteRBIZ");
	add_style(file, "Accent", "BoldOnly");
	add_event(file, R"({\rAccent}A{\fnCompleteRBIZ}B{\r}C)");
	add_event(file, "D", 1, "Accent");
	auto before_ass = snapshot_ass(file);
	auto before_rendered = replay_rendered_text(file, catalog);
	std::vector<RenderedAuditCharacter> expected{
		rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		rendered_character(0, 'B', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
		rendered_character(0, 'C', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
		rendered_character(1, 'D', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
	};
	ASSERT_EQ(expected, before_rendered);

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
	EXPECT_EQ("Accent", plan.findings[0].source.style);
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
}

TEST(font_variant_audit, safe_span_pins_respect_explicit_weight_and_reset_boundaries) {
	FontFamilyCatalog catalog({
		family(1, "CompleteRBIZ", regular_profile()),
		family(2, "BoldOnly", bold_only_profile()),
	});
	std::array<std::string, 4> boundaries{
		"{\\fnCompleteRBIZ\\b0}B",
		"{\\r}B",
		"{\\rBase}B",
		R"({\p1}m 0 0 l 10 10{\p0\fnCompleteRBIZ\b0}B)",
	};
	auto drawing_text = [](AssDialogue const& event) {
		std::vector<std::string> drawings;
		for (auto const& block : event.ParseTags()) {
			if (block->GetType() == AssBlockType::DRAWING)
				drawings.push_back(block->GetRawText());
		}
		return drawings;
	};
	for (auto const& boundary : boundaries) {
		SCOPED_TRACE(boundary);
		AssFile file;
		add_style(file, "Default", "CompleteRBIZ");
		add_style(file, "Base", "CompleteRBIZ");
		auto& event = add_event(file, "{\\fnBoldOnly}A" + boundary);
		auto before_ass = snapshot_ass(file);
		auto before_rendered = replay_rendered_text(file, catalog);
		std::vector<RenderedAuditCharacter> expected{
			rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
			rendered_character(0, 'B', "CompleteRBIZ", outcome(FontVariantRole::Regular, 100)),
		};
		ASSERT_EQ(expected, before_rendered);
		auto original_text = event.Text.get();
		auto drawings_before = drawing_text(event);
		if (boundary.find("\\p1") != std::string::npos) {
			ASSERT_EQ(1u, drawings_before.size());
			EXPECT_EQ("m 0 0 l 10 10", drawings_before.front());
		}

		auto plan = BuildFontVariantAuditPlan(file, catalog);
		ASSERT_EQ(1u, plan.findings.size());
		std::array<std::size_t, 1> selected{0};
		auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
		ASSERT_TRUE(applied.success) << applied.error;
		EXPECT_FALSE(applied.styles_changed);
		EXPECT_TRUE(applied.dialogue_text_changed);
		EXPECT_NE(original_text, event.Text.get());
		EXPECT_EQ(drawings_before, drawing_text(event));
		expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
	}
}

TEST(font_variant_audit, individually_safe_style_and_span_pins_are_checked_as_one_selection) {
	FontFamilyCatalog catalog({
		family(1, "BoldOnly", bold_only_profile()),
		family(2, "ItalicFlat", profile(outcome(FontVariantRole::Italic, 20, FontVariantStatus::Canonical, 400, true), outcome(FontVariantRole::Italic, 20, FontVariantStatus::Canonical, 400, true), outcome(FontVariantRole::Italic, 20, FontVariantStatus::Canonical, 400, true), outcome(FontVariantRole::Italic, 20, FontVariantStatus::Canonical, 400, true))),
		family(3, "Corner", profile(outcome(FontVariantRole::Regular, 100), outcome(FontVariantRole::Regular, 100), outcome(FontVariantRole::Regular, 100), outcome(FontVariantRole::BoldItalic, 103, FontVariantStatus::Canonical, 700, true))),
	});
	std::array<std::vector<std::size_t>, 4> selections{{{0}, {1}, {0, 1}, {1, 0}}};
	for (auto const& selected : selections) {
		SCOPED_TRACE(::testing::PrintToString(selected));
		AssFile file;
		add_style(file, "Default", "BoldOnly");
		add_event(file, "{\\fnItalicFlat}A{\\fnCorner}B");
		add_event(file, "C", 1);
		auto before_ass = snapshot_ass(file);
		auto before_rendered = replay_rendered_text(file, catalog);
		std::vector<RenderedAuditCharacter> expected{
			rendered_character(0, 'A', "ItalicFlat", outcome(FontVariantRole::Italic, 20, FontVariantStatus::Canonical, 400, true)),
			rendered_character(0, 'B', "Corner", outcome(FontVariantRole::Regular, 100)),
			rendered_character(1, 'C', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		};
		ASSERT_EQ(expected, before_rendered);

		auto plan = BuildFontVariantAuditPlan(file, catalog);
		ASSERT_EQ(2u, plan.findings.size());
		ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
		ASSERT_EQ(FontVariantAuditSourceKind::OverrideSpan, plan.findings[1].source.kind);
		auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
		if (selected.size() == 1)
			ASSERT_TRUE(applied.success) << applied.error;
		expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
	}
}

TEST(font_variant_audit, apply_rechecks_live_profile_of_dependent_unselected_text) {
	auto regular_only = profile(
		outcome(FontVariantRole::Regular, 100),
		outcome(FontVariantRole::Regular, 100));
	FontFamilyCatalog catalog({
		family(1, "BoldOnly", bold_only_profile()),
		family(2, "Dependent", regular_only),
	});
	FontVariantAuditProfileProvider initial = [regular_only](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		if (request.family == "BoldOnly")
			return bold_only_profile();
		if (request.family == "Dependent")
			return regular_only;
		return std::nullopt;
	};
	std::array<FontFamilyVariantProfile, 4> live_profiles{
		regular_only,
		regular_profile(),
		profile(outcome(FontVariantRole::Regular, 100),
				outcome(FontVariantRole::Regular, 100, FontVariantStatus::Synthetic)),
		profile(outcome(FontVariantRole::Regular, 100),
				outcome(FontVariantRole::Regular, 100, FontVariantStatus::Canonical, 500)),
	};
	for (std::size_t live_index = 0; live_index < live_profiles.size(); ++live_index) {
		SCOPED_TRACE(live_index);
		AssFile file;
		add_style(file, "Default", "BoldOnly");
		add_event(file, "A{\\fnDependent}B");
		auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
		ASSERT_EQ(1u, plan.findings.size());
		ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
		EXPECT_EQ("BoldOnly", plan.findings[0].family);

		FontVariantAuditProfileProvider live = [=](auto const& request)
			-> std::optional<FontFamilyVariantProfile> {
			if (request.family == "Dependent")
				return live_profiles[live_index];
			return initial(request);
		};
		auto before_ass = snapshot_ass(file);
		auto before_rendered = replay_rendered_text(file, catalog, live);
		std::vector<RenderedAuditCharacter> expected{
			rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
			rendered_character(0, 'B', "Dependent", outcome(FontVariantRole::Regular, 100)),
		};
		ASSERT_EQ(expected, before_rendered);
		std::array<std::size_t, 1> selected{0};
		auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {}, live);
		if (live_index == 0)
			ASSERT_TRUE(applied.success) << applied.error;
		expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied, live);
	}
}

TEST(font_variant_audit, font_transform_is_report_only_but_color_transform_allows_safe_pin) {
	FontFamilyCatalog catalog({family(1, "BoldOnly", bold_only_profile())});
	std::array<std::pair<std::string, bool>, 2> transforms{{
		{"{\\t(\\b1)}A", true},
		{"{\\t(\\c&HFFFFFF&)}A", false},
	}};
	for (auto const& [text, changes_font] : transforms) {
		SCOPED_TRACE(text);
		AssFile file;
		add_style(file, "Default", "BoldOnly");
		add_event(file, text);
		auto before_ass = snapshot_ass(file);
		auto before_rendered = replay_rendered_text(file, catalog);
		std::vector<RenderedAuditCharacter> expected{
			rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		};
		ASSERT_EQ(expected, before_rendered);

		auto plan = BuildFontVariantAuditPlan(file, catalog);
		ASSERT_EQ(1u, plan.findings.size());
		ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
		std::array<std::size_t, 1> selected{0};
		auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
		if (changes_font) {
			expect_rejected_apply(file, before_ass, applied);
		}
		else {
			ASSERT_TRUE(applied.success) << applied.error;
		}
		expect_safe_apply(file, catalog, before_ass, before_rendered, selected.size(), applied);
	}
}

TEST(font_variant_audit, unresolved_event_style_or_reset_rejects_style_pin_atomically) {
	FontFamilyCatalog catalog({family(1, "BoldOnly", bold_only_profile())});
	std::array<std::pair<std::string, std::string>, 2> events{{
		{"A{\\rMissing}B", "Default"},
		{"A", "Missing"},
	}};
	for (auto const& [text, style] : events) {
		SCOPED_TRACE(::testing::Message() << text << ':' << style);
		AssFile file;
		add_style(file, "Default", "BoldOnly");
		add_event(file, text, 0, style);
		auto before_ass = snapshot_ass(file);
		auto plan = BuildFontVariantAuditPlan(file, catalog);
		ASSERT_EQ(1u, plan.findings.size());
		ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
		std::array<std::size_t, 1> selected{0};

		auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
		expect_rejected_apply(file, before_ass, applied);
	}
}

TEST(font_variant_audit, missing_live_profile_for_dependent_text_rejects_atomically) {
	auto regular_only = profile(
		outcome(FontVariantRole::Regular, 100),
		outcome(FontVariantRole::Regular, 100));
	FontFamilyCatalog catalog({
		family(1, "BoldOnly", bold_only_profile()),
		family(2, "Dependent", regular_only),
	});
	AssFile file;
	add_style(file, "Default", "BoldOnly");
	add_event(file, "A{\\fnDependent}B");
	FontVariantAuditProfileProvider initial = [regular_only](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		if (request.family == "BoldOnly")
			return bold_only_profile();
		if (request.family == "Dependent")
			return regular_only;
		return std::nullopt;
	};
	auto before_ass = snapshot_ass(file);
	auto before_rendered = replay_rendered_text(file, catalog, initial);
	std::vector<RenderedAuditCharacter> expected{
		rendered_character(0, 'A', "BoldOnly", outcome(FontVariantRole::Bold, 10, FontVariantStatus::Canonical, 700)),
		rendered_character(0, 'B', "Dependent", outcome(FontVariantRole::Regular, 100)),
	};
	ASSERT_EQ(expected, before_rendered);
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::Style, plan.findings[0].source.kind);
	FontVariantAuditProfileProvider live = [](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		if (request.family == "BoldOnly")
			return bold_only_profile();
		return std::nullopt;
	};
	std::array<std::size_t, 1> selected{0};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {}, live);
	expect_rejected_apply(file, before_ass, applied);
}
