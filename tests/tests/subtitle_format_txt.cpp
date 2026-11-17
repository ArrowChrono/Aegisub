#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/options.h"
#include "../../src/subtitle_format_txt.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/vfr.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr char kTxtOptionDefaults[] = R"({
	"Tool" : {
		"Import" : {
			"Text" : {
				"Actor Separator" : ":",
				"Comment Starter" : "#",
				"Include Blank" : false
			}
		}
	}
})";

class subtitle_format_txt : public ::testing::Test {
	agi::Options options{"", kTxtOptionDefaults, agi::Options::FLUSH_SKIP};
	agi::Options *previous_options = nullptr;
	agi::fs::path const path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-txt-actor-%%%%%%%%.txt");
	AssFile file;

	protected:
	void SetUp() override {
		previous_options = config::opt;
		config::opt = &options;
	}

	void TearDown() override {
		config::opt = previous_options;
		std::error_code error;
		std::filesystem::remove(path, error);
		EXPECT_FALSE(error) << error.message();
	}

	std::vector<AssDialogue const *> Read(std::string const& text, std::string const& separator,
										  std::string const& comment = "#") {
		options.Get("Tool/Import/Text/Actor Separator")->SetString(separator);
		options.Get("Tool/Import/Text/Comment Starter")->SetString(comment);
		{
			agi::io::Save output(path, true);
			output.Get() << text;
			output.Close();
		}

		TXTSubtitleFormat format;
		format.ReadFile(&file, path, agi::vfr::Framerate(), "utf-8", {}, {});

		std::vector<AssDialogue const *> events;
		for (auto const& event : file.Events)
			events.push_back(&event);
		return events;
	}
};

TEST_F(subtitle_format_txt, single_character_separator_preserves_actor_inheritance) {
	auto const events = Read("Alice : first: body\ncontinued\n \tBob: literal\n\tCarol: literal\nBob: second\nlast\n", ":");
	ASSERT_EQ(6u, events.size());
	EXPECT_EQ("Alice", events[0]->Actor.get());
	EXPECT_EQ("first: body", events[0]->Text.get());
	EXPECT_EQ("Alice", events[1]->Actor.get());
	EXPECT_EQ("continued", events[1]->Text.get());
	EXPECT_EQ("Alice", events[2]->Actor.get());
	EXPECT_EQ("Bob: literal", events[2]->Text.get());
	EXPECT_EQ("Alice", events[3]->Actor.get());
	EXPECT_EQ("Carol: literal", events[3]->Text.get());
	EXPECT_EQ("Bob", events[4]->Actor.get());
	EXPECT_EQ("second", events[4]->Text.get());
	EXPECT_EQ("Bob", events[5]->Actor.get());
	EXPECT_EQ("last", events[5]->Text.get());
	for (auto const *event : events)
		EXPECT_FALSE(event->Comment);
}

TEST_F(subtitle_format_txt, consumes_complete_multicharacter_separator) {
	auto const events = Read("Alice:: first::body\n", "::");
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ("Alice", events[0]->Actor.get());
	EXPECT_EQ("first::body", events[0]->Text.get());
	EXPECT_FALSE(events[0]->Comment);
}

TEST_F(subtitle_format_txt, consumes_complete_multibyte_separator_after_unicode_actor) {
	auto const events = Read("小明：你好：正文\n", "：");
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ("小明", events[0]->Actor.get());
	EXPECT_EQ("你好：正文", events[0]->Text.get());
	EXPECT_FALSE(events[0]->Comment);
}

TEST_F(subtitle_format_txt, consumes_multicharacter_multibyte_separator) {
	auto const events = Read("Alice：→ 正文：→保留\n", "：→");
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ("Alice", events[0]->Actor.get());
	EXPECT_EQ("正文：→保留", events[0]->Text.get());
	EXPECT_FALSE(events[0]->Comment);
}

TEST_F(subtitle_format_txt, missing_separator_preserves_text_and_previous_actor) {
	auto const events = Read("unmatched: text\nAlice:: first\nnot: a match\n \tBob:: literal\n", "::");
	ASSERT_EQ(4u, events.size());
	EXPECT_TRUE(events[0]->Actor.get().empty());
	EXPECT_EQ("unmatched: text", events[0]->Text.get());
	EXPECT_EQ("Alice", events[1]->Actor.get());
	EXPECT_EQ("first", events[1]->Text.get());
	EXPECT_EQ("Alice", events[2]->Actor.get());
	EXPECT_EQ("not: a match", events[2]->Text.get());
	EXPECT_EQ("Alice", events[3]->Actor.get());
	EXPECT_EQ("Bob:: literal", events[3]->Text.get());
}

TEST_F(subtitle_format_txt, multicharacter_comment_prefix_does_not_split_or_change_actor) {
	auto const events = Read("Alice:: spoken\n## Bob:: note\ncontinued\n", "::", "##");
	ASSERT_EQ(3u, events.size());
	EXPECT_TRUE(events[1]->Comment);
	EXPECT_TRUE(events[1]->Actor.get().empty());
	EXPECT_EQ("Bob:: note", events[1]->Text.get());
	EXPECT_FALSE(events[2]->Comment);
	EXPECT_EQ("Alice", events[2]->Actor.get());
	EXPECT_EQ("continued", events[2]->Text.get());
}

TEST_F(subtitle_format_txt, multibyte_comment_prefix_takes_precedence_over_actor_separator) {
	auto const events = Read("Alice※※ spoken\n※※ Bob※※ note\ncontinued\n", "※※", "※※");
	ASSERT_EQ(3u, events.size());
	EXPECT_TRUE(events[1]->Comment);
	EXPECT_TRUE(events[1]->Actor.get().empty());
	EXPECT_EQ("Bob※※ note", events[1]->Text.get());
	EXPECT_FALSE(events[2]->Comment);
	EXPECT_EQ("Alice", events[2]->Actor.get());
	EXPECT_EQ("continued", events[2]->Text.get());
}

TEST_F(subtitle_format_txt, empty_separator_disables_actor_splitting_but_not_comments) {
	auto const events = Read("Alice:: spoken\n## Bob:: note\ncontinued\n", "", "##");
	ASSERT_EQ(3u, events.size());
	EXPECT_TRUE(events[0]->Actor.get().empty());
	EXPECT_EQ("Alice:: spoken", events[0]->Text.get());
	EXPECT_FALSE(events[0]->Comment);
	EXPECT_TRUE(events[1]->Actor.get().empty());
	EXPECT_EQ("Bob:: note", events[1]->Text.get());
	EXPECT_TRUE(events[1]->Comment);
	EXPECT_TRUE(events[2]->Actor.get().empty());
	EXPECT_EQ("continued", events[2]->Text.get());
}

TEST_F(subtitle_format_txt, separator_at_end_preserves_actor_for_following_line) {
	auto const events = Read("Alice::\ncontinued\n", "::");
	ASSERT_EQ(2u, events.size());
	EXPECT_TRUE(events[0]->Comment);
	EXPECT_TRUE(events[0]->Actor.get().empty());
	EXPECT_TRUE(events[0]->Text.get().empty());
	EXPECT_FALSE(events[1]->Comment);
	EXPECT_EQ("Alice", events[1]->Actor.get());
	EXPECT_EQ("continued", events[1]->Text.get());
}

} // namespace
