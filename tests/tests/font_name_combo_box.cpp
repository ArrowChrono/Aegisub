#include <gtest/gtest.h>

#include "../../src/font_name_combo_box.h"

#ifdef __WXMSW__
#include <wx/app.h>
#include <wx/frame.h>
#include <wx/init.h>
#endif

namespace {

std::vector<std::pair<wxString, FontFamilyId>> SampleChoices() {
	return {
		{wxString::FromUTF8("Arial"), 1},
		{wxString::FromUTF8("Arial Black"), 2},
		{wxString::FromUTF8("Microsoft YaHei"), 3},
		{wxString::FromUTF8("Times New Roman"), 4},
	};
}

} // namespace

TEST(font_name_combo_box, auto_completed_suffix_is_removed_from_typed_query) {
	EXPECT_EQ(2u, font_name_combo_box_detail::TypedQueryLength(5, 2, 5, 2));
}

TEST(font_name_combo_box, committed_drop_down_choice_is_not_an_empty_query) {
	EXPECT_EQ(5u, font_name_combo_box_detail::TypedQueryLength(5, 0, 5, 0));
	EXPECT_EQ(5u, font_name_combo_box_detail::TypedQueryLength(5, 0, 0, 0));
}

TEST(font_name_combo_box, enter_commits_visible_list_caret_before_tracked_match) {
	EXPECT_EQ(3u, font_name_combo_box_detail::ResolveCommitSelection(4, 3u, 0u));
	EXPECT_EQ(2u, font_name_combo_box_detail::ResolveCommitSelection(
		4, std::nullopt, 2u));
	EXPECT_FALSE(font_name_combo_box_detail::ResolveCommitSelection(4, 4u, 5u));
}

TEST(font_name_combo_box, prefix_match_supports_auto_expanding_native_matching) {
	auto const choices = SampleChoices();
	auto const arial = font_name_combo_box_detail::FindPrefixMatch(
		choices, wxString::FromUTF8("ari"));
	ASSERT_TRUE(arial.has_value());
	EXPECT_EQ(0u, arial->index);
	EXPECT_EQ(1, arial->family_id);

	auto const times = font_name_combo_box_detail::FindPrefixMatch(
		choices, wxString::FromUTF8("TIMES"));
	ASSERT_TRUE(times.has_value());
	EXPECT_EQ(3u, times->index);
	EXPECT_EQ(4, times->family_id);

	EXPECT_FALSE(font_name_combo_box_detail::FindPrefixMatch(
		choices, wxString::FromUTF8("rial")).has_value());
	EXPECT_FALSE(font_name_combo_box_detail::FindPrefixMatch(
		choices, wxString()).has_value());
}

TEST(font_name_combo_box, contains_match_returns_first_list_index_without_filtering) {
	auto const choices = SampleChoices();
	auto const arial = font_name_combo_box_detail::FindContainsMatch(
		choices, wxString::FromUTF8("rial"));
	ASSERT_TRUE(arial.has_value());
	EXPECT_EQ(0u, arial->index);
	EXPECT_EQ(1, arial->family_id);

	auto const yahei = font_name_combo_box_detail::FindContainsMatch(
		choices, wxString::FromUTF8("yahei"));
	ASSERT_TRUE(yahei.has_value());
	EXPECT_EQ(2u, yahei->index);
	EXPECT_EQ(3, yahei->family_id);

	// First contains match wins; list order is unchanged (still full catalog).
	auto const ari = font_name_combo_box_detail::FindContainsMatch(
		choices, wxString::FromUTF8("ari"));
	ASSERT_TRUE(ari.has_value());
	EXPECT_EQ(0u, ari->index);

	EXPECT_FALSE(font_name_combo_box_detail::FindContainsMatch(
		choices, wxString::FromUTF8("zzz")).has_value());
	EXPECT_FALSE(font_name_combo_box_detail::FindContainsMatch(
		choices, wxString()).has_value());
}

TEST(font_name_combo_box, contains_match_works_for_enumerator_fallback_zero_ids) {
	std::vector<std::pair<wxString, FontFamilyId>> fallback = {
		{wxString::FromUTF8("Arial"), 0},
		{wxString::FromUTF8("Times New Roman"), 0},
	};
	auto const match = font_name_combo_box_detail::FindContainsMatch(
		fallback, wxString::FromUTF8("rial"));
	ASSERT_TRUE(match.has_value());
	EXPECT_EQ(0u, match->index);
	EXPECT_EQ(0, match->family_id);
}

#ifdef __WXMSW__
TEST(font_name_combo_box, replacing_choices_keeps_an_explicit_family_and_invalidates_deferred_matching) {
	auto const initialize = wxApp::GetInitializerFunction();
	wxApp::SetInitializerFunction([]() -> wxAppConsole * { return new wxApp; });
	auto restore_initializer = agi::make_scope_exit([&] { wxApp::SetInitializerFunction(initialize); });
	wxInitializer wx;
	ASSERT_TRUE(wx.IsOk());
	wxFrame parent(nullptr, wxID_ANY, wxS(""));
	FontNameComboBox combo(&parent, wxS("Shared"), wxDefaultSize,
						   {{.label = "Shared", .family_id = 1}, {.label = "Shared", .family_id = 2}, {.label = "Prior", .family_id = 3}}, true, true);
	int notifications = 0;
	combo.Bind(wxEVT_TEXT, [&](wxCommandEvent&) { ++notifications; });
	combo.Bind(wxEVT_COMBOBOX, [&](wxCommandEvent&) { ++notifications; });

	combo.SetExplicitSelection(wxS("Shared"), 2);
	combo.SetSelection(1, 3);
	combo.SetChoices({{.label = "Shared", .family_id = 1}});
	long start = 0, end = 0;
	combo.GetSelection(&start, &end);
	EXPECT_EQ(1, start);
	EXPECT_EQ(3, end);
	EXPECT_EQ(2u, combo.SelectedFamilyId());
	EXPECT_EQ(wxS("Shared"), combo.GetValue());
	EXPECT_EQ(0, notifications);

	combo.SetValue(wxS("Shared!"));
	EXPECT_FALSE(combo.SelectedFamilyId());
	combo.SetValue(wxS("Shared"));
	EXPECT_EQ(1u, combo.SelectedFamilyId());
	EXPECT_GT(notifications, 0);
	combo.SetChoices({{.label = "Shared", .family_id = 4}});
	combo.SetExplicitSelection(wxS("Chosen outside filter"), 2);
	wxTheApp->ProcessPendingEvents();
	EXPECT_EQ(wxS("Chosen outside filter"), combo.GetValue());
	EXPECT_EQ(2u, combo.SelectedFamilyId());
}
#endif

TEST(font_name_combo_box, contains_match_preserves_catalog_order) {
	std::vector<std::pair<wxString, FontFamilyId>> choices = {
		{wxString::FromUTF8("@Arial"), 10},
		{wxString::FromUTF8("Arial Black"), 11},
		{wxString::FromUTF8("Arial"), 12},
	};
	auto const match = font_name_combo_box_detail::FindContainsMatch(
		choices, wxString::FromUTF8("rial"));
	ASSERT_TRUE(match.has_value());
	EXPECT_EQ(0u, match->index);
	EXPECT_EQ(10, match->family_id);
}
