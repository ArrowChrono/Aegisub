#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/visual_tool_shear_policy.h"

#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
struct AdmissionCase {
	char const *name;
	std::string text;
	bool expected;
};

void ExpectClassification(std::initializer_list<AdmissionCase> cases) {
	for (auto const& test : cases) {
		SCOPED_TRACE(test.name);
		AssDialogue dialogue;
		dialogue.Text = test.text;
		EXPECT_EQ(test.expected, aegisub::visual_shear::IsStatic(dialogue));
		EXPECT_EQ(test.text, dialogue.Text.get());

		AssDialogue parsed_dialogue;
		parsed_dialogue.Text = test.text;
		auto const blocks = parsed_dialogue.ParseTags();
		EXPECT_EQ(test.expected, aegisub::visual_shear::IsStatic(std::span<std::unique_ptr<AssDialogueBlock> const>{blocks}));
		EXPECT_EQ(test.text, parsed_dialogue.Text.get());
	}
}

std::string NestedTransform(unsigned count, std::string const& innermost) {
	std::string text = "{";
	for (unsigned i = 0; i < count; ++i)
		text += R"(\t()";
	text += innermost;
	text.append(count, ')');
	text += "}text";
	return text;
}
}

TEST(visual_tool_shear_policy, accepts_missing_shear_and_independent_initial_axes) {
	ExpectClassification({
		{.name = "EmptyLine", .text = "", .expected = true},
		{.name = "PlainText", .text = R"(plain \fax0.5 text\Nsecond line)", .expected = true},
		{.name = "CommentOnly", .text = "{fax 0.5}", .expected = true},
		{.name = "UnrelatedTagsAndDrawing", .text = R"({\an7\pos(24,36)\p1}m 0 0 l 20 0 20 10{\p0}text{\i1}tail)", .expected = true},
		{.name = "InitialX", .text = R"({\fax0.25}text)", .expected = true},
		{.name = "InitialY", .text = R"({\fay-0.125}text)", .expected = true},
		{.name = "InitialBothAxes", .text = R"({\fay-0.125\fax0.25\move(24,36,48,72,10,500)\org(20,30)}text)", .expected = true},
		{.name = "ResetBeforeAxes", .text = R"({\rAlternate\fax0.25\fay-0.125}text{\bord2}tail)", .expected = true},
		{.name = "RepeatedResetsBeforeAxes", .text = R"({\rAlternate\rDefault\fax0.25}text)", .expected = true},
	});
}

TEST(visual_tool_shear_policy, requires_initial_parsed_block_not_first_override) {
	ExpectClassification({
		{.name = "PlainBeforeFirstOverride", .text = R"(prefix{\fax0.25}text)", .expected = false},
		{.name = "CommentBeforeFirstOverride", .text = R"({comment}{\fay0.25}text)", .expected = false},
		{.name = "EmptyOverrideBeforeShear", .text = R"({}{\fax0.25}text)", .expected = false},
		{.name = "AdjacentOverrides", .text = R"({\bord2}{\fay0.25}text)", .expected = false},
		{.name = "InlineShearAfterDrawing", .text = R"({\p1}m 0 0 l 20 10{\p0\fax0.25}text)", .expected = false},
		{.name = "InlineResetWithoutShear", .text = R"({\bord2}text{\rAlternate}tail)", .expected = false},
		{.name = "InlineUnrelatedTagsAfterShear", .text = R"({\fax0.25\fay-0.5}text{\i1\bord3}tail)", .expected = true},
	});
}

TEST(visual_tool_shear_policy, rejects_duplicate_axes_and_reset_after_shear) {
	ExpectClassification({
		{.name = "DuplicateXSameValue", .text = R"({\fax0.25\fax0.25}text)", .expected = false},
		{.name = "DuplicateYDifferentValue", .text = R"({\fay0.25\fay-0.5}text)", .expected = false},
		{.name = "SeparatedDuplicateX", .text = R"({\fax0.25\bord2\fay0.5\fax0.75}text)", .expected = false},
		{.name = "ResetAfterX", .text = R"({\fax0.25\r}text)", .expected = false},
		{.name = "ResetAfterY", .text = R"({\fay0.25\rAlternate}text)", .expected = false},
		{.name = "ResetBetweenAxes", .text = R"({\fax0.25\rAlternate\fay0.5}text)", .expected = false},
		{.name = "ResetPrefixAfterShear", .text = R"({\fax0.25\reset}text)", .expected = false},
		{.name = "ResetPrefixBeforeShear", .text = R"({\reset\fax0.25}text)", .expected = true},
	});
}

TEST(visual_tool_shear_policy, preserves_raw_spelling_and_parenthesis_grammar) {
	ExpectClassification({
		{.name = "SpaceAfterSlash", .text = R"({\ fax0.25}text)", .expected = false},
		{.name = "TabAfterSlash", .text = "{\\\tfay0.25}text", .expected = false},
		{.name = "SpaceAfterName", .text = R"({\fax +0.25\fay -0.5}text)", .expected = true},
		{.name = "ParenthesizedX", .text = R"({\fax(0.25)}text)", .expected = false},
		{.name = "ParenthesizedY", .text = R"({\fay(0.25)}text)", .expected = false},
		{.name = "UnclosedParenthesizedShear", .text = R"({\fax(0.25}text)", .expected = false},
		{.name = "UppercaseAxesAreUnrecognized", .text = R"({\FAX0.25\FAY0.5\FAX1}text)", .expected = true},
		{.name = "UppercaseDoesNotDuplicateLowercase", .text = R"({\fax0.25\FAX0.5}text)", .expected = true},
		{.name = "UppercaseResetIsUnrecognized", .text = R"({\fax0.25\RAlternate}text)", .expected = true},
		{.name = "LowercaseShearPrefix", .text = R"({\faxsuffix}text)", .expected = true},
		{.name = "DuplicateLowercaseShearPrefix", .text = R"({\faxsuffix\fax0.5}text)", .expected = false},
	});
}

TEST(visual_tool_shear_policy, uses_ass_numeric_prefix_and_float_range) {
	ExpectClassification({
		{.name = "SignedExponentAndNumericSuffix", .text = R"({\fax+2.5e-1suffix\fay-.125tail}text)", .expected = true},
		{.name = "MissingNumericSubject", .text = R"({\fax\fay}text)", .expected = true},
		{.name = "InvalidNumericSubjectIsZero", .text = R"({\faxinvalid\fay0x10}text)", .expected = true},
		{.name = "NanSubjectIsZero", .text = R"({\faxnan\fay-nan}text)", .expected = true},
		{.name = "InfinitySubjectIsZero", .text = R"({\faxinf\fay+infinity}text)", .expected = true},
		{.name = "LargestFiniteFloat", .text = R"({\fax3.4028234663852886e38\fay-3.4028234663852886e38}text)", .expected = true},
		{.name = "PositiveFloatOverflow", .text = R"({\fax3.402824e38}text)", .expected = false},
		{.name = "NegativeFloatOverflow", .text = R"({\fay-3.402824e38}text)", .expected = false},
		{.name = "DoubleOverflowSubjectIsZero", .text = R"({\fax1e400\fay-1e400}text)", .expected = true},
		{.name = "DoubleUnderflowSubjectIsZero", .text = R"({\fax1e-400\fay-1e-400}text)", .expected = true},
	});
}

TEST(visual_tool_shear_policy, rejects_animated_shear_with_libass_parenthesis_boundaries) {
	ExpectClassification({
		{.name = "AnimatedX", .text = R"({\t(0,500,\fax0.25)}text)", .expected = false},
		{.name = "AnimatedY", .text = R"({\fax0.25\t(0,500,2,\fay0.5)}text)", .expected = false},
		{.name = "NestedAnimatedX", .text = R"({\t(0,500,\t(1,\fax0.25))}text)", .expected = false},
		{.name = "WhitespaceAnimatedShear", .text = R"({\t(0,500,\ fay0.25)}text)", .expected = false},
		{.name = "InlineAnimatedShear", .text = R"({\bord2}text{\t(\fax0.25)}tail)", .expected = false},
		{.name = "UnrelatedAnimationWithStaticShear", .text = R"({\fax0.25\t(0,500,\frz30)\fay-0.5}text)", .expected = true},
		{.name = "UnclosedTransformContainsShear", .text = R"({\t(0,500,\fax0.25}text)", .expected = false},
		{.name = "TransformWithoutParentheses", .text = R"({\t\fax0.25}text)", .expected = true},
		{.name = "UppercaseTransformIsUnrecognized", .text = R"({\T(\fax0.25)}text)", .expected = true},
		{.name = "UppercaseAnimatedShearIsUnrecognized", .text = R"({\t(\FAX0.25)}text)", .expected = true},
		{.name = "FirstClosingParenEndsTransform", .text = R"({\t(\pos(1,2)\fax0.25)}text)", .expected = true},
	});
}

TEST(visual_tool_shear_policy, caps_nested_transform_depth_even_without_shear) {
	ExpectClassification({
		{.name = "ThirtyOneTransforms", .text = NestedTransform(31, R"(\frz30)"), .expected = true},
		{.name = "ThirtyTwoTransforms", .text = NestedTransform(32, R"(\frz30)"), .expected = true},
		{.name = "ThirtyThreeTransforms", .text = NestedTransform(33, R"(\frz30)"), .expected = false},
		{.name = "ShearAtDepthThirtyTwo", .text = NestedTransform(32, R"(\fax0.25)"), .expected = false},
	});
}

TEST(visual_tool_shear_policy, leaves_dialogue_and_borrowed_blocks_unchanged) {
	AdmissionCase const cases[] = {
		{.name = "AcceptedRawBytes", .text = R"({\rAlternate \fax +0.2500\fay -0.1250\bord2 \clip(1,m 0 0 l 40 0 40 20)}Text\N{\i1 }tail{comment})", .expected = true},
		{.name = "RejectedRawBytes", .text = R"({\fax +0.2500\fay -0.1250 \t(0,500,\fay0.75)}Text\N{\rAlternate}tail{comment})", .expected = false},
	};
	for (auto const& test : cases) {
		SCOPED_TRACE(test.name);
		AssDialogue dialogue;
		dialogue.Text = test.text;
		dialogue.Actor = "speaker";
		dialogue.Effect = "effect";
		dialogue.Style = "Alternate";
		dialogue.Margin = {12, 34, 56};
		dialogue.Comment = true;
		auto const entry = dialogue.GetEntryData();
		auto const blocks = dialogue.ParseTags();
		std::vector<std::string> raw_blocks;
		std::vector<AssDialogueBlock const *> identities;
		std::vector<AssBlockType> types;
		for (auto const& block : blocks) {
			raw_blocks.push_back(block->GetRawText());
			identities.push_back(block.get());
			types.push_back(block->GetType());
		}

		EXPECT_EQ(test.expected, aegisub::visual_shear::IsStatic(std::span<std::unique_ptr<AssDialogueBlock> const>{blocks}));
		ASSERT_EQ(raw_blocks.size(), blocks.size());
		for (std::size_t i = 0; i < blocks.size(); ++i) {
			EXPECT_EQ(raw_blocks[i], blocks[i]->GetRawText()) << "block " << i;
			EXPECT_EQ(identities[i], blocks[i].get()) << "block " << i;
			EXPECT_EQ(types[i], blocks[i]->GetType()) << "block " << i;
		}
		EXPECT_EQ(entry, dialogue.GetEntryData());
		EXPECT_EQ(test.expected, aegisub::visual_shear::IsStatic(dialogue));
		EXPECT_EQ(entry, dialogue.GetEntryData());
	}
}

TEST(visual_tool_shear_policy, accepts_empty_borrowed_block_span) {
	EXPECT_TRUE(aegisub::visual_shear::IsStatic(std::span<std::unique_ptr<AssDialogueBlock> const>{}));
}
