#include "lua_language_environment.h"

#include "karaoke_line_classifier.h"

#include <algorithm>
#include <map>
#include <ranges>

namespace Automation4 {
namespace {
constexpr auto record_types = R"lua(---@meta
---@class AegisubInfo
---@field class 'info'
---@field section string
---@field raw string
---@field key string
---@field value string
---@class AegisubDialogue
---@field class 'dialogue'
---@field section string
---@field raw string
---@field comment boolean
---@field layer integer
---@field start_time integer
---@field end_time integer
---@field style string
---@field actor string
---@field effect string
---@field margin_l integer
---@field margin_r integer
---@field margin_t integer
---@field margin_b integer
---@field text string
---@field extra table<string, string>
---@class AegisubStyle
---@field class 'style'
---@field section string
---@field raw string
---@field name string
---@field fontname string
---@field fontsize number
---@field color1 string
---@field color2 string
---@field color3 string
---@field color4 string
---@field bold boolean
---@field italic boolean
---@field underline boolean
---@field strikeout boolean
---@field scale_x number
---@field scale_y number
---@field spacing number
---@field angle number
---@field borderstyle integer
---@field outline number
---@field shadow number
---@field align integer
---@field margin_l integer
---@field margin_r integer
---@field margin_t integer
---@field margin_b integer
---@field encoding integer
---@field relative_to integer
)lua";

constexpr auto automation_api_types = R"lua(---@alias AegisubSubtitleRecord AegisubInfo|AegisubDialogue|AegisubStyle
---@class AegisubSubtitles
---@field [integer] AegisubSubtitleRecord
---@field n integer
---@field delete fun(index?: integer|integer[], ...: integer)
---@field deleterange fun(first: integer, last: integer)
---@field insert fun(before: integer, ...: AegisubSubtitleRecord)
---@field append fun(...: AegisubSubtitleRecord)
---@field script_resolution fun(): integer, integer
---@class AegisubProgress
---@field set fun(percent: number)
---@field task fun(message: string)
---@field title fun(title: string)
---@field is_cancelled fun(): boolean
---@class AegisubDebug
---@field out fun(level: integer|string, format?: string, ...: any)
---@class AegisubDialog
---@field display function
---@field open function
---@field save function
---@alias AegisubMacroRun fun(subs: AegisubSubtitles, selected: integer[], active: integer): integer[]?, integer?
---@alias AegisubMacroValidate fun(subs: AegisubSubtitles, selected: integer[], active: integer?): boolean, string?
---@alias AegisubMacroIsActive fun(subs: AegisubSubtitles, selected: integer[], active: integer): boolean
---@class AegisubApi
---@field lua_automation_version integer
---@field register_macro fun(name: string, description: string, run: AegisubMacroRun, validate?: AegisubMacroValidate, isactive?: AegisubMacroIsActive, execution_id?: string)
---@field register_filter fun(name: string, description: string, priority: integer, run: fun(subs: AegisubSubtitles, config: table), config?: function)
---@field text_extents fun(style: AegisubStyle, text: string): number, number, number, number
---@field frame_from_ms fun(milliseconds: integer): integer?
---@field ms_from_frame fun(frame: integer): integer?
---@field video_size fun(): integer?, integer?, number?, integer?
---@field keyframes fun(): integer[]?
---@field decode_path fun(path: string): string
---@field cancel fun()
---@field file_name fun(): string?
---@field gettext fun(message: string): string
---@field project_properties fun(): table<string, any>?
---@field get_audio_selection fun(): integer?, integer?
---@field get_visual_guides fun(): table<string, any>
---@field scroll_audio_to fun(time_ms: integer): boolean
---@field set_status_text fun(text: string)
---@field focus_edit_box fun(): boolean
---@field get_edit_box_cursor fun(): integer?, integer?
---@field set_edit_box_cursor fun(start: integer, stop_or_after?: integer|boolean): boolean
---@field parse_karaoke_data? fun(line: AegisubDialogue): table<integer, table>
---@field set_undo_point? fun(description: string)
---@field progress? AegisubProgress
---@field debug? AegisubDebug
---@field log? fun(level: integer|string, format?: string, ...: any)
---@field dialog? AegisubDialog
)lua";

constexpr auto automation_globals = R"lua(
---@type AegisubApi
aegisub = {}
---@param path string
---@return any
function include(path) end
)lua";

constexpr auto karaoke_types = R"lua(---@class KaraokeTableLibrary
---@field insert fun(list: table, position_or_value: any, value?: any)
---@field remove fun(list: table, position?: integer): any
---@field concat fun(list: table, separator?: string, first?: integer, last?: integer): string
---@field sort fun(list: table, compare?: function)
---@class KaraokeHostGlobals
---@field aegisub AegisubApi
---@field tostring fun(value: any): string
---@field tonumber fun(value: any, base?: integer): number?
---@field pairs fun(value: table): function
---@field ipairs fun(value: table): function
---@field table KaraokeTableLibrary
---@field include fun(path: string): any
---@field require fun(name: string): any
---@field assert fun(value: any, message?: any): any
---@field error fun(message: any, level?: integer)
---@field pcall fun(callback: function, ...: any): boolean, any
---@field string table
---@field math table
---@class KaraokeMeta
---@field res_x number
---@field res_y number
---@field video_x_correct_factor number
---@field [string] string|number
---@class KaraokeHighlight
---@field start_time number
---@field end_time number
---@field duration number
---@class KaraokeHighlightCollection
---@field [integer] KaraokeHighlight
---@field n integer
---@class KaraokeSyllableCollection
---@field [integer] KaraokeSyllable
---@field n integer
---@class KaraokeFuriganaCollection
---@field [integer] KaraokeFurigana
---@field n integer
---@class KaraokeFragment
---@field i integer
---@field text string
---@field text_stripped string
---@field text_spacestripped string
---@field prespace string
---@field postspace string
---@field start_time number
---@field end_time number
---@field duration number
---@field kdur number
---@field tag string
---@field inline_fx string
---@field line KaraokeLine
---@field highlights KaraokeHighlightCollection
---@class KaraokeSyllable: KaraokeFragment
---@field furi KaraokeFuriganaCollection
---@field width number
---@field height number
---@field prespacewidth number
---@field postspacewidth number
---@field left number
---@field center number
---@field right number
---@field style AegisubStyle
---@class KaraokeFurigana: KaraokeFragment
---@field isfuri true
---@field isbreak boolean
---@field spillback boolean
---@field syl KaraokeSyllable
---@field width? number
---@field height? number
---@field prespacewidth? number
---@field postspacewidth? number
---@field left? number
---@field center? number
---@field right? number
---@field style? AegisubStyle
---@class KaraokeLine: AegisubDialogue
---@field i integer
---@field text_stripped string
---@field duration number
---@field styleref AegisubStyle
---@field width number
---@field height number
---@field descent number
---@field extlead number
---@field margin_v integer
---@field eff_margin_l integer
---@field eff_margin_r integer
---@field eff_margin_t integer
---@field eff_margin_b integer
---@field eff_margin_v? integer
---@field x number
---@field y number
---@field left number
---@field center number
---@field right number
---@field top number
---@field middle number
---@field bottom number
---@field halign 'left'|'center'|'right'
---@field valign 'top'|'middle'|'bottom'
---@field hcenter number
---@field vcenter number
---@field kara KaraokeSyllableCollection
---@field furi KaraokeFuriganaCollection
---@class KaraokeRecall
---@field [string] any
---@operator call(name: string, default?: any): any
---@class KaraokeEnvironment
---@field meta KaraokeMeta
---@field string table
---@field math table
---@field _G KaraokeHostGlobals
---@field tenv KaraokeEnvironment
---@field fxgroup table<string, boolean>
---@field recall KaraokeRecall
---@field retime fun(mode: string, addstart?: number, addend?: number): string
---@field relayer fun(layer: integer): string
---@field restyle fun(style: string): string
---@field maxloop fun(maxj: integer): string
---@field maxloops fun(maxj: integer): string
---@field loopctl fun(j: integer, maxj: integer): string
---@field remember fun(name: string, value: any, decorator?: function): any
---@field remember_line fun(name: string, value: any): any
---@field remember_syl fun(name: string, value: any): any
---@field remember_basesyl fun(name: string, value: any): any
---@field remember_if fun(name: string, value: any, condition: boolean, decorator?: function): any
---@field j integer
---@field maxj integer
---@type KaraokeMeta
meta = {}
---@type table<string, boolean>
fxgroup = {}
---@type KaraokeRecall
recall = {}
---@type KaraokeHostGlobals
_G = {}
---@type integer
j = 0
---@type integer
maxj = 0
---@param mode string
---@param addstart? number
---@param addend? number
---@return string
function retime(mode, addstart, addend) end
---@param layer integer
---@return string
function relayer(layer) end
---@param style string
---@return string
function restyle(style) end
---@param newmaxj integer
---@return string
function maxloop(newmaxj) end
---@param newmaxj integer
---@return string
function maxloops(newmaxj) end
---@param newj integer
---@param newmaxj integer
---@return string
function loopctl(newj, newmaxj) end
---@param name string
---@param value any
---@param decorator? function
---@return any
function remember(name, value, decorator) end
---@param name string
---@param value any
---@return any
function remember_line(name, value) end
---@param name string
---@param value any
---@return any
function remember_syl(name, value) end
---@param name string
---@param value any
---@return any
function remember_basesyl(name, value) end
---@param name string
---@param value any
---@param condition boolean
---@param decorator? function
---@return any
function remember_if(name, value, condition, decorator) end
)lua";

std::string_view Trim(std::string_view value) {
	auto first = value.find_first_not_of(" \t\r");
	if (first == std::string_view::npos)
		return {};
	return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

bool Identifier(std::string_view value) {
	auto start = [](char character) { return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_'; };
	if (value.empty() || !start(value.front()))
		return false;
	return std::ranges::all_of(value.substr(1), [&](char character) { return start(character) || (character >= '0' && character <= '9'); });
}

LuaHostHints ParseHostHints(std::string_view definitions) {
	LuaHostHints hints;
	std::string current_class;
	std::string pending_type;
	while (!definitions.empty()) {
		auto end = definitions.find('\n');
		auto line = Trim(definitions.substr(0, end));
		definitions = end == std::string_view::npos ? std::string_view{} : definitions.substr(end + 1);
		if (line.starts_with("---@class ")) {
			auto value = Trim(line.substr(10));
			auto name_end = value.find_first_of(" :\t");
			current_class = std::string(value.substr(0, name_end));
			if (!Identifier(current_class)) {
				current_class.clear();
				continue;
			}
			auto& entry = hints.classes[current_class];
			if (auto colon = value.find(':'); colon != std::string_view::npos)
				entry.parent = std::string(Trim(value.substr(colon + 1)));
			continue;
		}
		if (line.starts_with("---@field ") && !current_class.empty()) {
			auto value = Trim(line.substr(10));
			auto name_end = value.find_first_of(" \t");
			auto name = value.substr(0, name_end);
			bool optional = name.ends_with('?');
			if (optional)
				name.remove_suffix(1);
			if (Identifier(name) && name_end != std::string_view::npos)
				hints.classes[current_class].fields.push_back({.name = std::string(name), .type = std::string(Trim(value.substr(name_end))), .optional = optional});
			continue;
		}
		if (line.starts_with("---@type ")) {
			pending_type = std::string(Trim(line.substr(9)));
			current_class.clear();
			continue;
		}
		if (!pending_type.empty()) {
			if (auto equal = line.find('='); equal != std::string_view::npos) {
				auto name = Trim(line.substr(0, equal));
				if (Identifier(name))
					hints.roots[std::string(name)] = pending_type;
			}
			pending_type.clear();
		}
	}
	return hints;
}

void CollectFields(LuaHostHints const& hints, std::string_view type, std::map<std::string, LuaHostHint>& fields) {
	while (!type.empty()) {
		auto separator = type.find('|');
		auto name = Trim(type.substr(0, separator));
		if (auto found = hints.classes.find(std::string(name)); found != hints.classes.end()) {
			if (!found->second.parent.empty())
				CollectFields(hints, found->second.parent, fields);
			for (auto const& field : found->second.fields) {
				auto& slot = fields[field.name];
				if (!slot.name.empty() && slot.type != field.type)
					slot.type += "|" + field.type;
				else
					slot = field;
			}
		}
		if (separator == std::string_view::npos)
			break;
		type.remove_prefix(separator + 1);
	}
}
}

std::vector<LuaHostHint> LuaHostHints::Members(std::string_view path) const {
	auto separator = path.find('.');
	auto root = path.substr(0, separator);
	if (!Identifier(root))
		return {};
	auto found = roots.find(std::string(root));
	if (found == roots.end())
		return {};
	std::string type = found->second;
	while (separator != std::string_view::npos) {
		path.remove_prefix(separator + 1);
		separator = path.find('.');
		auto member = path.substr(0, separator);
		if (!Identifier(member))
			return {};
		std::map<std::string, LuaHostHint> fields;
		CollectFields(*this, type, fields);
		auto next = fields.find(std::string(member));
		if (next == fields.end())
			return {};
		type = next->second.type;
	}
	std::map<std::string, LuaHostHint> fields;
	CollectFields(*this, type, fields);
	std::vector<LuaHostHint> result;
	result.reserve(fields.size());
	for (auto& [name, field] : fields)
		result.push_back(std::move(field));
	return result;
}

std::optional<LuaHostHint> LuaHostHints::Find(std::string_view path, std::string_view name) const {
	auto members = Members(path);
	auto found = std::ranges::find(members, name, &LuaHostHint::name);
	return found == members.end() ? std::nullopt : std::make_optional(*found);
}

LuaLanguageEnvironment BuildLuaLanguageEnvironment(unsigned scopes) {
	LuaLanguageEnvironment environment;
	environment.definitions = record_types;
	environment.definitions += automation_api_types;
	if (scopes == 0) {
		environment.definitions += automation_globals;
		environment.host_hints = ParseHostHints(environment.definitions);
		return environment;
	}
	environment.definitions += karaoke_types;
	bool const has_line = (scopes & KaraokeOnce) == 0;
	unsigned constexpr syllable_scopes = KaraokeSyllable | KaraokeFurigana;
	bool const has_syllable = (scopes & syllable_scopes) != 0 && (scopes & ~syllable_scopes) == 0;
	if (has_line) {
		environment.definitions += "---@class KaraokeLineEnvironment: KaraokeEnvironment\n---@field tenv KaraokeLineEnvironment\n---@field orgline KaraokeLine\n---@field line KaraokeLine\n---@type KaraokeLine\norgline = {}\n---@type KaraokeLine\nline = {}\n";
		if (has_syllable) {
			char const *syllable_type;
			if ((scopes & KaraokeFurigana) == 0)
				syllable_type = "KaraokeSyllable";
			else if ((scopes & KaraokeSyllable) == 0)
				syllable_type = "KaraokeFurigana";
			else
				syllable_type = "KaraokeSyllable|KaraokeFurigana";
			environment.definitions += "---@class KaraokeSyllableEnvironment: KaraokeLineEnvironment\n---@field tenv KaraokeSyllableEnvironment\n---@field syl ";
			environment.definitions += syllable_type;
			environment.definitions += "\n---@field basesyl ";
			environment.definitions += syllable_type;
			environment.definitions += "\n---@type ";
			environment.definitions += syllable_type;
			environment.definitions += "\nsyl = {}\n---@type ";
			environment.definitions += syllable_type;
			environment.definitions += "\nbasesyl = {}\n---@type KaraokeSyllableEnvironment\ntenv = {}\n";
		}
		else
			environment.definitions += "---@type KaraokeLineEnvironment\ntenv = {}\n";
	}
	else
		environment.definitions += "---@type KaraokeEnvironment\ntenv = {}\n";
	environment.disabled_builtins = {"basic", "table", "coroutine", "package", "io", "os", "debug", "utf8", "bit", "bit32", "jit", "ffi"};
	environment.host_hints = ParseHostHints(environment.definitions);
	return environment;
}

}
