#include "../../src/font_portability_audit.h"

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_font_state.h"
#include "../../src/ass_style.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace {

FontVariantOutcome outcome(
	FontVariantRole role,
	std::uint64_t entity,
	int weight,
	bool italic = false) {
	FontVariantOutcome result;
	result.realized_weight = weight;
	result.realized_italic = italic;
	result.role = role;
	result.status = FontVariantStatus::Canonical;
	result.entity_token = entity;
	return result;
}

FontFamilyVariantProfile regular_profile(std::uint64_t base) {
	return BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Regular, base, 400),
		outcome(FontVariantRole::Bold, base + 1, 700),
		outcome(FontVariantRole::Italic, base + 2, 400, true),
		outcome(FontVariantRole::BoldItalic, base + 3, 700, true),
	}, FontVariantBackend::VsFilterGdi,
	   FontSelectionEvidence::Observed, true);
}

FontFamilyVariantProfile bold_only_profile(std::uint64_t entity) {
	return BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Bold, entity, 700),
		outcome(FontVariantRole::Bold, entity, 700),
		{},
		{},
	}, FontVariantBackend::VsFilterGdi,
	   FontSelectionEvidence::Observed, true);
}

FontFamilyRecord family(
	FontFamilyId id,
	std::string localized,
	std::string english,
	FontFamilyVariantProfile profile) {
	FontFamilyRecord result;
	result.id = id;
	result.localized_family_name = std::move(localized);
	result.english_win32_family_name = std::move(english);
	result.variant_profile = std::move(profile);
	return result;
}

AssStyle& add_style(AssFile& file, std::string name, std::string font) {
	auto* style = new AssStyle;
	style->name = std::move(name);
	style->font = std::move(font);
	style->UpdateData();
	file.Styles.push_back(*style);
	return *style;
}

AssDialogue& add_event(AssFile& file, std::string text, std::string style = "Default") {
	auto* event = new AssDialogue;
	event->Text = std::move(text);
	event->Style = std::move(style);
	file.Events.push_back(*event);
	return *event;
}

FontPortabilityAuditOptions english_names() {
	FontPortabilityAuditOptions options;
	options.name_target = FontNameNormalizationTarget::EnglishWin32;
	return options;
}

FontVariantAuditProfileProvider observed_profiles(std::map<std::string, FontFamilyVariantProfile> profiles) {
	return [profiles = std::move(profiles)](aegisub::ass::AssFontRequest const& probe)
			   -> std::optional<FontFamilyVariantProfile> {
		auto const found = profiles.find(probe.family);
		if (found == profiles.end()) return std::nullopt;
		return found->second;
	};
}

FontPortabilityAuditPlan selection_plan() {
	FontPortabilityAuditPlan plan;
	plan.names.changes.resize(2);
	plan.names.changes[0].safe_to_apply = true;
	plan.names.changes[1].safe_to_apply = false;
	plan.variants.findings.resize(2);
	plan.variants.findings[0].safe_to_apply = true;
	plan.variants.findings[1].safe_to_apply = false;
	return plan;
}

} // namespace

TEST(font_portability_audit, selection_defaults_follow_the_entry_filter) {
	auto plan = selection_plan();
	FontPortabilityAuditSelection selection;

	selection.Reset(plan, FontPortabilityAuditFilter::Names);
	EXPECT_TRUE(selection.IsNameSelected(0));
	EXPECT_FALSE(selection.IsNameSelected(1));
	EXPECT_FALSE(selection.IsVariantSelected(0));

	selection.Reset(plan, FontPortabilityAuditFilter::Variants);
	EXPECT_FALSE(selection.IsNameSelected(0));
	EXPECT_TRUE(selection.IsVariantSelected(0));

	selection.Reset(plan, FontPortabilityAuditFilter::All);
	EXPECT_TRUE(selection.IsNameSelected(0));
	EXPECT_TRUE(selection.IsVariantSelected(0));
}

TEST(font_portability_audit, selection_safe_and_clear_only_touch_the_visible_category) {
	auto plan = selection_plan();
	FontPortabilityAuditSelection selection;
	selection.Reset(plan, FontPortabilityAuditFilter::All);
	selection.SetVariantSelected(plan.variants, 0, false);

	selection.SelectSafe(plan, FontPortabilityAuditFilter::Names, false);
	EXPECT_FALSE(selection.IsNameSelected(0));
	EXPECT_FALSE(selection.IsVariantSelected(0));

	selection.SelectSafe(plan, FontPortabilityAuditFilter::Names, true);
	EXPECT_TRUE(selection.IsNameSelected(0));
	EXPECT_FALSE(selection.IsVariantSelected(0));

	EXPECT_FALSE(selection.SetNameSelected(plan.names, 1, true));
	EXPECT_FALSE(selection.IsNameSelected(1));
}

TEST(font_portability_audit, selection_collects_hidden_category_and_survives_other_reset) {
	auto plan = selection_plan();
	FontPortabilityAuditSelection selection;
	selection.Reset(plan, FontPortabilityAuditFilter::All);
	selection.SetNameSelected(plan.names, 0, false);

	EXPECT_EQ(std::vector<std::size_t>{}, selection.SelectedNameIndices());
	EXPECT_EQ(std::vector<std::size_t>{0}, selection.SelectedVariantIndices());

	selection.ResetNames(plan.names, false);
	EXPECT_EQ(std::vector<std::size_t>{0}, selection.SelectedVariantIndices());
}

TEST(font_portability_audit, applies_name_and_variant_to_one_style_atomically) {
	FontFamilyCatalog catalog({family(
		1, "Localized Bold", "English Bold", bold_only_profile(10))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Bold");
	auto* original_style = &style;
	auto options = english_names();
	auto provider = observed_profiles({
		{"Localized Bold", bold_only_profile(10)},
		{"English Bold", bold_only_profile(10)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_EQ(1u, plan.variants.findings.size());

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, selected, selected, options, provider);

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_TRUE(result.styles_changed);
	EXPECT_FALSE(result.dialogue_text_changed);
	EXPECT_EQ(1u, result.applied_name_change_count);
	EXPECT_EQ(1u, result.applied_variant_change_count);
	EXPECT_EQ(original_style, &file.Styles.front());
	EXPECT_EQ("English Bold", style.font);
	EXPECT_TRUE(style.bold);
}

TEST(font_portability_audit, combines_name_and_variant_edits_on_one_override_span) {
	FontFamilyCatalog catalog({
		family(1, "Regular", "Regular", regular_profile(100)),
		family(2, "Localized Bold", "English Bold", bold_only_profile(20)),
	});
	AssFile file;
	add_style(file, "Default", "Regular");
	auto& event = add_event(file, "{\\fnLocalized Bold}A");
	auto* original_event = &event;
	auto options = english_names();
	auto provider = observed_profiles({
		{"Regular", regular_profile(100)},
		{"Localized Bold", bold_only_profile(20)},
		{"English Bold", bold_only_profile(20)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_EQ(1u, plan.variants.findings.size());

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, selected, selected, options, provider);

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_FALSE(result.styles_changed);
	EXPECT_TRUE(result.dialogue_text_changed);
	EXPECT_EQ(original_event, &file.Events.front());
	EXPECT_NE(std::string::npos, event.Text.get().find("\\fnEnglish Bold"));
	EXPECT_NE(std::string::npos, event.Text.get().find("\\b1"));
}

TEST(font_portability_audit, second_plan_failure_leaves_original_file_untouched) {
	FontFamilyCatalog catalog({
		family(1, "Localized Regular", "English Regular", regular_profile(100)),
		family(2, "Bold Only", "Bold Only", bold_only_profile(20)),
	});
	AssFile file;
	auto& name_style = add_style(file, "Name", "Localized Regular");
	auto& variant_style = add_style(file, "Variant", "Bold Only");
	auto options = english_names();
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_EQ(1u, plan.variants.findings.size());

	name_style.font = "Changed after scan";
	name_style.UpdateData();
	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, selected, selected, options);

	EXPECT_FALSE(result.success);
	EXPECT_EQ("Changed after scan", name_style.font);
	EXPECT_FALSE(variant_style.bold);
}

TEST(font_portability_audit, applies_only_the_selected_audit_category) {
	FontFamilyCatalog catalog({family(
		1, "Localized Bold", "English Bold", bold_only_profile(10))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Bold");
	auto options = english_names();
	auto provider = observed_profiles({
		{"Localized Bold", bold_only_profile(10)},
		{"English Bold", bold_only_profile(10)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_EQ(1u, plan.variants.findings.size());

	std::array<std::size_t, 1> selected_name{0};
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, selected_name, {}, options, provider);

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_EQ("English Bold", style.font);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(1u, result.applied_name_change_count);
	EXPECT_EQ(0u, result.applied_variant_change_count);
}

TEST(font_portability_audit, rejects_same_family_alias_entity_change_before_atomic_commit) {
	FontFamilyCatalog catalog({family(
		1, "Localized Regular", "English Regular", regular_profile(100))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Regular");
	auto& event = add_event(file, "Plain{\\b1}bold{\\b0}plain");
	auto const original_style = style.GetEntryData();
	auto const original_text = event.Text.get();
	auto options = english_names();
	auto build_provider = observed_profiles({
		{"Localized Regular", regular_profile(100)},
		{"English Regular", regular_profile(100)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, build_provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_TRUE(plan.names.changes.front().safe_to_apply);
	ASSERT_TRUE(plan.variants.findings.empty());
	ASSERT_EQ(catalog.Resolve("Localized Regular").family, catalog.Resolve("English Regular").family);
	auto apply_provider = observed_profiles({
		{"Localized Regular", regular_profile(100)},
		{"English Regular", regular_profile(200)},
	});

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(file, plan, selected, {}, options, apply_provider);

	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
	EXPECT_FALSE(result.styles_changed);
	EXPECT_FALSE(result.dialogue_text_changed);
	EXPECT_EQ(0u, result.applied_name_change_count);
	EXPECT_EQ(0u, result.applied_variant_change_count);
	EXPECT_EQ("Localized Regular", style.font);
	EXPECT_EQ(original_style, style.GetEntryData());
	EXPECT_EQ(original_text, event.Text.get());
}

TEST(font_portability_audit, rejects_combined_name_and_variant_when_target_alias_entity_changes) {
	FontFamilyCatalog catalog({family(
		1, "Localized Bold", "English Bold", bold_only_profile(20))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Bold");
	auto const original_style = style.GetEntryData();
	auto options = english_names();
	auto build_provider = observed_profiles({
		{"Localized Bold", bold_only_profile(20)},
		{"English Bold", bold_only_profile(20)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, build_provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_EQ(1u, plan.variants.findings.size());
	ASSERT_TRUE(plan.names.changes.front().safe_to_apply);
	ASSERT_TRUE(plan.variants.findings.front().safe_to_apply);
	ASSERT_TRUE(file.Events.empty());
	auto apply_provider = observed_profiles({
		{"Localized Bold", bold_only_profile(20)},
		{"English Bold", bold_only_profile(30)},
	});

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(file, plan, selected, selected, options, apply_provider);

	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
	EXPECT_FALSE(result.styles_changed);
	EXPECT_FALSE(result.dialogue_text_changed);
	EXPECT_EQ(0u, result.applied_name_change_count);
	EXPECT_EQ(0u, result.applied_variant_change_count);
	EXPECT_EQ("Localized Bold", style.font);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(original_style, style.GetEntryData());
	EXPECT_TRUE(file.Events.empty());
}

TEST(font_portability_audit, rejects_name_change_when_live_target_alias_is_unavailable) {
	FontFamilyCatalog catalog({family(
		1, "Localized Regular", "English Regular", regular_profile(100))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Regular");
	auto& event = add_event(file, "Plain text");
	auto const original_style = style.GetEntryData();
	auto const original_text = event.Text.get();
	auto options = english_names();
	auto build_provider = observed_profiles({
		{"Localized Regular", regular_profile(100)},
		{"English Regular", regular_profile(100)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, build_provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_TRUE(plan.names.changes.front().safe_to_apply);
	auto apply_provider = observed_profiles({{"Localized Regular", regular_profile(100)}});

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(file, plan, selected, {}, options, apply_provider);

	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
	EXPECT_EQ(0u, result.applied_name_change_count);
	EXPECT_EQ(0u, result.applied_variant_change_count);
	EXPECT_EQ("Localized Regular", style.font);
	EXPECT_EQ(original_style, style.GetEntryData());
	EXPECT_EQ(original_text, event.Text.get());
}

TEST(font_portability_audit, catalog_profile_does_not_prove_renamed_alias_without_live_provider) {
	FontFamilyCatalog catalog({family(
		1, "Localized Regular", "English Regular", regular_profile(100))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Regular");
	auto const original_style = style.GetEntryData();
	auto options = english_names();
	auto build_provider = observed_profiles({
		{"Localized Regular", regular_profile(100)},
		{"English Regular", regular_profile(100)},
	});
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options, {}, build_provider);
	ASSERT_EQ(1u, plan.names.changes.size());
	ASSERT_TRUE(plan.names.changes.front().safe_to_apply);

	std::array<std::size_t, 1> selected{0};
	auto result = ApplyFontPortabilityAuditChanges(file, plan, selected, {}, options);

	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
	EXPECT_EQ(0u, result.applied_name_change_count);
	EXPECT_EQ(0u, result.applied_variant_change_count);
	EXPECT_EQ("Localized Regular", style.font);
	EXPECT_EQ(original_style, style.GetEntryData());
}

TEST(font_portability_audit, option_mismatch_does_not_modify_the_file) {
	FontFamilyCatalog catalog({family(
		1, "Localized Bold", "English Bold", bold_only_profile(10))});
	AssFile file;
	auto& style = add_style(file, "Default", "Localized Bold");
	auto options = english_names();
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options);
	ASSERT_EQ(1u, plan.names.changes.size());

	auto changed_options = options;
	changed_options.variant.allow_replace_explicit = true;
	std::array<std::size_t, 1> selected_name{0};
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, selected_name, {}, changed_options);

	EXPECT_FALSE(result.success);
	EXPECT_EQ("Localized Bold", style.font);
	EXPECT_FALSE(style.bold);
}

TEST(font_portability_audit, staging_does_not_consume_dialogue_ids) {
	FontFamilyCatalog catalog({family(
		1, "Regular", "Regular", regular_profile(10))});
	AssFile file;
	add_style(file, "Default", "Regular");
	add_event(file, "Text");
	auto options = english_names();
	auto plan = BuildFontPortabilityAuditPlan(file, catalog, options);

	AssDialogue before;
	auto const before_id = before.Id;
	auto result = ApplyFontPortabilityAuditChanges(
		file, plan, {}, {}, options);
	AssDialogue after;

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_EQ(before_id + 1, after.Id);
}
