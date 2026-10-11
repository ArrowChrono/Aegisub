#include <main.h>

#include "../../src/automation/local_script_references.h"

#include <libaegisub/path.h>

#include <array>
#include <filesystem>

using namespace Automation4;

TEST(LocalScriptReferences, preserves_reference_order_and_diagnostic_input) {
	auto references = ParseLocalScriptReferences(" | \t~first.lua |$ second.lua||!bad.lua|/third.lua|~first.lua|$\r\n");
	ASSERT_EQ(6u, references.size());
	auto const subtitle_directory = agi::fs::path("project/subtitles");
	std::string const automation_directory = "automation/scripts";
	auto first = ResolveLocalScriptReference(references[0], subtitle_directory, automation_directory);
	ASSERT_TRUE(first);
	EXPECT_EQ(subtitle_directory, first->base_directory);
	EXPECT_EQ(subtitle_directory / "first.lua", first->filename);
	auto second = ResolveLocalScriptReference(references[1], subtitle_directory, automation_directory);
	ASSERT_TRUE(second);
	EXPECT_EQ(agi::fs::path(automation_directory) / " second.lua", second->filename);
	EXPECT_EQ('!', references[2].location);
	EXPECT_EQ("bad.lua", references[2].filename);
	EXPECT_FALSE(ResolveLocalScriptReference(references[2], subtitle_directory, automation_directory));
	auto third = ResolveLocalScriptReference(references[3], subtitle_directory, automation_directory);
	ASSERT_TRUE(third);
	EXPECT_TRUE(third->base_directory.empty());
	EXPECT_EQ(agi::fs::path("third.lua"), third->filename);
	EXPECT_EQ('~', references[4].location);
	EXPECT_EQ("first.lua", references[4].filename);
	auto bare = ResolveLocalScriptReference(references[5], subtitle_directory, automation_directory);
	ASSERT_TRUE(bare);
	EXPECT_EQ(agi::fs::path(automation_directory) / "", bare->filename);
	EXPECT_TRUE(ParseLocalScriptReferences(" |\t||\r\n").empty());
}

TEST(LocalScriptReferences, serializes_shortest_paths_in_script_order) {
	agi::Path paths;
	auto const root = std::filesystem::current_path() / "artifacts/local-script-references";
	auto const automation_directory = root / "automation";
	auto const subtitle_directory = root / "subtitles";
	paths.SetToken("?script", subtitle_directory);
	std::array const filenames = {automation_directory / "alpha.lua", subtitle_directory / "beta.lua", agi::fs::path("tiny.lua")};
	EXPECT_EQ("$alpha.lua|~beta.lua|/tiny.lua", SerializeLocalScriptReferences(filenames, paths, automation_directory));
	EXPECT_EQ("", SerializeLocalScriptReferences({}, paths, automation_directory));
}

TEST(LocalScriptReferences, retains_tie_breaks_and_unset_base_behavior) {
	agi::Path paths;
	paths.SetToken("?script", "");
	std::array const filenames = {agi::fs::path("tiny.lua")};
	EXPECT_EQ("$tiny.lua", SerializeLocalScriptReferences(filenames, paths, {}));
	EXPECT_EQ("~tiny.lua", SerializeLocalScriptReferences(filenames, paths, "nested/base"));

	auto const root = std::filesystem::current_path() / "artifacts/local-script-references";
	paths.SetToken("?script", root / "subs");
	std::array const outside_base = {root / "shared/example.lua"};
	EXPECT_EQ("$../shared/example.lua", SerializeLocalScriptReferences(outside_base, paths, root / "auto"));
}
