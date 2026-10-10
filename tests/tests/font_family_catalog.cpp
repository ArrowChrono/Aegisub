#include <gtest/gtest.h>

#include "../../src/font_face_selection.h"
#include "../../src/font_family_catalog.h"
#include "../../src/font_family_selection_model.h"

#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace {

FontFamilyRecord family(
	FontFamilyId id,
	std::string localized,
	std::string english,
	std::vector<FontFamilyName> aliases = {})
{
	FontFamilyRecord record;
	record.id = id;
	record.localized_family_name = std::move(localized);
	record.english_win32_family_name = std::move(english);
	record.names = std::move(aliases);
	return record;
}

} // namespace

TEST(font_family_catalog, maps_localized_and_english_names_to_preferred_family_name) {
	FontFamilyCatalog catalog({family(1, "\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", "Microsoft YaHei")});

	auto localized = catalog.Resolve("\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91");
	ASSERT_EQ(FontFamilyMatchKind::Exact, localized.match);
	EXPECT_EQ(1u, localized.family);
	EXPECT_EQ("Microsoft YaHei", catalog.MapToPreferredWriteName(
		"\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", false));
	EXPECT_EQ("\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", catalog.MapToPreferredWriteName(
		"Microsoft YaHei", true));
	EXPECT_EQ("Microsoft YaHei", catalog.MapToPreferredWriteName(
		"microsoft yahei", false));
}

TEST(font_family_catalog, falls_back_to_localized_name_when_english_name_is_unavailable) {
	FontFamilyCatalog catalog({family(1, "Localized Only", "")});

	EXPECT_EQ("Localized Only", catalog.PreferredWriteName(1, false));
	EXPECT_EQ("Localized Only", catalog.MapToPreferredWriteName("Localized Only", false));
}

TEST(font_family_catalog, leaves_ambiguous_alias_unchanged) {
	FontFamilyName alias{"Shared Alias", "en-US", FontFamilyNameKind::Win32Family};
	FontFamilyCatalog catalog({
		family(1, "Localized A", "English A", {alias}),
		family(2, "Localized B", "English B", {alias}),
	});

	auto result = catalog.Resolve("Shared Alias");
	EXPECT_EQ(FontFamilyMatchKind::Ambiguous, result.match);
	EXPECT_FALSE(result.family.has_value());
	EXPECT_EQ("Shared Alias", catalog.MapToPreferredWriteName("Shared Alias", false));
}

TEST(font_family_catalog, diagnostic_platform_alias_is_not_auto_resolved) {
	FontFamilyCatalog catalog({family(1, "Duplicated Family", "", {
		{"Duplicated Family", "", FontFamilyNameKind::PlatformAlias},
	})});

	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Duplicated Family").match);
	EXPECT_EQ("Duplicated Family", catalog.MapToPreferredWriteName(
		"Duplicated Family", false));
}

TEST(font_family_catalog, exact_alias_wins_before_case_insensitive_ambiguity) {
	FontFamilyCatalog catalog({
		family(1, "Localized A", "Alias"),
		family(2, "Localized B", "ALIAS"),
	});

	EXPECT_EQ(1u, catalog.Resolve("Alias").family);
	EXPECT_EQ(2u, catalog.Resolve("ALIAS").family);
	EXPECT_EQ(FontFamilyMatchKind::Ambiguous, catalog.Resolve("alias").match);
}

#ifdef _WIN32
TEST(font_family_catalog, resolves_non_ascii_aliases_with_windows_ordinal_case_folding) {
	FontFamilyCatalog catalog({family(
		1,
		"Font \xD0\x96",
		"Gr\xC3\x85" "nd")});

	auto cyrillic = catalog.Resolve("fONT \xD0\xB6");
	ASSERT_EQ(FontFamilyMatchKind::CaseInsensitiveExact, cyrillic.match);
	EXPECT_EQ(1u, cyrillic.family);

	auto latin = catalog.Resolve("gr\xC3\xA5" "ND");
	ASSERT_EQ(FontFamilyMatchKind::CaseInsensitiveExact, latin.match);
	EXPECT_EQ(1u, latin.family);
}
#endif

TEST(font_family_catalog, indexes_only_win32_family_names_as_ass_aliases) {
	FontFamilyCatalog catalog({family(1, "Localized Family", "English Family", {
		{"Win32 Alias", "en-US", FontFamilyNameKind::Win32Family},
		{"Typographic Family", "en-US", FontFamilyNameKind::TypographicFamily},
		{"Full Face Name", "en-US", FontFamilyNameKind::FullName},
		{"PostScript-Name", "en-US", FontFamilyNameKind::PostScript},
		{"Platform Alias", "", FontFamilyNameKind::PlatformAlias},
	})});

	EXPECT_EQ(FontFamilyMatchKind::Exact, catalog.Resolve("Win32 Alias").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Typographic Family").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Full Face Name").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("PostScript-Name").match);
	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Platform Alias").match);
}

TEST(font_family_catalog, informational_names_resolve_only_with_unique_family_and_variant) {
	FontFamilyName bold_full{
		"Example Bold", "en-US", FontFamilyNameKind::FullName,
		22, FontVariantRole::Bold};
	FontFamilyCatalog catalog({
		family(1, "Localized Example", "Example", {bold_full}),
	});

	EXPECT_EQ(FontFamilyMatchKind::None, catalog.Resolve("Example Bold").match);
	auto resolved = catalog.ResolveInformationalName("@Example Bold");
	ASSERT_EQ(FontFamilyMatchKind::Exact, resolved.match);
	ASSERT_EQ(1u, resolved.family);
	EXPECT_EQ(FontVariantRole::Bold, resolved.variant_role);

	FontFamilyCatalog ambiguous({
		family(1, "Family A", "A", {bold_full}),
		family(2, "Family B", "B", {bold_full}),
	});
	EXPECT_EQ(
		FontFamilyMatchKind::Ambiguous,
		ambiguous.ResolveInformationalName("Example Bold").match);
}

TEST(font_family_catalog, informational_name_index_preserves_entity_ambiguity) {
	FontFamilyName bold_a{
		"Shared Bold", "en-US", FontFamilyNameKind::FullName,
		22, FontVariantRole::Bold};
	FontFamilyName bold_b = bold_a;
	bold_b.entity_token = 23;
	FontFamilyCatalog catalog({family(1, "Family A", "A", {bold_a, bold_b})});

	EXPECT_EQ(
		FontFamilyMatchKind::Ambiguous,
		catalog.ResolveInformationalName("Shared Bold").match);
}

TEST(font_family_catalog, vertical_prefix_counts_toward_gdi_limit) {
	auto thirty_one = std::string(31, 'A');
	auto thirty = std::string(30, 'B');
	FontFamilyCatalog catalog({
		family(1, "Local31", thirty_one),
		family(2, "Local30", thirty),
	});

	EXPECT_EQ(thirty_one, catalog.MapToPreferredWriteName("Local31", false));
	EXPECT_EQ("@Local31", catalog.MapToPreferredWriteName("@Local31", false));
	EXPECT_EQ("@" + thirty, catalog.MapToPreferredWriteName("@Local30", false));
	EXPECT_EQ(31u, FontFamilyCatalog::Utf16CodeUnitLength("@" + thirty));
	EXPECT_EQ(2u, FontFamilyCatalog::Utf16CodeUnitLength("\xF0\x9F\x98\x80"));
	EXPECT_FALSE(FontFamilyCatalog::ExceedsGdiFaceNameLimit(thirty_one));
	EXPECT_TRUE(FontFamilyCatalog::ExceedsGdiFaceNameLimit(thirty_one + "X"));
	EXPECT_TRUE(FontFamilyCatalog::ExceedsGdiFaceNameLimit("@" + thirty_one));
}

TEST(font_face_selection, writes_font_name_only_when_face_was_modified) {
	EXPECT_FALSE(ShouldWriteFontFace(
		"Localized Family", "English Family", false));
	EXPECT_FALSE(ShouldWriteFontFace(
		"Other Family", "English Family", false));
	EXPECT_FALSE(ShouldWriteFontFace(
		"@Localized Family", "@English Family", false));
	EXPECT_TRUE(ShouldWriteFontFace(
		"Localized Family", "English Family", true));
	EXPECT_TRUE(ShouldWriteFontFace(
		"Arial", "English Family", true));
	EXPECT_FALSE(ShouldWriteFontFace(
		"English Family", "English Family", true));
	EXPECT_TRUE(ShouldWriteFontFace(
		"English Family", "english family", true));
}

TEST(font_family_selection_model, candidate_labels_follow_name_preference) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{family(1, "Localized Family", "English Family")});

	auto const english_model = BuildFontFamilySelectionModel(catalog, false);
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"English Family", 1}}), english_model.choices);
	auto const localized_model = BuildFontFamilySelectionModel(catalog, true);
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"Localized Family", 1}}), localized_model.choices);
}

TEST(font_family_selection_model, selected_choice_id_disambiguates_duplicate_display_aliases) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{
			family(7, "Localized A", "Shared Alias"),
			family(9, "Localized B", "Shared Alias")});
	FontFamilySelectionModel model;
	model.catalog = catalog;
	model.choices = {{"Shared Alias", 7}, {"Shared Alias", 9}};

	EXPECT_EQ(nullptr, model.ResolveRecord("Shared Alias"));
	auto const *first = model.ResolveChoice(model.choices[0].family_id);
	auto const *second = model.ResolveChoice(model.choices[1].family_id);
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ("Localized A", first->localized_family_name);
	EXPECT_EQ("Localized B", second->localized_family_name);
}

TEST(font_family_selection_model, builds_sorted_catalog_choices_with_stable_ids) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{
			family(9, "Localized B", "Shared Alias"),
			family(7, "Localized A", "Shared Alias"),
			family(3, "Localized C", "Earlier Alias")});

	auto const model = BuildFontFamilySelectionModel(catalog, false);
	EXPECT_EQ((std::vector<FontFamilyChoice>{
		{"Earlier Alias", 3},
		{"Shared Alias", 7},
		{"Shared Alias", 9},
	}), model.choices);
}

TEST(font_family_selection_model, preserves_ordered_fallback_names_without_catalog_ids) {
	auto const model = BuildFontFamilySelectionModel(
		nullptr, true, {"Fallback B", "Fallback A"});

	EXPECT_EQ((std::vector<FontFamilyChoice>{
		{"Fallback B", 0},
		{"Fallback A", 0},
	}), model.choices);
	EXPECT_EQ(nullptr, model.ResolveChoice(0));
}

TEST(font_family_selection_model, system_list_fallback_preserves_gdi_vertical_rows) {
	auto model = BuildFontFamilySelectionModel(
		nullptr, true, {"Family", "@Family"});
	model.vertical_ui_mode = VerticalFontUiMode::SystemList;

	EXPECT_EQ((std::vector<FontFamilyChoice>{
		{"Family", 0},
		{"@Family", 0},
	}), model.choices);
}

TEST(font_family_selection_model, compact_fallback_uses_live_gdi_vertical_capability) {
	auto model = BuildFontFamilySelectionModel(nullptr, true, {"Family"});
	model.vertical_ui_mode = VerticalFontUiMode::CompactToggle;
	FillVerticalCapability(model, {"@Family"});

	ASSERT_EQ(1u, model.choices.size());
	EXPECT_EQ("Family", model.choices.front().label);
	EXPECT_EQ(0u, model.choices.front().family_id);
	EXPECT_TRUE(model.SupportsVerticalWriting(0, "Family"));
}

TEST(font_family_selection_model, compact_list_commit_preserves_vertical_intent) {
	EXPECT_TRUE(UpdateVerticalWritingIntent(true, "Family", true));
	EXPECT_FALSE(UpdateVerticalWritingIntent(false, "@Family", true));
}

TEST(font_family_selection_model, manual_edit_updates_vertical_intent_from_prefix) {
	EXPECT_FALSE(UpdateVerticalWritingIntent(true, "Family", false));
	EXPECT_TRUE(UpdateVerticalWritingIntent(false, "@Family", false));
}

TEST(font_family_selection_model, supports_vertical_writing_by_family_id) {
	FontFamilySelectionModel model;
	model.vertical_capable_family_ids.insert(3);
	EXPECT_TRUE(model.SupportsVerticalWriting(3, "Anything"));
	EXPECT_FALSE(model.SupportsVerticalWriting(4, "Anything"));
	EXPECT_FALSE(model.SupportsVerticalWriting(0, "Anything"));
}

TEST(font_family_selection_model, supports_vertical_writing_by_bare_name) {
	FontFamilySelectionModel model;
	model.vertical_capable_bare_names.insert("MS Gothic");
	EXPECT_TRUE(model.SupportsVerticalWriting(0, "MS Gothic"));
	EXPECT_TRUE(model.SupportsVerticalWriting(0, "@MS Gothic"));
	EXPECT_FALSE(model.SupportsVerticalWriting(0, "Arial"));
	EXPECT_FALSE(model.SupportsVerticalWriting(0, "@"));
	EXPECT_FALSE(model.SupportsVerticalWriting(0, ""));
}

TEST(font_family_selection_model, supports_vertical_writing_via_catalog_resolve) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{family(5, "Local Name", "English Name")});
	FontFamilySelectionModel model;
	model.catalog = catalog;
	model.vertical_capable_family_ids.insert(5);
	EXPECT_TRUE(model.SupportsVerticalWriting(0, "English Name"));
	EXPECT_TRUE(model.SupportsVerticalWriting(0, "@Local Name"));
	EXPECT_FALSE(model.SupportsVerticalWriting(0, "Missing"));
}

TEST(font_family_selection_model, fill_vertical_capability_strips_at_and_resolves_ids) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{family(8, "Local V", "English V")});
	FontFamilySelectionModel model;
	model.catalog = catalog;
	FillVerticalCapability(model, {"@English V", "not-vertical", "@", "@English V"});
	EXPECT_TRUE(model.vertical_capable_bare_names.contains("English V"));
	EXPECT_TRUE(model.vertical_capable_family_ids.contains(8));
	EXPECT_FALSE(model.vertical_capable_bare_names.contains("not-vertical"));
}

TEST(font_family_selection_model, append_vertical_facename_choices_dedupes_and_sorts) {
	auto catalog = std::make_shared<FontFamilyCatalog>(
		std::vector<FontFamilyRecord>{
			family(1, "Local B", "English B"),
			family(2, "Local A", "English A")});
	auto model = BuildFontFamilySelectionModel(catalog, false);
	// Pre-existing @ row for B should not be duplicated.
	model.choices.push_back({"@English B", 1});
	AppendVerticalFacenameChoices(model, {"@English A", "@English B", "@Missing"});
	std::vector<FontFamilyChoice> at_rows;
	for (auto const& choice : model.choices) {
		if (!choice.label.empty() && choice.label.front() == '@')
			at_rows.push_back(choice);
	}
	ASSERT_EQ(2u, at_rows.size());
	EXPECT_EQ("@English A", at_rows[0].label);
	EXPECT_EQ(2u, at_rows[0].family_id);
	EXPECT_EQ("@English B", at_rows[1].label);
	EXPECT_EQ(1u, at_rows[1].family_id);
}
