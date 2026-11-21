#include "../../src/font_quick_lists.h"

#include <libaegisub/fs.h>
#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <utility>

namespace {
using namespace font_quick_lists;

FontFamilySelectionModel CollidingCatalog(bool localized, FontFamilyId first, FontFamilyId second) {
	auto catalog = std::make_shared<FontFamilyCatalog const>(std::vector<FontFamilyRecord>{
		{.id = first, .localized_family_name = "Localized A", .english_win32_family_name = "Shared Alias", .names = {{.value = "Alias A", .locale = "en-US"}}},
		{.id = second, .localized_family_name = "Localized B", .english_win32_family_name = "Shared Alias", .names = {{.value = "Alias B", .locale = "en-US"}}},
	});
	return BuildFontFamilySelectionModel(catalog, localized);
}
}

TEST(font_quick_lists, favorites_keep_the_explicit_family_across_catalog_rebuild_and_language_change) {
	auto english = CollidingCatalog(false, 10, 11);
	ASSERT_EQ((std::vector<FontFamilyChoice>{{"Shared Alias", 10}, {"Shared Alias", 11}}), english.choices);
	auto const second = FavoriteIdentity(english, "Shared Alias", 11);
	EXPECT_EQ((FontFamilyChoice{"Localized B", 11}), second);
	Names names = {""};
	ToggleFavorite(english, names, second);
	EXPECT_EQ((Names{"Localized B"}), names);
	EXPECT_FALSE(IsFavorite(english, names, FavoriteIdentity(english, "Shared Alias", 10)));
	EXPECT_EQ((std::vector{second}), FavoriteChoices(english, names));

	auto localized = CollidingCatalog(true, 21, 20);
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"Localized B", 20}}), FavoriteRows(localized, names));
	names.insert(names.end(), {"Alias B", "@LOCALIZED B", "Alias A"});
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"Localized B", 20}, {"Localized A", 21}}), FavoriteRows(localized, names));
	RemoveFavorite(localized, names, FavoriteIdentity(localized, "Alias B"));
	EXPECT_EQ((Names{"Alias A"}), names);
}

TEST(font_quick_lists, ambiguous_names_without_a_unique_alias_are_single_literal_favorites) {
	auto catalog = std::make_shared<FontFamilyCatalog const>(std::vector<FontFamilyRecord>{
		{.id = 10, .localized_family_name = "Shared Alias", .english_win32_family_name = "Shared Alias"},
		{.id = 11, .localized_family_name = "Shared Alias", .english_win32_family_name = "Shared Alias"},
	});
	auto model = BuildFontFamilySelectionModel(catalog, false);
	auto const identity = FavoriteIdentity(model, "@Shared Alias", 11);
	EXPECT_EQ((FontFamilyChoice{"Shared Alias", 0}), identity);
	Names names;
	ToggleFavorite(model, names, identity);
	EXPECT_EQ((Names{"Shared Alias"}), names);
	EXPECT_EQ((std::vector{identity}), FavoriteChoices(model, names));
	ToggleFavorite(model, names, FavoriteIdentity(model, "shared alias", 11));
	EXPECT_TRUE(names.empty());
}

TEST(font_quick_lists, unavailable_catalog_keeps_enumerated_vertical_faces_and_unknown_favorites) {
	auto model = BuildFontFamilySelectionModel(nullptr, false, {"@Foo", "Bar", "Foo", "FOO", "@FOO"});
	Names names = {"", " \t", "@foo", "Not Installed", "NOT installed"};
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"foo", 0}, {"Not Installed", 0}}), FavoriteRows(model, names));
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"@Foo", 0}, {"Foo", 0}, {"Not Installed", 0}}), FavoriteChoices(model, names));
	model.vertical_ui_mode = VerticalFontUiMode::CompactToggle;
	EXPECT_EQ((std::vector<FontFamilyChoice>{{"Foo", 0}, {"Not Installed", 0}}), FavoriteChoices(model, names));
	ToggleFavorite(model, names, FavoriteIdentity(model, "Foo"));
	EXPECT_EQ((Names{"Not Installed", "NOT installed"}), names);
}

TEST(font_quick_lists, recent_deduplicates_unicode_case_without_rewriting_exact_ass_names) {
	Names names = {"", "\t", "@fOo", "Foo", "Étoile", " Custom Face "};
	EXPECT_EQ((Names{"éTOILE", "@fOo", "Foo", " Custom Face "}), RecordRecent(names, "éTOILE", 16));
	EXPECT_EQ((Names{"fOo", "@fOo"}), RecordRecent(names, "fOo", 2));
	auto recent = TrimRecent(names, 16);
	RemoveRecent(recent, "FOO");
	EXPECT_EQ((Names{"@fOo", "Étoile", " Custom Face "}), recent);
	EXPECT_TRUE(RecordRecent(recent, "New face", 0).empty());
}

TEST(font_quick_lists, reduced_and_paused_recent_limits_survive_a_configuration_restart) {
	const char path[] = "data/options/recent-font-limit.json";
	const char defaults[] = R"({"Subtitle":{"Font":{"Recent":[{"string":""}],"Recent Limit":16}}})";
	{
		agi::Options options(path, defaults, agi::Options::FLUSH_SKIP);
		Names names;
		for (int i = 1; i <= 17; ++i)
			names = RecordRecent(names, "Font " + std::to_string(i), options.Get(RecentLimitOption)->GetInt());
		ASSERT_EQ(16, names.size());
		EXPECT_EQ("Font 2", names.back());
		options.Get(RecentOption)->SetListString(std::move(names));
		options.Flush();
	}
	for (auto const& [limit, expected] : std::array<std::pair<int, Names>, 2>{
			 std::pair{2, Names{"Font 17", "Font 16"}}, std::pair{0, Names{}}}) {
		{
			agi::Options options(path, defaults, agi::Options::FLUSH_SKIP);
			options.ConfigUser();
			options.Get(RecentLimitOption)->SetInt(limit);
			options.Get(RecentOption)->SetListString(TrimRecent(options.Get(RecentOption)->GetListString(), limit));
			options.Flush();
		}
		agi::Options restarted(path, defaults, agi::Options::FLUSH_SKIP);
		restarted.ConfigUser();
		EXPECT_EQ(limit, restarted.Get(RecentLimitOption)->GetInt());
		EXPECT_EQ(expected, restarted.Get(RecentOption)->GetListString());
	}
	agi::fs::Remove(path);
}
