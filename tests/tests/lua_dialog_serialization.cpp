#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_io_core.h"
#include "../../src/auto4_lua.h"
#include "../../src/automation/automation_features.h"
#include "../../src/string_codec.h"

#include <libaegisub/fs.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/vfr.h>

#include <lua.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace {
using namespace Automation4;

enum class StringControl : std::uint8_t { Edit,
										  Textbox,
										  Dropdown };

AutomationDialogValue StringValue(StringControl type, std::string value) {
	if (type == StringControl::Dropdown)
		return AutomationDialogDropdown{.items = {"default"}, .value = std::move(value)};
	return AutomationDialogEdit{.value = std::move(value), .multiline = type == StringControl::Textbox};
}

std::string const& StringValue(AutomationDialogValue const& value) {
	if (auto const *dropdown = std::get_if<AutomationDialogDropdown>(&value))
		return dropdown->value;
	return std::get<AutomationDialogEdit>(value).value;
}

std::string UnicodeWire() {
	if (std::numeric_limits<char>::is_signed)
		return "#E5#AD#97#E5#B9#95";
	return "\xE5\xAD\x97\xE5\xB9\x95";
}

class lua_dialog_serialization : public testing::Test {
	protected:
	std::unique_ptr<lua_State, decltype(&lua_close)> state{luaL_newstate(), lua_close};

	void SetUp() override {
		ASSERT_NE(nullptr, state);
	}

	std::unique_ptr<LuaDialog> Dialog() {
		lua_settop(state.get(), 0);
		lua_newtable(state.get());
		return std::make_unique<LuaDialog>(state.get(), false);
	}
};

class lua_dialog_settings_disk : public lua_dialog_serialization,
								 public testing::WithParamInterface<std::tuple<StringControl, bool>> {};

class SettingsOnlyExportFilter final : public AutomationExportFilter {
	std::string name = " Leading:|#";

	public:
	[[nodiscard]] std::string const& Name() const override { return name; }
	void SetName(std::string value) override { name = std::move(value); }
	[[nodiscard]] std::string const& Description() const override { return name; }
	[[nodiscard]] int Priority() const override { return 0; }
	std::shared_ptr<AutomationDialog> GenerateConfigDialog(AutomationMacroContext const&) override {
		ADD_FAILURE() << "Settings persistence must not generate a configuration dialog";
		return {};
	}
	void ProcessSubs(AssFile *, std::shared_ptr<AutomationHost>, AutomationUiAnchor) override {
		ADD_FAILURE() << "Settings persistence must not execute the filter";
	}
};
}

TEST_F(lua_dialog_serialization, serializes_string_tails_against_literal_wire_values) {
	struct Case {
		std::string value;
		std::string wire;
	};
	Case const cases[] = {
		{.value = "", .wire = ""},
		{.value = "   ", .wire = "#20#20#20"},
		{.value = " leading and internal space", .wire = " leading and internal space"},
		{.value = "value  ", .wire = "value#20#20"},
		{.value = "value \t", .wire = "value#20#09"},
		{.value = "value \t\r\n\f\v ", .wire = "value#20#09#0D#0A#0C#0B#20"},
		{.value = "a:|,#\xE5\xAD\x97\xE5\xB9\x95 ", .wire = "a#3A#7C#2C#23" + UnicodeWire() + "#20"},
	};
	for (auto type : {StringControl::Edit, StringControl::Textbox, StringControl::Dropdown}) {
		SCOPED_TRACE(static_cast<int>(type));
		for (auto const& sample : cases) {
			SCOPED_TRACE(sample.wire);
			auto dialog = Dialog();
			dialog->controls.push_back({.name = "tail", .value = StringValue(type, sample.value)});
			EXPECT_EQ("tail:" + sample.wire, dialog->Serialise());
		}
	}
}

TEST_F(lua_dialog_serialization, leaves_the_global_inline_codec_space_contract_unchanged) {
	EXPECT_EQ(" leading inner trailing ", inline_string_encode(" leading inner trailing "));
	EXPECT_EQ("\t leading inner trailing \r\n", inline_string_decode("#09 leading inner trailing #0D#0A"));
}

TEST_F(lua_dialog_serialization, decodes_literal_settings_to_independent_typed_values) {
	auto dialog = Dialog();
	dialog->controls = {
		{.name = "edit:|#", .value = AutomationDialogEdit{.value = "default"}},
		{.name = "multiline", .value = AutomationDialogEdit{.value = "default", .multiline = true}},
		{.name = "pick", .value = AutomationDialogDropdown{.items = {"default"}, .value = "default"}},
		{.name = "empty", .value = AutomationDialogEdit{.value = "default"}},
		{.name = "duplicate", .value = AutomationDialogEdit{.value = "default"}},
		{.name = "duplicate", .value = AutomationDialogEdit{.value = "other default"}},
		{.name = "integer", .value = AutomationDialogIntEdit{}},
		{.name = "float", .value = AutomationDialogFloatEdit{}},
		{.name = "enabled", .value = AutomationDialogCheckbox{.value = true}},
	};
	dialog->Unserialise(
		"edit#3A#7C#23: leading#3A#7C#2C#23#E5#AD#97#E5#B9#95#20#20|"
		"multiline:first#0Asecond#20#09#0D#0A#0C#0B|pick:#20#20|empty:|"
		"duplicate:first|unknown:ignored|no-separator|duplicate:last|"
		"integer: -42tail|float: +1.25e2 |enabled:0");

	EXPECT_EQ(" leading:|,#\xE5\xAD\x97\xE5\xB9\x95  ", StringValue(dialog->controls[0].value));
	EXPECT_EQ("first\nsecond \t\r\n\f\v", StringValue(dialog->controls[1].value));
	EXPECT_EQ("  ", StringValue(dialog->controls[2].value));
	EXPECT_EQ("", StringValue(dialog->controls[3].value));
	EXPECT_EQ("last", StringValue(dialog->controls[4].value));
	EXPECT_EQ("last", StringValue(dialog->controls[5].value));
	EXPECT_EQ(-42, std::get<AutomationDialogIntEdit>(dialog->controls[6].value).value);
	EXPECT_DOUBLE_EQ(125.0, std::get<AutomationDialogFloatEdit>(dialog->controls[7].value).value);
	EXPECT_FALSE(std::get<AutomationDialogCheckbox>(dialog->controls[8].value).value);
}

TEST_F(lua_dialog_serialization, retains_numeric_precision_and_legacy_acceptance) {
	auto dialog = Dialog();
	dialog->controls = {
		{.name = "integer", .value = AutomationDialogIntEdit{.value = -42}},
		{.name = "precise", .value = AutomationDialogFloatEdit{.value = 1.2345678901234567}},
		{.name = "enabled", .value = AutomationDialogCheckbox{.value = true}},
	};
	EXPECT_EQ("integer:-42|precise:1.2345678901234567|enabled:1", dialog->Serialise());

	struct Case {
		char const *wire;
		int integer;
		double floating;
		bool enabled;
	};
	Case const cases[] = {
		{.wire = "integer: +17tail|precise: -12.5tail|enabled:false", .integer = 17, .floating = -12.5, .enabled = true},
		{.wire = "integer:garbage|precise:garbage|enabled:", .integer = 0, .floating = 0.0, .enabled = true},
		{.wire = "integer:0x10|precise:0x1p-3|enabled:0", .integer = 0, .floating = 0.125, .enabled = false},
		{.wire = "integer:-1|precise: +1e-100 |enabled:00", .integer = -1, .floating = 1e-100, .enabled = true},
		{.wire = "integer:1|precise:4.9406564584124654e-324|enabled:1", .integer = 1, .floating = std::numeric_limits<double>::denorm_min(), .enabled = true},
		{.wire = "integer:2|precise:1.7976931348623157e308|enabled:1", .integer = 2, .floating = std::numeric_limits<double>::max(), .enabled = true},
	};
	for (auto const& sample : cases) {
		SCOPED_TRACE(sample.wire);
		dialog->Unserialise(sample.wire);
		EXPECT_EQ(sample.integer, std::get<AutomationDialogIntEdit>(dialog->controls[0].value).value);
		EXPECT_DOUBLE_EQ(sample.floating, std::get<AutomationDialogFloatEdit>(dialog->controls[1].value).value);
		EXPECT_EQ(sample.enabled, std::get<AutomationDialogCheckbox>(dialog->controls[2].value).value);
	}
	dialog->Unserialise("precise:-0.000000");
	EXPECT_TRUE(std::signbit(std::get<AutomationDialogFloatEdit>(dialog->controls[1].value).value));
	dialog->Unserialise("precise:1e9999");
	EXPECT_EQ(std::numeric_limits<double>::infinity(), std::get<AutomationDialogFloatEdit>(dialog->controls[1].value).value);
}

TEST_F(lua_dialog_serialization, empty_stored_filter_settings_keep_dialog_defaults) {
	SettingsOnlyExportFilter filter;
	ASSERT_EQ(" Leading#3A#7C#23", filter.SettingsIdentifier());
	AssFile file;
	file.Properties.automation_settings[" Leading#3A#7C#23"] = "";
	auto dialog = Dialog();
	dialog->controls = {
		{.name = "tail", .value = AutomationDialogEdit{.value = "default  "}},
		{.name = "integer", .value = AutomationDialogIntEdit{.value = 17}},
	};
	filter.LoadSettings(file, *dialog);
	EXPECT_EQ("default  ", StringValue(dialog->controls[0].value));
	EXPECT_EQ(17, std::get<AutomationDialogIntEdit>(dialog->controls[1].value).value);
}

TEST_P(lua_dialog_settings_disk, fresh_ass_file_restores_typed_settings_with_each_string_kind_last) {
	auto const [type, write_ui_state] = GetParam();
	SettingsOnlyExportFilter filter;
	ASSERT_EQ(" Leading#3A#7C#23", filter.SettingsIdentifier());
	auto const path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-lua-settings-%%%%%%%%.ass");
	auto cleanup = agi::make_scope_exit([&] {
		std::error_code error;
		std::filesystem::remove(path, error);
	});
	std::string const literal_payload =
		"integer:-42|precise:1.2345678901234567|enabled:1|tail: leading #3A#7C#2C#23" +
		UnicodeWire() + "#20#20#09#0D#0A#0C#0B#20#20";
	{
		auto dialog = Dialog();
		dialog->controls = {
			{.name = "integer", .value = AutomationDialogIntEdit{.value = -42}},
			{.name = "precise", .value = AutomationDialogFloatEdit{.value = 1.2345678901234567}},
			{.name = "enabled", .value = AutomationDialogCheckbox{.value = true}},
			{.name = "tail", .value = StringValue(type, " leading :|,#\xE5\xAD\x97\xE5\xB9\x95  \t\r\n\f\v  ")},
		};
		ASSERT_EQ(literal_payload, dialog->Serialise());
		AssFile original;
		original.LoadDefault();
		original.Events.front().Text = "Saved \\Nsubtitle:|,#";
		filter.SaveSettings(original, *dialog);
		ASSERT_EQ(1U, original.Properties.automation_settings.size());
		EXPECT_EQ(literal_payload, original.Properties.automation_settings.at(" Leading#3A#7C#23"));
		AssWriteOptions options;
		options.write_project_garbage = true;
		options.write_ui_state = write_ui_state;
		WriteAssFileForCore(&original, path, agi::vfr::Framerate(), "UTF-8", options);
	}

	AssFile reopened;
	ASSERT_TRUE(reopened.Properties.automation_settings.empty());
	ReadAssFileForCore(&reopened, path, "UTF-8");
	ASSERT_EQ(1U, reopened.Properties.automation_settings.size());
	auto const setting = reopened.Properties.automation_settings.find(" Leading#3A#7C#23");
	ASSERT_NE(reopened.Properties.automation_settings.end(), setting);
	EXPECT_EQ(literal_payload, setting->second);
	ASSERT_EQ(1U, reopened.Events.size());
	EXPECT_EQ("Saved \\Nsubtitle:|,#", reopened.Events.front().Text.get());

	auto restored = Dialog();
	restored->controls = {
		{.name = "integer", .value = AutomationDialogIntEdit{}},
		{.name = "precise", .value = AutomationDialogFloatEdit{}},
		{.name = "enabled", .value = AutomationDialogCheckbox{}},
		{.name = "tail", .value = StringValue(type, "default")},
	};
	filter.LoadSettings(reopened, *restored);
	EXPECT_EQ(-42, std::get<AutomationDialogIntEdit>(restored->controls[0].value).value);
	EXPECT_DOUBLE_EQ(1.2345678901234567, std::get<AutomationDialogFloatEdit>(restored->controls[1].value).value);
	EXPECT_TRUE(std::get<AutomationDialogCheckbox>(restored->controls[2].value).value);
	EXPECT_EQ(" leading :|,#\xE5\xAD\x97\xE5\xB9\x95  \t\r\n\f\v  ", StringValue(restored->controls[3].value));
}

INSTANTIATE_TEST_SUITE_P(string_kind_and_ui_state, lua_dialog_settings_disk,
						 testing::Combine(testing::Values(StringControl::Edit, StringControl::Textbox, StringControl::Dropdown), testing::Bool()));
