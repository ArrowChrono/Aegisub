#include "karaoke_line_classifier.h"

#include <lua.hpp>

#include <utility>

namespace Automation4 {
namespace {
bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::pair<std::string_view, std::string_view> HeadTail(std::string_view text) {
	auto const separator = text.find_first_of(" \t\n\r\f\v");
	if (separator == std::string_view::npos)
		return {text, {}};

	auto tail_start = separator;
	while (tail_start < text.size() && IsSpace(text[tail_start]))
		++tail_start;
	return {text.substr(0, separator), text.substr(tail_start)};
}

bool EqualsIgnoreCase(std::string_view text, std::string_view expected) {
	if (text.size() != expected.size())
		return false;
	for (size_t i = 0; i < text.size(); ++i) {
		auto c = text[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != expected[i])
			return false;
	}
	return true;
}

struct NumberState {
	lua_State *state = luaL_newstate();
	~NumberState() {
		if (state)
			lua_close(state);
	}
};

bool ParseRepeat(std::string_view word, double& value) {
	if (word.empty())
		return false;
	thread_local NumberState number_state;
	auto *state = number_state.state;
	if (!state)
		return false;

	lua_pushlstring(state, word.data(), word.size());
	bool const valid = lua_isnumber(state, -1);
	if (valid)
		value = lua_tonumber(state, -1);
	lua_pop(state, 1);
	return valid;
}
}

KaraokeLineClassification ClassifyKaraokeLine(bool comment, std::string_view effect) {
	KaraokeLineClassification result;
	if (!comment)
		return result;

	auto [first, rest] = HeadTail(effect);
	if (EqualsIgnoreCase(first, "template")) {
		result.kind = KaraokeLineKind::Template;
		return result;
	}
	if (!EqualsIgnoreCase(first, "code"))
		return result;

	result.kind = KaraokeLineKind::Code;
	while (!rest.empty()) {
		auto [modifier, tail] = HeadTail(rest);
		rest = tail;
		if (EqualsIgnoreCase(modifier, "once"))
			result.scopes |= KaraokeOnce;
		else if (EqualsIgnoreCase(modifier, "line"))
			result.scopes |= KaraokeLine;
		else if (EqualsIgnoreCase(modifier, "syl"))
			result.scopes |= KaraokeSyllable;
		else if (EqualsIgnoreCase(modifier, "furi"))
			result.scopes |= KaraokeFurigana;
		else if (EqualsIgnoreCase(modifier, "all"))
			result.all_styles = true;
		else if (EqualsIgnoreCase(modifier, "noblank"))
			result.no_blank = true;
		else if (EqualsIgnoreCase(modifier, "repeat") || EqualsIgnoreCase(modifier, "loop")) {
			auto [times, after_times] = HeadTail(rest);
			double repeat = 1;
			if (ParseRepeat(times, repeat)) {
				result.repeat = repeat;
				rest = after_times;
			}
			else
				result.repeat = 1;
		}
	}
	if (!result.scopes)
		result.scopes = KaraokeOnce;
	return result;
}

bool IsKaraokeCodeLine(bool comment, std::string_view effect) {
	return comment && EqualsIgnoreCase(HeadTail(effect).first, "code");
}
}
