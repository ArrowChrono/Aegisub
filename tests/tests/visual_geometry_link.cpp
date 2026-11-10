#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_style.h"
#include "../../src/visual_ass_edit.h"
#include "../../src/visual_geometry_link.h"

#include <array>
#include <initializer_list>

using namespace aegisub::visual_geometry;

namespace {
AssDialogue Dialogue(std::string text, int id = 1, int row = 0) {
	AssDialogue line;
	line.Id = id;
	line.Row = row;
	line.Text = std::move(text);
	return line;
}

PlanResult Apply(Snapshot const& source, std::initializer_list<Edit> edits, Vector2D layout = {640, 480}, bool restore = false) {
	Request const request{.source = &source, .edits = {edits.begin(), edits.size()}};
	return Plan(std::span(&request, 1), layout, restore);
}

Edit ScaleX(std::string value) {
	return {.tag = "\\fscx", .value = std::move(value)};
}
}

TEST(visual_geometry_link, affine_uses_source_pose_and_never_accumulates_preview) {
	AssStyle style;
	auto line = Dialogue(R"({\pos(300,200)\org(260,180)\frz90\fscx100\fscy100\clip(260,100,340,180)\bord3}Text)");
	Snapshot source(line, &style, {640, 480});
	auto result = Apply(source, {ScaleX("200"), {.tag = "\\fscy", .value = "50"}});
	ASSERT_TRUE(result) << result.error;
	ASSERT_EQ(1u, result.edits.size());
	EXPECT_EQ(line.Text.get(), result.edits[0].original_text);
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\clip(270,60,310,220))"));
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\pos(300,200)\org(260,180)\frz90)"));
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\bord3)"));
	auto repeated = Apply(source, {ScaleX("200"), {.tag = "\\fscy", .value = "50"}});
	ASSERT_TRUE(repeated) << repeated.error;
	ASSERT_EQ(1u, repeated.edits.size());
	EXPECT_EQ(result.edits[0].text, repeated.edits[0].text);
	auto original = Apply(source, {ScaleX("100"), {.tag = "\\fscy", .value = "100"}}, {640, 480}, true);
	ASSERT_TRUE(original);
	ASSERT_EQ(1u, original.edits.size());
	EXPECT_EQ(line.Text.get(), original.edits[0].text);
}

TEST(visual_geometry_link, layout_aspect_and_origin_change_follow_the_text_transform) {
	AssStyle style;
	auto line = Dialogue(R"({\pos(100,100)\org(100,100)\frz90\clip(0,0,20,20)}Text)");
	Snapshot source(line, &style, {640, 480});
	auto result = Apply(source, {{.tag = "\\org", .value = "(110,100)"}}, {1280, 480});
	ASSERT_TRUE(result) << result.error;
	ASSERT_EQ(1u, result.edits.size());
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\clip(10,20,30,40))"));
	auto plain = Dialogue(R"({\pos(100,100)\org(100,100)\clip(0,0,20,20)}Text)");
	Snapshot unrotated(plain, &style, {640, 480});
	auto no_clip_move = Apply(unrotated, {{.tag = "\\org", .value = "(110,100)"}});
	ASSERT_TRUE(no_clip_move) << no_clip_move.error;
	ASSERT_EQ(1u, no_clip_move.edits.size());
	EXPECT_NE(std::string::npos, no_clip_move.edits[0].text.find(R"(\clip(0,0,20,20))"));
}

TEST(visual_geometry_link, complete_move_translation_preserves_times_and_translates_origin_and_clip) {
	AssStyle style;
	auto line = Dialogue(R"({\move(20,30,60,70,10,100)\org(5,6)\clip(0,0,20,20)\frx30\b1}Text)");
	Snapshot source(line, &style, {640, 480});
	auto result = Apply(source, {{.tag = "\\move", .value = "(30,50,70,90,10,100)", .kind = EditKind::Translation, .translation = {10, 20}}});
	ASSERT_TRUE(result) << result.error;
	ASSERT_EQ(1u, result.edits.size());
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\move(30,50,70,90,10,100))"));
	EXPECT_NE(std::string::npos, result.edits[0].text.find(R"(\org(15,26)\clip(10,20,30,40)\frx30\b1)"));
}

TEST(visual_geometry_link, later_rejection_returns_no_partial_selected_edits) {
	AssStyle style;
	auto first = Dialogue(R"({\pos(20,30)\clip(0,0,20,20)}First)", 10, 2);
	auto second = Dialogue(R"({\pos(120,130)\clip(0,0,20,20)\frx20}Second)", 20, 9);
	Snapshot a(first, &style, {640, 480}), b(second, &style, {640, 480});
	std::array edits{ScaleX("200")};
	std::array requests{Request{.source = &a, .edits = edits}, Request{.source = &b, .edits = edits}};
	auto result = Plan(requests, {640, 480});
	EXPECT_FALSE(result);
	EXPECT_FALSE(result.error.empty());
	EXPECT_TRUE(result.edits.empty());
	EXPECT_EQ(first.Text.get(), a.Source().Text.get());
	EXPECT_EQ(second.Text.get(), b.Source().Text.get());
}

TEST(visual_geometry_link, endpoint_and_perspective_restrictions_require_the_relevant_attachment) {
	AssStyle style;
	for (auto const& attachment : {R"(\org(5,6))", R"(\clip(0,0,20,20))"}) {
		Snapshot source(Dialogue(std::string(R"({\move(20,30,60,70,10,100))") + attachment + "}Text"), &style, {640, 480});
		auto result = Apply(source, {{.tag = "\\move", .value = "(30,40,60,70,20,100)", .kind = EditKind::Endpoint}});
		EXPECT_FALSE(result);
		EXPECT_TRUE(result.edits.empty());
	}
	Snapshot bare_move(Dialogue(R"({\move(20,30,60,70,10,100)}Text)"), &style, {640, 480});
	auto endpoint = Apply(bare_move, {{.tag = "\\move", .value = "(30,40,60,70,20,100)", .kind = EditKind::Endpoint}});
	ASSERT_TRUE(endpoint);
	ASSERT_EQ(1u, endpoint.edits.size());
	EXPECT_EQ(R"({\move(30,40,60,70,20,100)}Text)", endpoint.edits[0].text);
	Snapshot clipped(Dialogue(R"({\pos(20,30)\clip(0,0,20,20)}Text)"), &style, {640, 480});
	EXPECT_FALSE(Apply(clipped, {{.tag = "\\frx", .value = "20", .kind = EditKind::Perspective}}));
	Snapshot origin_only(Dialogue(R"({\pos(20,30)\org(5,6)}Text)"), &style, {640, 480});
	auto perspective = Apply(origin_only, {{.tag = "\\frx", .value = "20", .kind = EditKind::Perspective}});
	ASSERT_TRUE(perspective);
	ASSERT_EQ(1u, perspective.edits.size());
	EXPECT_NE(std::string::npos, perspective.edits[0].text.find(R"(\frx20)"));
}

TEST(visual_geometry_link, static_pose_validation_has_only_the_translation_exemption) {
	AssStyle style;
	style.font = "@Arial";
	Snapshot vertical(Dialogue(R"({\pos(20,30)\clip(0,0,20,20)}Text)"), &style, {640, 480});
	EXPECT_FALSE(Apply(vertical, {ScaleX("200")}));
	auto translated = Apply(vertical, {{.tag = "\\pos", .value = "(30,50)", .kind = EditKind::Translation, .translation = {10, 20}}});
	ASSERT_TRUE(translated) << translated.error;
	ASSERT_EQ(1u, translated.edits.size());
	EXPECT_NE(std::string::npos, translated.edits[0].text.find(R"(\clip(10,20,30,40))"));
	Snapshot drawing(Dialogue(R"({\pos(20,30)\clip(0,0,20,20)\p1}m 0 0 l 20 0 20 20{\p0})"), &style, {640, 480});
	EXPECT_TRUE(Apply(drawing, {ScaleX("200")}));
	for (auto const& text : {
			 R"({\pos(20,30)\clip(0,0,20,20)\t(0,500,\frz30)}Text)",
			 R"({\pos(20,30)\move(20,30,40,50)\clip(0,0,20,20)}Text)",
			 R"({\pos(20)\clip(0,0,20,20)}Text)",
			 R"({\pos(20,30)\t(0,500, \clip(0,0,20,20))}Text)"}) {
		Snapshot invalid(Dialogue(text), &style, {640, 480});
		auto result = Apply(invalid, {{.tag = "\\pos", .value = "(30,50)", .kind = EditKind::Translation, .translation = {10, 20}}});
		EXPECT_FALSE(result);
		EXPECT_TRUE(result.edits.empty());
	}
}

TEST(visual_geometry_link, implicit_anchor_requires_integer_unambiguous_style_position) {
	AssStyle style;
	auto line = Dialogue(R"({\clip(0,0,20,20)\bord3}Text)");
	Snapshot integral(line, &style, {640, 480});
	auto result = Apply(integral, {ScaleX("200")});
	ASSERT_TRUE(result) << result.error;
	ASSERT_EQ(1u, result.edits.size());
	EXPECT_EQ(std::string::npos, result.edits[0].text.find("\\pos"));
	Snapshot fractional(line, &style, {641, 480});
	EXPECT_FALSE(Apply(fractional, {ScaleX("200")}, {641, 480}));
	Snapshot missing_style(line, nullptr, {640, 480});
	EXPECT_FALSE(Apply(missing_style, {ScaleX("200")}));
	Snapshot reset(Dialogue(R"({\rDefault\clip(0,0,20,20)}Text)"), &style, {640, 480});
	EXPECT_FALSE(Apply(reset, {ScaleX("200")}));
}

TEST(visual_geometry_link, nonpositive_scales_singular_shear_and_conflicting_edits_are_atomic_failures) {
	AssStyle style;
	for (auto const& scale : {"0", "-1"}) {
		Snapshot source(Dialogue(R"({\pos(20,30)\clip(0,0,20,20)}Text)"), &style, {640, 480});
		EXPECT_FALSE(Apply(source, {ScaleX(scale)}));
		Snapshot invalid(Dialogue(std::string(R"({\pos(20,30)\clip(0,0,20,20)\fscx)") + scale + "}Text"), &style, {640, 480});
		EXPECT_FALSE(Apply(invalid, {ScaleX("200")}));
	}
	Snapshot singular(Dialogue(R"({\pos(20,30)\clip(0,0,20,20)\fax1\fay1}Text)"), &style, {640, 480});
	EXPECT_FALSE(Apply(singular, {ScaleX("200")}));
	Snapshot source(Dialogue(R"({\pos(20,30)\org(5,6)\clip(0,0,20,20)}Text)"), &style, {640, 480});
	auto mixed = Apply(source, {{.tag = "\\pos", .value = "(30,50)", .kind = EditKind::Translation, .translation = {10, 20}},
								{.tag = "\\org", .value = "(16,26)"}});
	EXPECT_FALSE(mixed);
	EXPECT_TRUE(mixed.edits.empty());
}

TEST(visual_geometry_link, return_to_start_restores_unsupported_source_bytes_and_snapshot_owns_source) {
	AssStyle style;
	auto line = Dialogue(R"({\pos(20,30)\clip(0,0,20,20)\t(0,500,\frz30)}Text)", 7, 4);
	auto const original = line.Text.get();
	Snapshot source(line, &style, {640, 480});
	line.Text = "rebound";
	auto result = Apply(source, {}, {640, 480}, true);
	ASSERT_TRUE(result);
	ASSERT_EQ(1u, result.edits.size());
	EXPECT_EQ(7, result.edits[0].id);
	EXPECT_EQ(4, result.edits[0].row);
	EXPECT_EQ(original, result.edits[0].original_text);
	EXPECT_EQ(original, result.edits[0].text);
}
