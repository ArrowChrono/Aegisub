#include <main.h>
#include "../../src/shift_times_history.h"
#include <libaegisub/cajun/reader.h>
#include <limits>
#include <sstream>

namespace {
aegisub::ShiftHistory Read(std::string const& amount, bool by_time = true) {
	std::istringstream input(R"([{"filename":"old.ass","amount":")" + amount + R"(","is by time":)" + (by_time ? "true" : "false") + R"(,"is backward":false,"fields":0,"mode":1,"selection":[{"start":2,"end":4}]}])");
	json::UnknownElement root;
	json::Reader::Read(root, input);
	return aegisub::DecodeShiftHistory(root);
}
}

TEST(shift_times_history, legacy_time_normalizes_to_actual_centisecond_shift) {
	EXPECT_EQ(1230, Read("00:00:01,234").front().amount);
	EXPECT_EQ(1240, Read("0:00:01.235").front().amount);
	EXPECT_EQ(2000, Read("2").front().amount);
	EXPECT_EQ(1100, Read("0:00:01.1").front().amount);
	EXPECT_EQ(60000, Read("0:00:59.995").front().amount);
	EXPECT_EQ(3600000, Read("0:59:59.995").front().amount);
}

TEST(shift_times_history, rejects_garbage_invalid_components_and_overflow) {
	for (auto const *text : {"", "1x", "0:60:00.00", "0:00:60.00", "-1", "0:00:01.2345", "596:31:23.648", "999999999999:00:00.00"})
		EXPECT_THROW(Read(text), json::Exception) << text;
	EXPECT_THROW(Read("2147483648", false), json::Exception);
	EXPECT_THROW(Read("-2147483649", false), json::Exception);
}

TEST(shift_times_history, preserves_signed_frame_range_and_canonicalizes_text) {
	EXPECT_EQ(12, Read("+0012", false).front().amount);
	EXPECT_EQ(-17, Read("-0017", false).front().amount);
	EXPECT_EQ(std::numeric_limits<int>::min(), Read("-2147483648", false).front().amount);
	EXPECT_EQ("12", aegisub::FormatShiftHistoryAmount(Read("+0012", false).front()));
}

TEST(shift_times_history, terminal_time_rounding_saturates_without_losing_value_on_write) {
	auto history = Read("596:31:23.647");
	EXPECT_EQ(std::numeric_limits<int>::max(), history.front().amount);
	EXPECT_EQ("596:31:23.647", aegisub::FormatShiftHistoryAmount(history.front()));
	EXPECT_EQ(std::numeric_limits<int>::max(), aegisub::DecodeShiftHistory(aegisub::EncodeShiftHistory(history)).front().amount);
}

TEST(shift_times_history, rejects_missing_null_and_bad_ranges_without_mutating_json) {
	std::istringstream input(R"([{"filename":"ok","amount":"0:00:01.00","is by time":true,"is backward":false,"fields":0,"mode":0,"selection":[]},{"amount":null}])");
	json::UnknownElement root;
	json::Reader::Read(root, input);
	EXPECT_THROW(aegisub::DecodeShiftHistory(root), json::Exception);
	auto const& entries = static_cast<json::Array const&>(root);
	EXPECT_THROW(static_cast<std::string const&>(static_cast<json::Object const&>(entries[1]).at("amount")), json::Exception);
	auto history = Read("0:00:01.00");
	history.front().selected_blocks = {{4, 2}};
	EXPECT_THROW(aegisub::EncodeShiftHistory(history), json::Exception);
}
