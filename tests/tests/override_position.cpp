#include <main.h>

#include "../../src/subtitle_edit_ops.h"

namespace edit_ops = aegisub::subtitle_edit_ops;

TEST(OverridePosition, PreservesAdjacentOverrideCaretAndProjectsCapturedOrdinal) {
	AssDialogue active;
	active.Text = "中😀{\\b1}{\\i1}éZ";
	AssDialogue other;
	other.Text = "{\\bord2}é中😀QéZ";

	auto const position = edit_ops::CaptureOverridePosition(active.Text, 15);
	EXPECT_EQ(15, position.original.raw);
	EXPECT_EQ(7, position.original.plain);
	EXPECT_EQ(2u, position.characters);

	auto const active_position = position.ForLine(active, true);
	EXPECT_EQ(15, active_position.raw);
	EXPECT_EQ(7, active_position.plain);
	auto const other_position = position.ForLine(other, false);
	EXPECT_EQ(13, other_position.raw);
	EXPECT_EQ(5, other_position.plain);
	EXPECT_EQ("中😀{\\b1}{\\i1}éZ", active.Text.get());
	EXPECT_EQ("{\\bord2}é中😀QéZ", other.Text.get());
}

TEST(OverridePosition, MapsStartAndEndAnchorsWithByteOffsetsAndShorterLineClamp) {
	AssDialogue active;
	active.Text = "{\\b1}中😀é";
	AssDialogue shorter;
	shorter.Text = "{\\i1}é中";
	AssDialogue longer;
	longer.Text = "{\\bord2}😀中éZ";

	auto const start = edit_ops::CaptureOverridePosition(active.Text, 0);
	auto const end = edit_ops::CaptureOverridePosition(active.Text, 14);
	EXPECT_EQ(0, start.original.raw);
	EXPECT_EQ(0, start.original.plain);
	EXPECT_EQ(0u, start.characters);
	EXPECT_EQ(14, end.original.raw);
	EXPECT_EQ(9, end.original.plain);
	EXPECT_EQ(3u, end.characters);

	auto const projected_start = start.ForLine(shorter, false);
	EXPECT_EQ(0, projected_start.raw);
	EXPECT_EQ(0, projected_start.plain);
	auto const clamped_end = end.ForLine(shorter, false);
	EXPECT_EQ(10, clamped_end.raw);
	EXPECT_EQ(5, clamped_end.plain);
	auto const projected_end = end.ForLine(longer, false);
	EXPECT_EQ(17, projected_end.raw);
	EXPECT_EQ(9, projected_end.plain);
	EXPECT_EQ("{\\b1}中😀é", active.Text.get());
	EXPECT_EQ("{\\i1}é中", shorter.Text.get());
	EXPECT_EQ("{\\bord2}😀中éZ", longer.Text.get());
}

TEST(OverridePosition, RetainsCaptureAfterActiveTextRewriteWithoutMutatingRows) {
	AssDialogue active;
	active.Text = "中{\\b1}{\\i1}😀é";
	AssDialogue other;
	other.Text = "{\\u1}😀é中QZ";

	auto const position = edit_ops::CaptureOverridePosition(active.Text, 11);
	active.Text = "x";
	auto const active_position = position.ForLine(active, true);
	EXPECT_EQ(11, active_position.raw);
	EXPECT_EQ(3, active_position.plain);
	EXPECT_EQ(11, position.original.raw);
	EXPECT_EQ(3, position.original.plain);
	EXPECT_EQ(1u, position.characters);
	auto const other_position = position.ForLine(other, false);
	EXPECT_EQ(9, other_position.raw);
	EXPECT_EQ(4, other_position.plain);
	EXPECT_EQ("x", active.Text.get());
	EXPECT_EQ("{\\u1}😀é中QZ", other.Text.get());
}
