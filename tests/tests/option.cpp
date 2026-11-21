// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <libaegisub/fs.h>
#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <main.h>
#include <util.h>

#include <fstream>
#include <sstream>

static const char default_opt[] = "{\"Valid\" : \"This is valid\"}";
static const char all_types[] = R"raw({
	"Integer" : 0,
	"Double" : 0.1,
	"String" : "",
	"Color" : "rgb(0, 0, 0)",
	"Boolean" : false,
	"Array" : {
		"Integer" : [{ "int" : 0 }, {"int" : 0}],
		"Double" : [{ "double" : 0.1 }, {"double" : 0.1}],
		"String" : [{ "string" : "" }, {"string" : ""}],
		"Color" : [{ "color" : "rgb(0,0,0)" }, {"color" : "rgb(0,0,0)"}],
		"Boolean" : [{ "bool" : false }, {"bool" : false}]
	}
})raw";

class lagi_option : public libagi {
protected:
	std::string conf_ok;

	void SetUp() override {
		conf_ok = "data/options/string.json";
	}
};

TEST_F(lagi_option, construct_from_file) {
	EXPECT_NO_THROW(agi::Options(conf_ok, default_opt, agi::Options::FLUSH_SKIP));
}

TEST_F(lagi_option, nonexistent_file) {
	EXPECT_NO_THROW(agi::Options("", default_opt, agi::Options::FLUSH_SKIP));
}

TEST_F(lagi_option, get_existing_option) {
	agi::Options opt(conf_ok, default_opt, agi::Options::FLUSH_SKIP);
	ASSERT_NO_THROW(opt.Get("Valid"));
	ASSERT_NO_THROW(opt.Get("Valid")->GetString());
	EXPECT_STREQ("This is valid", opt.Get("Valid")->GetString().c_str());
}

TEST_F(lagi_option, get_nonexistant_option) {
	agi::Options opt(conf_ok, default_opt, agi::Options::FLUSH_SKIP);
	EXPECT_THROW(opt.Get("Nonexistant option"), agi::InternalError);
}

TEST_F(lagi_option, flush_skip) {
	agi::fs::Copy("data/options/string.json", "data/options/tmp");
	{
		agi::Options opt("data/options/tmp", default_opt, agi::Options::FLUSH_SKIP);
		ASSERT_NO_THROW(opt.Get("Valid")->SetString(""));
	}
	EXPECT_TRUE(util::compare("data/options/string.json", "data/options/tmp"));
}

TEST_F(lagi_option, flush_no_skip) {
	agi::fs::Copy("data/options/string.json", "data/options/tmp");
	{
		agi::Options opt("data/options/tmp", default_opt);
		ASSERT_NO_THROW(opt.Get("Valid")->SetString(""));
	}
	EXPECT_FALSE(util::compare("data/options/string.json", "data/options/tmp"));
}

TEST_F(lagi_option, existent_but_invalid_file_uses_default) {
	agi::Options opt("data/options/null.json", default_opt, agi::Options::FLUSH_SKIP);
	EXPECT_NO_THROW(opt.Get("Valid")->GetString());
	EXPECT_THROW(opt.Get("Null"), agi::Exception);
}

TEST_F(lagi_option, arrays) {
	agi::Options opt("", all_types, agi::Options::FLUSH_SKIP);

	EXPECT_NO_THROW(opt.Get("Array/String")->GetListString());
	EXPECT_NO_THROW(opt.Get("Array/Integer")->GetListInt());
	EXPECT_NO_THROW(opt.Get("Array/Double")->GetListDouble());
	EXPECT_NO_THROW(opt.Get("Array/Boolean")->GetListBool());
	EXPECT_NO_THROW(opt.Get("Array/Color")->GetListColor());
}

TEST_F(lagi_option, bad_default_throws_and_null_is_rejected) {
	EXPECT_THROW(agi::Options("", "{\"Null\" : null}", agi::Options::FLUSH_SKIP), agi::Exception);
}

TEST_F(lagi_option, nested_options) {
	const char conf[] = R"raw({ "a" : { "b" : { "c" : { "c" : "value" } } } })raw";
	ASSERT_NO_THROW(agi::Options("", conf, agi::Options::FLUSH_SKIP));
	agi::Options opt("", conf, agi::Options::FLUSH_SKIP);
	ASSERT_NO_THROW(opt.Get("a/b/c/c"));
	ASSERT_NO_THROW(opt.Get("a/b/c/c")->GetString());
	EXPECT_STREQ("value", opt.Get("a/b/c/c")->GetString().c_str());
}

TEST_F(lagi_option, heterogeneous_arrays_rejected) {
	EXPECT_NO_THROW(agi::Options("", "{ \"key\" : [ { \"bool\" : true }] }", agi::Options::FLUSH_SKIP));
	EXPECT_THROW(agi::Options("", "{ \"key\" : [ { \"bool\" : true }, { \"double\" : 1.0 } ] }", agi::Options::FLUSH_SKIP), agi::Exception);
}

TEST_F(lagi_option, set_works) {
	agi::Options opt("", all_types, agi::Options::FLUSH_SKIP);

	ASSERT_NO_THROW(opt.Get("Integer")->SetInt(1000));
	EXPECT_EQ(1000, opt.Get("Integer")->GetInt());

	ASSERT_NO_THROW(opt.Get("String")->SetString("Hello"));
	EXPECT_EQ("Hello", opt.Get("String")->GetString());
}

TEST_F(lagi_option, flush_roundtrip) {
	agi::fs::Remove("data/options/tmp");

	{
		agi::Options opt("data/options/tmp", all_types);
		EXPECT_NO_THROW(opt.Get("Integer")->SetInt(1));
		EXPECT_NO_THROW(opt.Get("Double")->SetDouble(1.1));
		EXPECT_NO_THROW(opt.Get("String")->SetString("hello"));
		EXPECT_NO_THROW(opt.Get("Color")->SetColor(agi::Color("rgb(255,255,255)")));
		EXPECT_NO_THROW(opt.Get("Boolean")->SetBool(true));

		EXPECT_NO_THROW(opt.Get("Array/Integer")->SetListInt({1}));
		EXPECT_NO_THROW(opt.Get("Array/Double")->SetListDouble({1.1}));
		EXPECT_NO_THROW(opt.Get("Array/String")->SetListString({"hello"}));
		EXPECT_NO_THROW(opt.Get("Array/Color")->SetListColor({agi::Color("rgb(255,255,255)")}));
		EXPECT_NO_THROW(opt.Get("Array/Boolean")->SetListBool({true}));
	}

	{
		agi::Options opt("data/options/tmp", all_types);
		ASSERT_NO_THROW(opt.ConfigUser());

		EXPECT_EQ(1, opt.Get("Integer")->GetInt());
		EXPECT_EQ(1.1, opt.Get("Double")->GetDouble());
		EXPECT_STREQ("hello", opt.Get("String")->GetString().c_str());
		EXPECT_STREQ("rgb(255, 255, 255)", opt.Get("Color")->GetColor().GetRgbFormatted().c_str());
		EXPECT_EQ(true, opt.Get("Boolean")->GetBool());

		EXPECT_EQ(1, opt.Get("Array/Integer")->GetListInt().size());
		EXPECT_EQ(1, opt.Get("Array/Double")->GetListDouble().size());
		EXPECT_EQ(1, opt.Get("Array/String")->GetListString().size());
		EXPECT_EQ(1, opt.Get("Array/Color")->GetListColor().size());
		EXPECT_EQ(1, opt.Get("Array/Boolean")->GetListBool().size());

		EXPECT_EQ(1, opt.Get("Array/Integer")->GetListInt().front());
		EXPECT_EQ(1.1, opt.Get("Array/Double")->GetListDouble().front());
		EXPECT_STREQ("hello", opt.Get("Array/String")->GetListString().front().c_str());
		EXPECT_STREQ("rgb(255, 255, 255)", opt.Get("Array/Color")->GetListColor().front().GetRgbFormatted().c_str());
		EXPECT_EQ(true, opt.Get("Array/Boolean")->GetListBool().front());
	}
}

TEST_F(lagi_option, mixed_valid_and_invalid_in_user_conf_loads_all_valid) {
	const char def[] = "{\"1\" : false, \"2\" : 1, \"3\" : false }";
	agi::Options opt("data/options/all_bool.json", def, agi::Options::FLUSH_SKIP);
	ASSERT_NO_THROW(opt.ConfigUser());
	EXPECT_EQ(true, opt.Get("1")->GetBool());
	EXPECT_EQ(1, opt.Get("2")->GetInt());
	EXPECT_EQ(true, opt.Get("3")->GetBool());
}

TEST_F(lagi_option, empty_object_works) {
	EXPECT_NO_THROW(agi::Options("", "{ \"obj\" : {} }", agi::Options::FLUSH_SKIP));
}

TEST_F(lagi_option, unknown_array_type) {
	EXPECT_THROW(agi::Options("", "{ \"arr\" : [ { \"float\" : 5.0 } ] }"), agi::Exception);
}

TEST_F(lagi_option, malformed_arrays) {
	EXPECT_THROW(agi::Options("", "{ \"arr\" : [ {} ] }"), agi::Exception);
	EXPECT_THROW(agi::Options("", "{ \"arr\" : [ { \"double\" : 5.0 }, {} ] }"), agi::Exception);
	EXPECT_THROW(agi::Options("", "{ \"arr\" : [ { \"double\" : 5.0, \"int\" : 5 } ] }"), agi::Exception);
}

TEST_F(lagi_option, int_vs_double) {
	agi::Options opt("", "{ \"int\" : 5, \"double\" : 5.0 }", agi::Options::FLUSH_SKIP);
	EXPECT_NO_THROW(opt.Get("int")->GetInt());
	EXPECT_NO_THROW(opt.Get("double")->GetDouble());
}

TEST_F(lagi_option, user_int_coerces_onto_default_double) {
	const char def[] = R"({"step" : 1.0})";
	{
		std::ofstream out("data/options/tmp");
		ASSERT_TRUE(out);
		out << R"({"step" : 3})";
	}
	agi::Options opt("data/options/tmp", def, agi::Options::FLUSH_SKIP);
	ASSERT_NO_THROW(opt.ConfigUser());
	EXPECT_DOUBLE_EQ(3.0, opt.Get("step")->GetDouble());
}

TEST_F(lagi_option, empty_lists_survive_a_disk_round_trip) {
	const auto path = "data/options/empty-lists-round-trip.json";
	{
		agi::Options opt(path, all_types, agi::Options::FLUSH_SKIP);
		opt.Get("Array/String")->SetListString({});
		opt.Get("Array/Integer")->SetListInt({});
		opt.Get("Array/Double")->SetListDouble({});
		opt.Get("Array/Boolean")->SetListBool({});
		opt.Get("Array/Color")->SetListColor({});
		opt.Flush();
	}

	agi::Options opt(path, all_types, agi::Options::FLUSH_SKIP);
	opt.ConfigUser();
	EXPECT_TRUE(opt.Get("Array/String")->GetListString().empty());
	EXPECT_TRUE(opt.Get("Array/Integer")->GetListInt().empty());
	EXPECT_TRUE(opt.Get("Array/Double")->GetListDouble().empty());
	EXPECT_TRUE(opt.Get("Array/Boolean")->GetListBool().empty());
	EXPECT_TRUE(opt.Get("Array/Color")->GetListColor().empty());
	agi::fs::Remove(path);
}

TEST_F(lagi_option, empty_arrays_require_a_user_list_with_a_known_type) {
	const char path[] = "data/options/empty-arrays.json";
	const char empty[] = R"({"Valid":[],"Unknown":[]})";
	EXPECT_THROW(agi::Options("", empty, agi::Options::FLUSH_SKIP), agi::Exception);
	{
		std::ofstream out(path);
		ASSERT_TRUE(out);
		out << empty;
	}

	agi::Options opt(path, default_opt, agi::Options::FLUSH_SKIP);
	std::istringstream next(empty);
	EXPECT_THROW(opt.ConfigNext(next), agi::Exception);
	opt.ConfigUser();
	EXPECT_EQ("This is valid", opt.Get("Valid")->GetString());
	EXPECT_THROW(opt.Get("Unknown"), agi::InternalError);
	agi::fs::Remove(path);
}

TEST_F(lagi_option, commands_load_an_empty_list_and_migrate_a_legacy_string) {
	const char path[] = "data/options/empty-commands.json";
	const char key[] = "Subtitle/Edit Box/Command Buttons/Commands";
	const char defaults[] = R"({"Subtitle":{"Edit Box":{"Command Buttons":{"Commands":[{"string":"edit/font"}]}}}})";
	{
		std::ofstream out(path);
		ASSERT_TRUE(out);
		out << R"({"Subtitle":{"Edit Box":{"Command Buttons":{"Commands":"edit/font,edit/line/copy"}}}})";
	}
	{
		agi::Options opt(path, defaults, agi::Options::FLUSH_SKIP);
		opt.ConfigUser();
		EXPECT_EQ((std::vector<std::string>{"edit/font", "edit/line/copy"}), opt.Get(key)->GetListString());
		opt.Get(key)->SetListString({});
		opt.Flush();
	}

	agi::Options opt(path, defaults, agi::Options::FLUSH_SKIP);
	opt.ConfigUser();
	EXPECT_TRUE(opt.Get(key)->GetListString().empty());
	agi::fs::Remove(path);
}

#define CHECK_TYPE(str, type) \
	do { \
		agi::Options opt("", "{ \"" str "\" : \"" str "\" }", agi::Options::FLUSH_SKIP); \
		EXPECT_NO_THROW(opt.Get(str)->Get##type()); \
	} while (false)

TEST_F(lagi_option, color_vs_string) {
	CHECK_TYPE("#", String);
	CHECK_TYPE("#a", String);
	CHECK_TYPE("#abc", Color);
	CHECK_TYPE("#aabbcc", Color);
	CHECK_TYPE("#aabb", String);

	CHECK_TYPE("&", String);
	CHECK_TYPE("&H000000&", Color);
	CHECK_TYPE("&H00000000", Color);
}
