#include "../../src/font_face_selection.h"

#include <gtest/gtest.h>

namespace {
using namespace aegisub::subtitle_edit_ops;

struct FontCase {
	AssDialogue line;
	AssStyle style;
	AssStyle alternate;
	int caret;

	explicit FontCase(std::string text) : caret(static_cast<int>(text.find('|'))) {
		text.erase(caret, 1);
		line.Text = std::move(text);
		style.scalex = 120.0;
		style.scaley = 95.0;
		style.spacing = 12.0;
		alternate.scalex = 145.0;
		alternate.scaley = 90.0;
		alternate.spacing = -1.25;
	}

	[[nodiscard]] AssStyle const *Reset(std::string_view name) const {
		return name == "Alt" ? &alternate : nullptr;
	}

	FontGeometry Read() {
		ParsedLine parsed(&line);
		return ReadFontGeometry(parsed, parsed.block_for_read(caret), style,
								[this](auto name) { return Reset(name); });
	}

	FontOverrideEdit Edit() {
		return CaptureFontOverrideEdit(ParsedLine(&line), NormalizePos(line.Text, caret), caret, style,
									   [this](auto name) { return Reset(name); });
	}

	FontOverrideResult Apply(std::initializer_list<std::pair<FontOverrideField, std::string>> changed) {
		FontOverrideValues values;
		for (auto const& [field, value] : changed)
			values[FontOverrideIndex(field)] = value;
		return Edit().Apply(line.Text.get(), values, caret, caret);
	}
};
using enum FontOverrideField;
} // namespace

TEST(font_face_selection, planned_writes_preserve_adjacent_blocks_resets_and_animation) {
	EXPECT_EQ(R"(A{\b0}{\fscy120\fscx110}B)",
			  FontCase(R"(A{\b0}{\fscy|80}B)").Apply({{ScaleX, "110"}, {ScaleY, "120"}}).text);
	EXPECT_EQ(R"({\fscx110\rAlt\fscx135}B)",
			  FontCase(R"({\fscx110\rAlt\fscx|150}B)").Apply({{ScaleX, "135"}}).text);
	EXPECT_EQ(R"({\fscx110\fsc\fscx135}B)",
			  FontCase(R"({\fscx110\fsc|}B)").Apply({{ScaleX, "135"}}).text);
	EXPECT_EQ(R"({\fscx110\t(0,1000,\fscx200)}B)",
			  FontCase(R"({\t(0,1000,\fscx200)|}B)").Apply({{ScaleX, "110"}}).text);
}

TEST(font_face_selection, last_effective_assignments_change_without_normalizing_other_bytes) {
	EXPECT_EQ(R"(lead {  \ b600 }A{\fscx80\r\fscx105\fscx125\t(0,100,\fscx200) \foo}x)",
			  FontCase(R"(lead {  \ b600 }A{\fscx80\r\fscx105\fscx1|15\t(0,100,\fscx200) \foo}x)")
				  .Apply({{ScaleX, "125"}})
				  .text);
	EXPECT_EQ(R"({\fscx80\t(0,100,\fscx200)\r\fscx130\t(100,200,\fscx220)}a)",
			  FontCase(R"({\fscx80\t(0,100,\fscx200)\r\t(100,200,\fscx220)|}a)")
				  .Apply({{ScaleX, "130"}})
				  .text);
	EXPECT_EQ(R"({\fscx110\t(0,100,\fscy200)\fsc\fscy125\t(100,200,\fscy220)}a)",
			  FontCase(R"({\fscx110\t(0,100,\fscy200)\fsc\t(100,200,\fscy220)|}a)")
				  .Apply({{ScaleY, "125"}})
				  .text);
	EXPECT_EQ(R"({\fscx85\t(\fscx200)\fscx125}a)",
			  FontCase(R"({\fscx85\t(\fscx200)\fscx110|}a)").Apply({{ScaleX, "125"}}).text);
}

TEST(font_face_selection, unchanged_fields_keep_original_bytes_across_heterogeneous_lines) {
	FontCase first(R"({\fs24\u0\b750\i1\fscx85.1234\fsp-|2.50}Aa)");
	FontCase second(R"({\fs48\u1\b0\i0\fscy115.4321\fsp|3}Bb)");
	EXPECT_EQ(R"({\fs24\u0\b750\i1\fscx85.1234\fsp5.1}Aa)", first.Apply({{Spacing, "5.1"}}).text);
	EXPECT_EQ(R"({\fs48\u1\b0\i0\fscy115.4321\fsp5.1}Bb)", second.Apply({{Spacing, "5.1"}}).text);
	EXPECT_EQ(R"({\fs24\u0\b750\i1\fscx85.1234\fsp-2.50}Aa)", first.Apply({}).text);
	EXPECT_EQ(R"({\fs48\u1\b0\i0\fscy130\fsp3\fscx110}Bb)",
			  second.Apply({{ScaleX, "110"}, {ScaleY, "130"}}).text);
	EXPECT_EQ(R"({\fs24\u0\b750\i1\fscx110\fsp-2.50\fnArial}Aa)",
			  first.Apply({{Face, "Arial"}, {ScaleX, "110"}}).text);
}

TEST(font_face_selection, static_values_use_style_resets_and_libass_numeric_prefixes) {
	auto const empty = FontCase(R"({\fscx\fsp|}B)").Read();
	EXPECT_EQ(120.0, empty.scale_x);
	EXPECT_EQ(12.0, empty.spacing);
	auto const invalid = FontCase(R"({\fscxabc\fscy-10\fspabc|}B)").Read();
	EXPECT_EQ(0.0, invalid.scale_x);
	EXPECT_EQ(0.0, invalid.scale_y);
	EXPECT_EQ(0.0, invalid.spacing);
	auto const suffix = FontCase(R"({\ fscx110.25tail\fsp-1.5px|}B)").Read();
	EXPECT_EQ(110.25, suffix.scale_x);
	EXPECT_EQ(-1.5, suffix.spacing);
	auto const named = FontCase(R"({\fscx110\rAlt\fscy160}{\fsc\fsp|}B)").Read();
	EXPECT_EQ(145.0, named.scale_x);
	EXPECT_EQ(90.0, named.scale_y);
	EXPECT_EQ(-1.25, named.spacing);
	for (auto const *reset : {R"({\r|}B)", R"({\rMissing|}B)"}) {
		auto const geometry = FontCase(R"({\rAlt\fsp-8})" + std::string(reset)).Read();
		EXPECT_EQ(120.0, geometry.scale_x);
		EXPECT_EQ(95.0, geometry.scale_y);
		EXPECT_EQ(12.0, geometry.spacing);
	}
}

TEST(font_face_selection, only_fields_inheriting_an_unshadowed_animation_are_blocked) {
	FontCase inherited(R"({\t(0,1000,\fscx200)}A{\b1|}B)");
	EXPECT_TRUE(inherited.Edit().InheritsAnimation(ScaleX));
	EXPECT_FALSE(inherited.Edit().InheritsAnimation(ScaleY));
	EXPECT_FALSE(inherited.Edit().InheritsAnimation(Spacing));
	EXPECT_EQ(R"({\t(0,1000,\fscx200)}A{\b1\fscy120}B)",
			  inherited.Apply({{ScaleY, "120"}}).text);
	EXPECT_TRUE(FontCase(R"({\t(\fscx200)}AB|C)").Edit().InheritsAnimation(ScaleX));
	EXPECT_FALSE(FontCase(R"({\t(\fscx200)}A{\rAlt|}B)").Edit().InheritsAnimation(ScaleX));
	EXPECT_FALSE(FontCase(R"({\t(\fscx200)}A{\fsc|}B)").Edit().InheritsAnimation(ScaleX));
	EXPECT_FALSE(FontCase(R"({\t(\fscx200)}A{\fscx140|}B)").Edit().InheritsAnimation(ScaleX));
	EXPECT_FALSE(FontCase(R"({\t(\fscx200)\fscx140}A{\b1|}B)").Edit().InheritsAnimation(ScaleX));
}

TEST(font_face_selection, one_new_override_block_and_independent_selection_endpoints) {
	FontCase plain(R"(Aé|BC)");
	FontOverrideValues values;
	values[FontOverrideIndex(Face)] = "Arial";
	values[FontOverrideIndex(ScaleX)] = "125";
	values[FontOverrideIndex(ScaleY)] = "115";
	auto const result = plain.Edit().Apply(plain.line.Text.get(), values, plain.caret, plain.caret + 1);
	std::string const inserted = R"({\fnArial\fscx125\fscy115})";
	EXPECT_EQ("Aé" + inserted + "BC", result.text);
	EXPECT_EQ(plain.caret + inserted.size(), result.selection_start);
	EXPECT_EQ(result.selection_start + 1, result.selection_end);

	FontCase adjacent(R"(A{\b1}{\fscy8|0}B)");
	FontOverrideValues scale;
	scale[FontOverrideIndex(ScaleY)] = "105";
	scale[FontOverrideIndex(ScaleX)] = "110";
	auto const ranged = adjacent.Edit().Apply(adjacent.line.Text.get(), scale, adjacent.caret, adjacent.caret + 3);
	EXPECT_EQ(adjacent.caret, ranged.selection_start);
	EXPECT_EQ(ranged.text.size(), ranged.selection_end);
}
