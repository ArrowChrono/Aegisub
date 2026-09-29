#include "lua_source_tools.h"
#include "lua_text_utf8.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Automation4 {
namespace {
using LuaState = std::unique_ptr<lua_State, decltype(&lua_close)>;

enum class TokenKind : std::uint8_t { Identifier,
									  Keyword,
									  Number,
									  String,
									  Symbol,
									  Comment,
									  Header };

struct Token {
	TokenKind kind;
	std::string_view text;
	std::string_view comment_body;
	size_t offset = 0;
	bool line_comment = false;
	bool newline_before = false;
};

bool NeedsSpace(Token const *previous, Token const& current) {
	if (previous && previous->kind == TokenKind::Symbol && previous->text == "[" && current.text.starts_with("["))
		return true;
	auto tight_before = [](Token const& token) {
		return token.kind == TokenKind::Symbol &&
			   (token.text == "." || token.text == ":" || token.text == "(" || token.text == "[" || token.text == ")" || token.text == "]" || token.text == "," || token.text == ";");
	};
	auto tight_after = [](Token const& token) {
		return token.kind == TokenKind::Symbol && (token.text == "." || token.text == ":" || token.text == "(" || token.text == "[");
	};
	return previous && !tight_after(*previous) && !tight_before(current);
}

bool IsNewline(char c) { return c == '\r' || c == '\n'; }
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || IsNewline(c); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIdentifier(char c) {
	return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 128;
}

bool IsKeyword(std::string_view text) {
	constexpr std::array words = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
	return std::ranges::find(words, text) != words.end();
}

LuaSourceDiagnostic LuaError(lua_State *state) {
	size_t length = 0;
	char const *message = lua_tolstring(state, -1, &length);
	LuaSourceDiagnostic result{.message = message ? std::string(message, length) : "Lua compiler failed without a diagnostic"};
	for (size_t pos = result.message.find(':'); pos != std::string::npos; pos = result.message.find(':', pos + 1)) {
		int line = 0;
		char const *first = result.message.data() + pos + 1;
		char const *last = result.message.data() + result.message.size();
		auto parsed = std::from_chars(first, last, line);
		if (parsed.ec == std::errc{} && parsed.ptr != last && *parsed.ptr == ':') {
			result.line = line;
			break;
		}
	}
	return result;
}

std::optional<LuaSourceDiagnostic> Compile(lua_State *state, std::string_view source) {
	lua_settop(state, 0);
	if (luaL_loadbufferx(state, source.empty() ? "" : source.data(), source.size(), "=lua-workspace", "t") != 0)
		return LuaError(state);
	return std::nullopt;
}

std::optional<size_t> LongBracketSize(std::string_view source, size_t pos) {
	if (pos >= source.size() || source[pos] != '[')
		return std::nullopt;
	size_t end = pos + 1;
	while (end < source.size() && source[end] == '=')
		++end;
	if (end < source.size() && source[end] == '[')
		return end - pos + 1;
	return std::nullopt;
}

std::optional<LuaSourceDiagnostic> Tokenize(std::string_view source, std::vector<Token>& tokens) {
	size_t pos = source.starts_with("\xef\xbb\xbf") ? 3 : 0;
	if (pos < source.size() && source[pos] == '#') {
		size_t start = pos;
		while (pos < source.size() && !IsNewline(source[pos]))
			++pos;
		auto text = source.substr(start, pos - start);
		tokens.push_back({.kind = TokenKind::Header, .text = text, .comment_body = text, .offset = start, .line_comment = true});
	}
	while (pos < source.size()) {
		bool newline_before = false;
		while (pos < source.size() && IsSpace(source[pos])) {
			newline_before |= IsNewline(source[pos]);
			++pos;
		}
		if (pos == source.size())
			break;
		size_t start = pos;
		Token token{.kind = TokenKind::Symbol, .text = {}, .comment_body = {}, .offset = start};
		token.newline_before = newline_before;
		bool comment = source.substr(pos).starts_with("--");
		if (comment)
			pos += 2;
		auto bracket = LongBracketSize(source, pos);
		if (bracket) {
			size_t body_start = pos + *bracket;
			std::string closing = "]" + std::string(*bracket - 2, '=') + "]";
			size_t end = source.find(closing, body_start);
			if (end == std::string_view::npos)
				return LuaSourceDiagnostic{.message = "Unterminated Lua long bracket"};
			token.kind = comment ? TokenKind::Comment : TokenKind::String;
			token.comment_body = source.substr(body_start, end - body_start);
			pos = end + closing.size();
		}
		else if (comment) {
			size_t body_start = pos;
			while (pos < source.size() && !IsNewline(source[pos]))
				++pos;
			token.kind = TokenKind::Comment;
			token.line_comment = true;
			token.comment_body = source.substr(body_start, pos - body_start);
		}
		else if (source[pos] == '\'' || source[pos] == '"') {
			char quote = source[pos++];
			bool closed = false;
			while (pos < source.size()) {
				char c = source[pos++];
				if (c == quote) {
					closed = true;
					break;
				}
				if (c != '\\')
					continue;
				if (pos == source.size())
					break;
				char escaped = source[pos++];
				if (escaped == 'z') {
					while (pos < source.size() && IsSpace(source[pos]))
						++pos;
				}
				else if (IsNewline(escaped) && pos < source.size() && IsNewline(source[pos]) && source[pos] != escaped)
					++pos;
			}
			if (!closed)
				return LuaSourceDiagnostic{.message = "Unterminated Lua short string"};
			token.kind = TokenKind::String;
		}
		else if (IsDigit(source[pos]) || (source[pos] == '.' && pos + 1 < source.size() && IsDigit(source[pos + 1]))) {
			char exponent = 'e';
			char previous = source[pos];
			if (source[pos] == '.')
				previous = source[++pos];
			if (source[pos] == '0') {
				++pos;
				if (pos < source.size() && (source[pos] == 'x' || source[pos] == 'X'))
					exponent = 'p';
			}
			while (pos < source.size() && (IsIdentifier(source[pos]) || source[pos] == '.' || ((source[pos] == '-' || source[pos] == '+') && (previous == exponent || previous == exponent - ('a' - 'A')))))
				previous = source[pos++];
			token.kind = TokenKind::Number;
		}
		else if (IsIdentifier(source[pos])) {
			while (pos < source.size() && IsIdentifier(source[pos]))
				++pos;
			token.kind = IsKeyword(source.substr(start, pos - start)) ? TokenKind::Keyword : TokenKind::Identifier;
		}
		else {
			++pos;
			if (source[start] == '.' && pos < source.size() && source[pos] == '.') {
				++pos;
				if (pos < source.size() && source[pos] == '.')
					++pos;
			}
			else if (pos < source.size() && ((source[pos] == '=' && (source[start] == '=' || source[start] == '<' || source[start] == '>' || source[start] == '~')) || (source[start] == ':' && source[pos] == ':')))
				++pos;
		}
		token.text = source.substr(start, pos - start);
		tokens.push_back(token);
	}
	return std::nullopt;
}

std::string EscapeString(std::string_view value) {
	std::string result = "\"";
	for (size_t index = 0; index < value.size();) {
		auto const c = static_cast<unsigned char>(value[index]);
		auto const length = Utf8SequenceLength(value, index);
		if (c == '"' || c == '\\') {
			result += '\\';
			result += static_cast<char>(c);
		}
		else if (!length || c < 32 || c == 127) {
			result += '\\';
			result += static_cast<char>('0' + c / 100);
			result += static_cast<char>('0' + c / 10 % 10);
			result += static_cast<char>('0' + c % 10);
		}
		else
			result.append(value.substr(index, length));
		index += length ? length : 1;
	}
	result += '"';
	return result;
}

LuaSourceResult SerializeString(Token const& token) {
	if (token.kind != TokenKind::String)
		return {.source = {}, .diagnostic = LuaSourceDiagnostic{.message = "Expected an isolated Lua string token"}};
	std::vector<Token> isolated;
	if (auto diagnostic = Tokenize(token.text, isolated))
		return {.source = {}, .diagnostic = diagnostic};
	if (isolated.size() != 1 || isolated.front().kind != TokenKind::String || isolated.front().text.size() != token.text.size())
		return {.source = {}, .diagnostic = LuaSourceDiagnostic{.message = "Expected exactly one complete Lua string token"}};
	LuaState state(luaL_newstate(), lua_close);
	if (!state)
		return {.source = {}, .diagnostic = LuaSourceDiagnostic{.message = "Unable to allocate a Lua compiler state"}};
	std::string literal = "return ";
	literal += token.text;
	if (auto diagnostic = Compile(state.get(), literal))
		return {.source = {}, .diagnostic = diagnostic};
	if (lua_pcall(state.get(), 0, 1, 0) != 0)
		return {.source = {}, .diagnostic = LuaError(state.get())};
	if (lua_type(state.get(), -1) != LUA_TSTRING)
		return {.source = {}, .diagnostic = LuaSourceDiagnostic{.message = "Lua string literal did not produce a string"}};
	size_t length = 0;
	char const *value = lua_tolstring(state.get(), -1, &length);
	return {.source = EscapeString(std::string_view(value, length)), .diagnostic = std::nullopt};
}

std::string SerializeComment(std::string_view body) {
	std::string flattened;
	flattened.reserve(body.size());
	for (size_t i = 0; i < body.size(); ++i) {
		char c = body[i];
		flattened += IsNewline(c) ? ' ' : c;
		if (IsNewline(c) && i + 1 < body.size() && IsNewline(body[i + 1]) && body[i + 1] != c)
			++i;
	}
	std::string equals;
	for (;;) {
		std::string closing = "]" + equals + "]";
		if ((flattened + closing).find(closing) == flattened.size()) {
			std::string result = "--[";
			result += equals;
			result += '[';
			result += flattened;
			result += closing;
			return result;
		}
		equals += '=';
	}
}

LuaSourceResult Failure(std::string_view source, LuaSourceDiagnostic diagnostic) {
	return {.source = std::string(source), .diagnostic = std::move(diagnostic)};
}

int AppendDump(lua_State *, void const *data, size_t size, void *output) {
	static_cast<std::string *>(output)->append(static_cast<char const *>(data), size);
	return 0;
}

std::optional<std::string> DumpBytecode(lua_State *state, std::string_view source) {
	lua_settop(state, 0);
	if (luaL_loadbufferx(state, source.empty() ? "" : source.data(), source.size(), "=lua-workspace", "t") != 0)
		return std::nullopt;
	std::string output;
	if (lua_dump(state, AppendDump, &output) != 0)
		return std::nullopt;
	return output;
}

class StatementBoundaries {
	std::vector<Token const *> tokens;
	size_t position = 0;
	std::vector<size_t>& boundaries;
	std::vector<size_t>& semicolons;

	[[nodiscard]] std::string_view Peek(size_t ahead = 0) const {
		return position + ahead < tokens.size() ? tokens[position + ahead]->text : std::string_view{};
	}
	[[nodiscard]] bool Kind(TokenKind kind) const { return position < tokens.size() && tokens[position]->kind == kind; }
	bool Take(std::string_view text) {
		if (Peek() != text)
			return false;
		++position;
		return true;
	}
	bool Name() {
		if (!Kind(TokenKind::Identifier))
			return false;
		++position;
		return true;
	}
	[[nodiscard]] bool BlockEnd() const {
		auto text = Peek();
		return text.empty() || text == "end" || text == "else" || text == "elseif" || text == "until";
	}
	bool ExpressionList() {
		do {
			if (!Expression())
				return false;
		} while (Take(","));
		return true;
	}
	bool FunctionBody() {
		if (!Take("("))
			return false;
		if (!Take(")")) {
			do {
				if (!Name() && !Take("..."))
					return false;
			} while (Take(","));
			if (!Take(")"))
				return false;
		}
		return Block() && Take("end");
	}
	bool Table() {
		if (!Take("{"))
			return false;
		while (!Take("}")) {
			if (Take("[")) {
				if (!Expression() || !Take("]") || !Take("="))
					return false;
			}
			else if (Kind(TokenKind::Identifier) && Peek(1) == "=")
				position += 2;
			if (!Expression())
				return false;
			if (!Take(",") && !Take(";"))
				return Take("}");
		}
		return true;
	}
	bool Arguments() {
		if (Take("("))
			return Take(")") || (ExpressionList() && Take(")"));
		if (Kind(TokenKind::String)) {
			++position;
			return true;
		}
		return Table();
	}
	bool Prefix() {
		if (Take("(")) {
			if (!Expression() || !Take(")"))
				return false;
		}
		else if (!Name())
			return false;
		for (;;) {
			if (Take(".")) {
				if (!Name())
					return false;
			}
			else if (Take("[")) {
				if (!Expression() || !Take("]"))
					return false;
			}
			else if (Take(":")) {
				if (!Name() || !Arguments())
					return false;
			}
			else if (Peek() == "(" || Peek() == "{" || Kind(TokenKind::String)) {
				if (!Arguments())
					return false;
			}
			else
				return true;
		}
	}
	bool Operand() {
		while (Take("not") || Take("-") || Take("#")) {
		}
		if (Take("function"))
			return FunctionBody();
		if (Peek() == "{")
			return Table();
		if (Kind(TokenKind::Number) || Kind(TokenKind::String)) {
			++position;
			return true;
		}
		return Take("nil") || Take("true") || Take("false") || Take("...") || Prefix();
	}
	bool Expression() {
		constexpr std::array operators = {"or", "and", "<", ">", "<=", ">=", "~=", "==", "..", "+", "-", "*", "/", "%", "^"};
		if (!Operand())
			return false;
		while (std::ranges::find(operators, Peek()) != operators.end()) {
			++position;
			if (!Operand())
				return false;
		}
		return true;
	}
	bool Statement() {
		if (Take("if")) {
			do {
				if (!Expression() || !Take("then") || !Block())
					return false;
			} while (Take("elseif"));
			return (!Take("else") || Block()) && Take("end");
		}
		if (Take("while"))
			return Expression() && Take("do") && Block() && Take("end");
		if (Take("do"))
			return Block() && Take("end");
		if (Take("repeat"))
			return Block() && Take("until") && Expression();
		if (Take("for")) {
			do {
				if (!Name())
					return false;
			} while (Take(","));
			return (Take("=") || Take("in")) && ExpressionList() && Take("do") && Block() && Take("end");
		}
		if (Take("function")) {
			if (!Name())
				return false;
			while (Take(".") || Take(":")) {
				if (!Name())
					return false;
			}
			return FunctionBody();
		}
		if (Take("local")) {
			if (Take("function"))
				return Name() && FunctionBody();
			do {
				if (!Name())
					return false;
			} while (Take(","));
			return !Take("=") || ExpressionList();
		}
		if (Take("return"))
			return BlockEnd() || Peek() == ";" || ExpressionList();
		if (Take("break"))
			return true;
		if (Take("goto"))
			return Name();
		if (Take("::"))
			return Name() && Take("::");
		do {
			if (!Prefix())
				return false;
		} while (Take(","));
		return !Take("=") || ExpressionList();
	}
	bool Block() {
		bool previous_statement = false;
		while (!BlockEnd()) {
			if (Peek() == ";") {
				semicolons.push_back(tokens[position++]->offset);
				previous_statement = false;
				continue;
			}
			if (previous_statement && tokens[position - 1]->text != "end")
				boundaries.push_back(tokens[position]->offset);
			if (!Statement())
				return false;
			previous_statement = true;
		}
		return true;
	}

	public:
	StatementBoundaries(std::vector<Token> const& source, std::vector<size_t>& boundaries, std::vector<size_t>& semicolons)
		: boundaries(boundaries), semicolons(semicolons) {
		for (auto const& token : source)
			if (token.kind != TokenKind::Comment && token.kind != TokenKind::Header)
				tokens.push_back(&token);
	}
	bool Read() { return Block() && position == tokens.size(); }
};

std::string InsertSeparators(std::string_view source, std::vector<size_t> const& offsets) {
	std::string output;
	output.reserve(source.size() + offsets.size());
	size_t copied = 0;
	for (auto offset : offsets) {
		output.append(source.substr(copied, offset - copied));
		output += ';';
		copied = offset;
	}
	output.append(source.substr(copied));
	return output;
}

std::optional<LuaSourceDiagnostic> FindStatementBoundaries(std::string_view source, std::vector<Token> const& tokens, std::vector<size_t>& boundaries, std::vector<size_t>& statement_semicolons) {
	if (!StatementBoundaries(tokens, boundaries, statement_semicolons).Read())
		return LuaSourceDiagnostic{.message = "Unable to read Lua statement structure"};
	LuaState state(luaL_newstate(), lua_close);
	if (!state)
		return LuaSourceDiagnostic{.message = "Unable to allocate a Lua compiler state"};
	auto const original = DumpBytecode(state.get(), source);
	if (!original)
		return LuaError(state.get());

	if (auto const separated = DumpBytecode(state.get(), InsertSeparators(source, boundaries)); !separated || *separated != *original)
		return LuaSourceDiagnostic{.message = "Unable to preserve Lua statement structure while formatting"};
	return std::nullopt;
}
}

std::optional<LuaSourceDiagnostic> ValidateLuaSource(std::string_view source) {
	LuaState state(luaL_newstate(), lua_close);
	if (!state)
		return LuaSourceDiagnostic{.message = "Unable to allocate a Lua compiler state"};
	return Compile(state.get(), source);
}

LuaSourceResult SerializeLuaSource(std::string_view source) {
	if (auto diagnostic = ValidateLuaSource(source))
		return Failure(source, *diagnostic);
	std::vector<Token> tokens;
	if (auto diagnostic = Tokenize(source, tokens))
		return Failure(source, *diagnostic);
	std::vector<size_t> boundaries;
	std::vector<size_t> statement_semicolons;
	if (auto diagnostic = FindStatementBoundaries(source, tokens, boundaries, statement_semicolons))
		return Failure(source, *diagnostic);
	std::string output;
	Token const *previous = nullptr;
	for (auto const& token : tokens) {
		Token emitted = token;
		if (emitted.kind == TokenKind::String)
			emitted.text = "\"";
		if (std::ranges::binary_search(boundaries, token.offset)) {
			output += "; ";
			previous = nullptr;
		}
		if (NeedsSpace(previous, emitted))
			output += ' ';
		if (token.kind == TokenKind::String) {
			auto literal = SerializeString(token);
			if (!literal.Succeeded())
				return Failure(source, *literal.diagnostic);
			output += literal.source;
		}
		else if (token.kind == TokenKind::Comment || token.kind == TokenKind::Header)
			output += SerializeComment(token.comment_body);
		else
			output += token.text;
		previous = &token;
	}
	if (auto diagnostic = ValidateLuaSource(output))
		return Failure(source, *diagnostic);
	return {.source = std::move(output), .diagnostic = std::nullopt};
}

LuaSourceResult FormatLuaSource(std::string_view source) {
	if (auto diagnostic = ValidateLuaSource(source))
		return Failure(source, *diagnostic);
	std::vector<Token> tokens;
	if (auto diagnostic = Tokenize(source, tokens))
		return Failure(source, *diagnostic);
	std::vector<size_t> boundaries;
	std::vector<size_t> statement_semicolons;
	if (auto diagnostic = FindStatementBoundaries(source, tokens, boundaries, statement_semicolons))
		return Failure(source, *diagnostic);
	std::string output;
	int indent = 0;
	bool function_parameters = false;
	Token const *previous = nullptr;
	auto newline = [&] {
		if (!output.empty() && output.back() != '\n')
			output += '\n';
	};
	for (auto const& token : tokens) {
		auto text = token.text;
		bool keyword = token.kind == TokenKind::Keyword;
		bool closer = keyword && (text == "end" || text == "until" || text == "else" || text == "elseif");
		if (closer) {
			indent = std::max(0, indent - 1);
			newline();
		}
		if (std::ranges::binary_search(boundaries, token.offset) || (token.kind == TokenKind::Comment && token.newline_before))
			newline();
		if (output.empty() || output.back() == '\n')
			output.append(static_cast<size_t>(indent), '\t');
		else if (NeedsSpace(previous, token))
			output += ' ';
		output += text;
		previous = &token;
		if (keyword && text == "function") {
			++indent;
			function_parameters = true;
		}
		else if (function_parameters && token.kind == TokenKind::Symbol && text == ")") {
			function_parameters = false;
			newline();
		}
		else if (keyword && (text == "then" || text == "do" || text == "else" || text == "repeat")) {
			++indent;
			newline();
		}
		else if ((keyword && text == "end") || std::ranges::binary_search(statement_semicolons, token.offset) || token.line_comment)
			newline();
	}
	if (auto diagnostic = ValidateLuaSource(output))
		return Failure(source, *diagnostic);
	return {.source = std::move(output), .diagnostic = std::nullopt};
}
}
