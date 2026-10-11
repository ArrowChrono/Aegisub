#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/auto4_lua.h"
#include "../../src/subtitle_grid_folding.h"

#include <lua.hpp>

#include <memory>
#include <vector>

namespace {
class lua_assfile : public ::testing::Test {
	protected:
	AssFile file;
	std::unique_ptr<lua_State, decltype(&lua_close)> state{luaL_newstate(), lua_close};
	std::shared_ptr<Automation4::LuaAssFile> subtitles;

	void SetUp() override {
		ASSERT_NE(nullptr, state);
		luaL_openlibs(state.get());
		lua_newtable(state.get());
		lua_setglobal(state.get(), "aegisub");
		for (int row = 0; row < 4; ++row) {
			auto *line = new AssDialogue;
			line->Text = "line " + std::to_string(row);
			file.Events.push_back(*line);
		}
		file.Commit("initial lines", AssFile::COMMIT_DIAG_ADDREM);
	}

	void BeginScript() {
		subtitles = Automation4::LuaAssFile::Create(state.get(), &file, true, true);
		lua_setglobal(state.get(), "subs");
	}
};
}

TEST_F(lua_assfile, replacing_fold_boundaries_preserves_identity_and_group) {
	auto& folding = file.Folding();
	ASSERT_TRUE(folding.Create(file, 0, 2));
	file.Commit("fold lines", AssFile::COMMIT_FOLD);
	auto const group_id = folding.Groups().front().id;
	std::vector<int> original_ids;
	for (auto const& line : file.Events)
		original_ids.push_back(line.Id);

	BeginScript();
	ASSERT_EQ(0, luaL_dostring(state.get(), R"(
		for _, index in ipairs({1, 3}) do
			local line = subs[index]
			line.text = "edited boundary " .. index
			subs[index] = line
		end
	)")) << lua_tostring(state.get(), -1);
	subtitles->ProcessingComplete("edit boundaries");

	std::vector<AssDialogue const *> lines;
	for (auto const& line : file.Events)
		lines.push_back(&line);
	ASSERT_EQ(4u, lines.size());
	for (size_t row = 0; row < lines.size(); ++row)
		EXPECT_EQ(original_ids[row], lines[row]->Id);
	EXPECT_EQ("edited boundary 1", lines[0]->Text.get());
	EXPECT_EQ("edited boundary 3", lines[2]->Text.get());
	ASSERT_EQ(1u, folding.Groups().size());
	EXPECT_EQ(group_id, folding.Groups().front().id);
	EXPECT_EQ(0, folding.Groups().front().start);
	EXPECT_EQ(2, folding.Groups().front().end);
	EXPECT_TRUE(folding.IsCollapsed(folding.Groups().front()));
	EXPECT_EQ(-1, folding.SourceToDisplay(2));
	EXPECT_EQ(1, folding.SourceToDisplay(3));
}

TEST_F(lua_assfile, appending_copy_keeps_distinct_identity_after_indexed_update) {
	auto const original_id = file.Events.front().Id;
	BeginScript();
	ASSERT_EQ(0, luaL_dostring(state.get(), R"(
		local line = subs[1]
		line.text = "edited original"
		subs[1] = line
		subs.append(line)
	)")) << lua_tostring(state.get(), -1);
	subtitles->ProcessingComplete("edit and copy");

	EXPECT_EQ(original_id, file.Events.front().Id);
	EXPECT_NE(original_id, file.Events.back().Id);
	EXPECT_EQ("edited original", file.Events.front().Text.get());
	EXPECT_EQ("edited original", file.Events.back().Text.get());
}
