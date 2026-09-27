#include "../../../src/ass_dialogue.h"
#include "../../../src/ass_file.h"
#include "../../../src/ass_io_core.h"
#include "../../../src/automation/karaoke_line_classifier.h"
#include "../../../src/automation/lua_source_tools.h"

#include <libaegisub/vfr.h>

#include <lua.hpp>
#include <luajit.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

thread_local int hook_budget = 0;

void InstructionHook(lua_State *state, lua_Debug *) {
	if (--hook_budget == 0)
		luaL_error(state, "Lua instruction budget exceeded");
}

fs::path RelativePath(char const *argument) {
	fs::path path(argument);
	if (path.empty() || path.is_absolute() || path.has_root_name())
		throw std::runtime_error("A nonempty repository-relative path is required");
	for (auto const& component : path) {
		if (component == "..")
			throw std::runtime_error("Parent traversal is not allowed in a path argument");
	}
	return path;
}

fs::path ArtifactPath(char const *argument) {
	fs::path path(argument);
	if (path.empty())
		throw std::runtime_error("A nonempty artifact directory is required");
	return path;
}

std::string ReadFile(fs::path const& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("Cannot read " + path.generic_string());
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteFile(fs::path const& path, std::string_view content) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("Cannot write " + path.generic_string());
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	if (!file)
		throw std::runtime_error("Cannot finish writing " + path.generic_string());
}

std::string Execute(std::string_view source) {
	using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
	State state(luaL_newstate(), &lua_close);
	if (!state)
		throw std::runtime_error("Cannot create LuaJIT state");
	luaL_openlibs(state.get());
	if (!luaJIT_setmode(state.get(), 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF))
		throw std::runtime_error("Cannot disable LuaJIT compilation");
	hook_budget = 2000;
	lua_sethook(state.get(), InstructionHook, LUA_MASKCOUNT, 1000);
	if (luaL_loadbuffer(state.get(), source.data(), source.size(), "@source-roundtrip") != 0)
		throw std::runtime_error(lua_tostring(state.get(), -1));
	if (lua_pcall(state.get(), 0, 1, 0) != 0)
		throw std::runtime_error(lua_tostring(state.get(), -1));
	if (lua_isnil(state.get(), -1))
		return "<no-return>";
	if (lua_type(state.get(), -1) != LUA_TSTRING)
		throw std::runtime_error("Fixture did not return a string");
	size_t length = 0;
	char const *data = lua_tolstring(state.get(), -1, &length);
	return std::string(data, length);
}

void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

void ExpectResult(std::string_view source, std::string_view expected, fs::path const& artifact) {
	auto actual = Execute(source);
	WriteFile(artifact, actual);
	Require(actual == expected, "Business result differs from the independent expected value at " + artifact.generic_string());
}

void CheckMemberAccessFormat(std::string_view source, std::string_view stage) {
	auto const method = source.find("object.branch:tag");
	Require(method != std::string_view::npos && source.find("object.branch:tag", method + 1) != std::string_view::npos,
			std::string(stage) + " separated a chained method definition or call");
	Require(source.find("object.branch.name") != std::string_view::npos, std::string(stage) + " separated chained member access");
	Require(source.find("self.name") != std::string_view::npos, std::string(stage) + " separated member access inside the method");
	Require(source.find("1 .. 2") != std::string_view::npos, std::string(stage) + " merged numeric concatenation into a number token");
	Require(source.find("n .. .5") != std::string_view::npos, std::string(stage) + " merged concatenation and leading-decimal number");
}

void CheckSemicolonFormat(std::string_view source, std::string_view stage) {
	for (auto const text : {"2;", "3;", "total + value;", "choose ( 7 );"})
		Require(source.find(text) != std::string_view::npos, std::string(stage) + " inserted space before a semicolon at " + text);
}

void RunExecutableCase(fs::path const& fixtures, fs::path const& artifacts, std::string const& name) {
	fs::create_directories(artifacts);
	auto const original = ReadFile(fixtures / (name + ".lua"));
	auto const expected = ReadFile(fixtures / (name + ".expected"));
	WriteFile(artifacts / "original.lua", original);
	WriteFile(artifacts / "result.expected", expected);
	Require(!Automation4::ValidateLuaSource(original), "Original source failed validation");
	ExpectResult(original, expected, artifacts / "original.actual");

	auto const formatted = Automation4::FormatLuaSource(original);
	Require(formatted.Succeeded(), "Formatting failed: " + (formatted.diagnostic ? formatted.diagnostic->message : std::string()));
	WriteFile(artifacts / "formatted.lua", formatted.source);
	Require(!Automation4::ValidateLuaSource(formatted.source), "Formatted source failed validation");
	if (name == "member-access") {
		CheckMemberAccessFormat(formatted.source, "Formatting");
		Require(formatted.source.find("[==[长串 . : .. -- ]=] intact]==]") != std::string::npos, "Formatting changed long-string token content");
		Require(formatted.source.find("--[=[Unicode Ω . : ... ]=]") != std::string::npos, "Formatting changed long-comment token content");
		Require(formatted.source.find("-- Ω . : .. ... - - 雪") != std::string::npos, "Formatting changed line-comment token content");
	}
	if (name == "semicolons") {
		CheckSemicolonFormat(formatted.source, "Formatting");
		for (auto const text : {"\"雪 ; 🌟\"", "[=[文 ; 本]=]", "-- punctuation ; stays inside this comment", "--[=[long comment ; punctuation]=]"})
			Require(formatted.source.find(text) != std::string::npos, "Formatting changed literal or comment semicolon spacing");
	}
	ExpectResult(formatted.source, expected, artifacts / "formatted.actual");

	auto const serialized = Automation4::SerializeLuaSource(formatted.source);
	Require(serialized.Succeeded(), "Serialization failed: " + (serialized.diagnostic ? serialized.diagnostic->message : std::string()));
	WriteFile(artifacts / "serialized.lua", serialized.source);
	Require(serialized.source.find_first_of("\r\n") == std::string::npos, "Serialized source contains a physical line break");
	Require(!Automation4::ValidateLuaSource(serialized.source), "Serialized source failed validation");
	if (name == "member-access") {
		CheckMemberAccessFormat(serialized.source, "Serialization");
		WriteFile(artifacts / "format-contract.txt", "Chained members and method calls stay adjacent; numeric concatenation remains separate.\n");
	}
	if (name == "semicolons") {
		CheckSemicolonFormat(serialized.source, "Serialization");
		WriteFile(artifacts / "format-contract.txt", "Statement and table separators have no preceding space; literals and comments retain their content.\n");
	}
	ExpectResult(serialized.source, expected, artifacts / "serialized.actual");

	AssFile file;
	file.LoadDefault();
	Require(!file.Events.empty(), "Default ASS has no dialogue event");
	auto& line = file.Events.front();
	line.Comment = true;
	line.Effect = "code once";
	line.Text = serialized.source;
	WriteAssFileForCore(&file, artifacts / "input.ass", agi::vfr::Framerate{}, "UTF-8", AssWriteOptions{});

	AssFile reopened;
	ReadAssFileForCore(&reopened, artifacts / "input.ass", "UTF-8");
	Require(reopened.Events.size() == 1, "Reopened ASS event count changed");
	auto const& restored = reopened.Events.front();
	Require(restored.Comment, "Reopened event lost its comment flag");
	Require(Automation4::IsKaraokeCodeLine(restored.Comment, restored.Effect.get()), "Reopened event is no longer a code line");
	Require(restored.Text.get() == serialized.source, "Reopened ASS source bytes differ from serialized source");
	WriteAssFileForCore(&reopened, artifacts / "output.ass", agi::vfr::Framerate{}, "UTF-8", AssWriteOptions{});
	ExpectResult(restored.Text.get(), expected, artifacts / "reopened.actual");
}

void RunValidateOnlyCase(fs::path const& fixtures, fs::path const& artifacts) {
	fs::create_directories(artifacts);
	auto const original = ReadFile(fixtures / "validate_only.lua");
	WriteFile(artifacts / "original.lua", original);
	Require(!Automation4::ValidateLuaSource(original), "Validation-only source failed validation");
	auto const formatted = Automation4::FormatLuaSource(original);
	Require(formatted.Succeeded(), "Validation-only source failed formatting");
	WriteFile(artifacts / "formatted.lua", formatted.source);
	auto const serialized = Automation4::SerializeLuaSource(formatted.source);
	Require(serialized.Succeeded(), "Validation-only source failed serialization");
	WriteFile(artifacts / "serialized.lua", serialized.source);
	Require(!Automation4::ValidateLuaSource(serialized.source), "Validation-only serialized source failed validation");
}

void RunInvalidCase(fs::path const& fixtures, fs::path const& artifacts, std::string const& name) {
	fs::create_directories(artifacts);
	auto const original = ReadFile(fixtures / (name + ".lua"));
	WriteFile(artifacts / "original.lua", original);
	auto const validation = Automation4::ValidateLuaSource(original);
	Require(validation.has_value(), "Invalid source passed validation");
	WriteFile(artifacts / "validation.diagnostic", validation->message);
	auto const formatted = Automation4::FormatLuaSource(original);
	Require(!formatted.Succeeded(), "Invalid source passed formatting");
	Require(formatted.source == original, "Failed formatting changed the original source");
	WriteFile(artifacts / "format.diagnostic", formatted.diagnostic->message);
	auto const serialized = Automation4::SerializeLuaSource(original);
	Require(!serialized.Succeeded(), "Invalid source passed serialization");
	Require(serialized.source == original, "Failed serialization changed the original source");
	WriteFile(artifacts / "serialize.diagnostic", serialized.diagnostic->message);
}

void RunLuaChunk(lua_State *state, std::string_view source, char const *name) {
	if (luaL_loadbuffer(state, source.data(), source.size(), name) != 0)
		throw std::runtime_error(lua_tostring(state, -1));
	if (lua_pcall(state, 0, 0, 0) != 0)
		throw std::runtime_error(lua_tostring(state, -1));
}

void CheckClassifier(fs::path const& artifacts) {
	using namespace Automation4;
	struct Case {
		bool comment;
		std::string_view effect;
		KaraokeLineKind kind;
		unsigned scopes;
		bool all_styles;
		bool no_blank;
		double repeat;
	};
	std::vector<Case> const cases = {
		{.comment = true, .effect = "code", .kind = KaraokeLineKind::Code, .scopes = KaraokeOnce, .repeat = 1},
		{.comment = true, .effect = "CODE line syl all noblank repeat 0x1.8p+2", .kind = KaraokeLineKind::Code, .scopes = KaraokeLine | KaraokeSyllable, .all_styles = true, .no_blank = true, .repeat = 6},
		{.comment = true, .effect = "code repeat nope furi loop 3 once", .kind = KaraokeLineKind::Code, .scopes = KaraokeFurigana | KaraokeOnce, .repeat = 3},
		{.comment = true, .effect = "code loop 2 repeat broken", .kind = KaraokeLineKind::Code, .scopes = KaraokeOnce, .repeat = 1},
		{.comment = true, .effect = "template line !x!", .kind = KaraokeLineKind::Template, .repeat = 1},
		{.comment = true, .effect = " code line", .kind = KaraokeLineKind::None, .repeat = 1},
		{.comment = false, .effect = "code line", .kind = KaraokeLineKind::None, .repeat = 1},
		{.comment = true, .effect = "codex line", .kind = KaraokeLineKind::None, .repeat = 1},
		{.comment = true, .effect = "code repeat nan", .kind = KaraokeLineKind::Code, .scopes = KaraokeOnce, .repeat = std::numeric_limits<double>::quiet_NaN()}};

	AssFile file;
	file.LoadDefault();
	for (size_t i = 0; i < cases.size(); ++i) {
		if (i)
			file.Events.push_back(*new AssDialogue);
		auto& line = file.Events.back();
		line.Comment = cases[i].comment;
		line.Effect = std::string(cases[i].effect);
		line.Text = "case-" + std::to_string(i + 1);
	}
	WriteAssFileForCore(&file, artifacts / "input.ass", agi::vfr::Framerate{}, "UTF-8", AssWriteOptions{});
	AssFile reopened;
	ReadAssFileForCore(&reopened, artifacts / "input.ass", "UTF-8");
	Require(reopened.Events.size() == cases.size(), "Classifier ASS event count changed");
	WriteAssFileForCore(&reopened, artifacts / "output.ass", agi::vfr::Framerate{}, "UTF-8", AssWriteOptions{});

	auto parse_with_templater = [&](AssFile const& source) {
		using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
		State state(luaL_newstate(), &lua_close);
		Require(static_cast<bool>(state), "Cannot create parser LuaJIT state");
		luaL_openlibs(state.get());
		Require(luaJIT_setmode(state.get(), 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) != 0, "Cannot disable parser JIT compilation");
		hook_budget = 20000;
		lua_sethook(state.get(), InstructionHook, LUA_MASKCOUNT, 1000);
		constexpr std::string_view bootstrap = R"lua(
aegisub = {
  gettext = function(s) return s end,
  register_macro = function() end,
  register_filter = function() end,
  progress = { set = function() end },
  debug = { out = function() end }
}
package.preload["aegisub.util"] = function()
  return { headtail = function(s)
    local a, b, head, tail = s:find('(.-)%s+(.*)')
    if a then return head, tail else return s, '' end
  end }
end
package.preload["aegisub.unicode"] = function() return {} end
include = function(name)
  local chunk = assert(loadfile('automation/include/' .. name))
  return chunk()
end
)lua";
		RunLuaChunk(state.get(), bootstrap, "@classifier-bootstrap");
		auto const parser_source = ReadFile("automation/autoload/kara-templater.lua");
		RunLuaChunk(state.get(), parser_source, "@kara-templater.lua");
		lua_getglobal(state.get(), "parse_templates");
		lua_newtable(state.get());
		lua_newtable(state.get());
		lua_newtable(state.get());
		int index = 1;
		for (auto const& line : source.Events) {
			lua_newtable(state.get());
			lua_pushliteral(state.get(), "dialogue");
			lua_setfield(state.get(), -2, "class");
			lua_pushboolean(state.get(), line.Comment);
			lua_setfield(state.get(), -2, "comment");
			lua_pushlstring(state.get(), line.Effect.get().data(), line.Effect.get().size());
			lua_setfield(state.get(), -2, "effect");
			lua_pushlstring(state.get(), line.Text.get().data(), line.Text.get().size());
			lua_setfield(state.get(), -2, "text");
			lua_pushstring(state.get(), line.Style.get().c_str());
			lua_setfield(state.get(), -2, "style");
			lua_pushinteger(state.get(), line.Layer);
			lua_setfield(state.get(), -2, "layer");
			lua_rawseti(state.get(), -2, index++);
		}
		if (lua_pcall(state.get(), 3, 1, 0) != 0)
			throw std::runtime_error(lua_tostring(state.get(), -1));
		int const templates_index = lua_gettop(state.get());
		std::vector<KaraokeLineClassification> parsed(cases.size());
		for (auto const& category : std::vector<std::pair<char const *, unsigned>>{{"once", KaraokeOnce}, {"line", KaraokeLine}, {"syl", KaraokeSyllable}, {"furi", KaraokeFurigana}}) {
			lua_getfield(state.get(), templates_index, category.first);
			int const category_index = lua_gettop(state.get());
			lua_pushnil(state.get());
			while (lua_next(state.get(), category_index)) {
				if (lua_istable(state.get(), -1)) {
					lua_getfield(state.get(), -1, "code");
					bool const code = lua_isstring(state.get(), -1);
					lua_pop(state.get(), 1);
					lua_getfield(state.get(), -1, code ? "code" : "t");
					std::string marker = lua_isstring(state.get(), -1) ? lua_tostring(state.get(), -1) : "";
					lua_pop(state.get(), 1);
					for (size_t i = 0; i < cases.size(); ++i) {
						if (marker != "case-" + std::to_string(i + 1))
							continue;
						auto& entry = parsed[i];
						entry.kind = code ? KaraokeLineKind::Code : KaraokeLineKind::Template;
						if (code) {
							entry.scopes |= category.second;
							lua_getfield(state.get(), -1, "style");
							entry.all_styles = lua_isnil(state.get(), -1);
							lua_pop(state.get(), 1);
							lua_getfield(state.get(), -1, "noblank");
							entry.no_blank = lua_toboolean(state.get(), -1);
							lua_pop(state.get(), 1);
							lua_getfield(state.get(), -1, "loops");
							entry.repeat = lua_tonumber(state.get(), -1);
							lua_pop(state.get(), 1);
						}
					}
				}
				lua_pop(state.get(), 1);
			}
			lua_pop(state.get(), 1);
		}
		return parsed;
	};

	std::string report;
	auto compare = [&](AssFile const& source, std::vector<KaraokeLineClassification> const& parsed, bool after_save) {
		size_t index = 0;
		for (auto const& line : source.Events) {
			auto expected = cases[index];
			if (after_save && index == 5) {
				Require(line.Effect.get() == "code line", "ASS did not trim the leading Effect whitespace");
				expected.kind = KaraokeLineKind::Code;
				expected.scopes = KaraokeLine;
			}
			else
				Require(line.Effect.get() == cases[index].effect, "Unexpected Effect change across the ASS boundary");
			auto const classified = ClassifyKaraokeLine(line.Comment, line.Effect.get());
			auto const& from_parser = parsed[index];
			auto const equal_repeat = [](double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); };
			bool const matches_parser = classified.kind == from_parser.kind && classified.scopes == from_parser.scopes && classified.all_styles == from_parser.all_styles && classified.no_blank == from_parser.no_blank && equal_repeat(classified.repeat, from_parser.repeat);
			bool const matches_expected = classified.kind == expected.kind && classified.scopes == expected.scopes && classified.all_styles == expected.all_styles && classified.no_blank == expected.no_blank && equal_repeat(classified.repeat, expected.repeat);
			report += std::string(after_save ? "reopened " : "memory ") + std::to_string(index + 1) + " " + std::string(expected.effect) + " parser=" + (matches_parser ? "match" : "mismatch") + " expected=" + (matches_expected ? "match" : "mismatch") + "\n";
			WriteFile(artifacts / "parser-comparison.txt", report);
			Require(matches_parser && matches_expected, "Classifier/parser mismatch at case " + std::to_string(index + 1));
			Require(IsKaraokeCodeLine(line.Comment, line.Effect.get()) == (expected.kind == KaraokeLineKind::Code), "Code predicate mismatch");
			++index;
		}
	};
	compare(file, parse_with_templater(file), false);
	compare(reopened, parse_with_templater(reopened), true);
}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "Usage: lua-workspace-source-e2e <repo-relative-fixtures> <artifact-directory>\n";
		return 2;
	}
	try {
		auto const fixtures = RelativePath(argv[1]);
		auto const artifacts = ArtifactPath(argv[2]);
		fs::create_directories(artifacts);
		std::string manifest;
		int failures = 0;
		auto run = [&](std::string const& name, auto task) {
			try {
				task();
				manifest += name + " PASS\n";
				WriteFile(artifacts / name / "status.txt", "PASS\n");
			}
			catch (std::exception const& error) {
				++failures;
				manifest += name + " FAIL: " + error.what() + "\n";
				fs::create_directories(artifacts / name);
				WriteFile(artifacts / name / "status.txt", std::string("FAIL: ") + error.what() + "\n");
			}
			WriteFile(artifacts / "manifest.txt", manifest);
		};
		run("classifier", [&] { fs::create_directories(artifacts / "classifier"); CheckClassifier(artifacts / "classifier"); });
		for (auto const& name : {"lexical", "strings", "crlf", "bom_shebang", "scope", "control", "empty", "member-access", "semicolons"})
			run(name, [&] { RunExecutableCase(fixtures, artifacts / name, name); });
		run("validate_only", [&] { RunValidateOnlyCase(fixtures, artifacts / "validate_only"); });
		for (auto const& name : {"invalid_escape", "binary"})
			run(name, [&] { RunInvalidCase(fixtures, artifacts / name, name); });
		std::cout << manifest;
		return failures ? 1 : 0;
	}
	catch (std::exception const& error) {
		std::cerr << error.what() << '\n';
		return 2;
	}
}
