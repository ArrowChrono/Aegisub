#pragma once

#include <libaegisub/ass/time.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/io.h>

#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisub {
struct ShiftHistoryEntry {
	std::string filename;
	int amount = 0;
	bool by_time = true;
	bool backward = false;
	int fields = 0;
	int mode = 0;
	std::vector<std::pair<int, int>> selected_blocks;
};
using ShiftHistory = std::vector<ShiftHistoryEntry>;

namespace shift_history_detail {
inline json::UnknownElement const& Field(json::Object const& object, char const *name) {
	auto found = object.find(name);
	if (found == object.end())
		throw json::Exception(std::string("Missing shift history field: ") + name);
	return found->second;
}

inline int Integer(json::UnknownElement const& value) {
	auto number = static_cast<int64_t>(value);
	if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
		throw json::Exception("Shift history integer exceeds the supported range");
	return static_cast<int>(number);
}

inline int ParseAmount(std::string_view text, bool by_time) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
		text.remove_suffix(1);
	if (!by_time) {
		if (!text.empty() && text.front() == '+')
			text.remove_prefix(1);
		if (text.empty())
			throw json::Exception("Invalid shift history frame amount");
		int value = 0;
		auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw json::Exception("Invalid shift history frame amount");
		return value;
	}
	auto decimal = [](std::string_view part) {
		int value = 0;
		if (part.empty() || part.find_first_not_of("0123456789") != std::string_view::npos)
			throw json::Exception("Invalid shift history time amount");
		auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
		if (parsed.ec != std::errc{})
			throw json::Exception("Shift history time exceeds the supported range");
		return value;
	};
	int fraction = 0;
	auto dot = text.find_first_of(".,");
	if (dot != std::string_view::npos) {
		auto digits = text.substr(dot + 1);
		if (digits.size() > 3)
			throw json::Exception("Shift history time has more than three decimal places");
		fraction = decimal(digits);
		for (auto count = digits.size(); count < 3; ++count)
			fraction *= 10;
		text = text.substr(0, dot);
	}
	int64_t seconds = 0;
	auto first = text.find(':');
	if (first == std::string_view::npos)
		seconds = decimal(text);
	else {
		auto second = text.find(':', first + 1);
		if (second == std::string_view::npos)
			throw json::Exception("Shift history time requires hours, minutes and seconds");
		auto hours = decimal(text.substr(0, first));
		auto minutes = decimal(text.substr(first + 1, second - first - 1));
		auto last = decimal(text.substr(second + 1));
		if (minutes >= 60 || last >= 60)
			throw json::Exception("Invalid shift history minute or second component");
		seconds = (static_cast<int64_t>(hours) * 60 + minutes) * 60 + last;
	}
	auto milliseconds = seconds * 1000 + fraction;
	if (milliseconds > std::numeric_limits<int>::max())
		throw json::Exception("Shift history time exceeds the supported range");
	return static_cast<int>(agi::Time(static_cast<int>(milliseconds)));
}

inline void Validate(ShiftHistoryEntry const& entry) {
	if (entry.fields < 0 || entry.fields > 2 || entry.mode < 0 || entry.mode > 2 ||
		(entry.by_time && (entry.amount < 0 || static_cast<int>(agi::Time(entry.amount)) != entry.amount)))
		throw json::Exception("Invalid shift history settings");
	int previous = 0;
	for (auto const& [start, end] : entry.selected_blocks) {
		if (start <= previous || end < start)
			throw json::Exception("Invalid shift history selected range");
		previous = end;
	}
}
}

inline std::string FormatShiftHistoryAmount(ShiftHistoryEntry const& entry) {
	return entry.by_time ? agi::Time(entry.amount).GetAssFormatted(entry.amount % 10 != 0) : std::to_string(entry.amount);
}

inline ShiftHistory DecodeShiftHistory(json::UnknownElement const& root) {
	ShiftHistory result;
	for (auto const& value : static_cast<json::Array const&>(root)) {
		auto const& object = static_cast<json::Object const&>(value);
		ShiftHistoryEntry entry;
		entry.filename = static_cast<std::string const&>(shift_history_detail::Field(object, "filename"));
		entry.by_time = static_cast<bool>(shift_history_detail::Field(object, "is by time"));
		entry.backward = static_cast<bool>(shift_history_detail::Field(object, "is backward"));
		entry.amount = shift_history_detail::ParseAmount(static_cast<std::string const&>(shift_history_detail::Field(object, "amount")), entry.by_time);
		entry.fields = shift_history_detail::Integer(shift_history_detail::Field(object, "fields"));
		entry.mode = shift_history_detail::Integer(shift_history_detail::Field(object, "mode"));
		for (auto const& block : static_cast<json::Array const&>(shift_history_detail::Field(object, "selection"))) {
			auto const& range = static_cast<json::Object const&>(block);
			entry.selected_blocks.emplace_back(shift_history_detail::Integer(shift_history_detail::Field(range, "start")), shift_history_detail::Integer(shift_history_detail::Field(range, "end")));
		}
		shift_history_detail::Validate(entry);
		result.push_back(std::move(entry));
	}
	return result;
}

inline json::Array EncodeShiftHistory(ShiftHistory const& history) {
	json::Array result;
	for (auto const& entry : history) {
		shift_history_detail::Validate(entry);
		json::Object object;
		object["filename"] = entry.filename;
		object["amount"] = FormatShiftHistoryAmount(entry);
		object["is by time"] = entry.by_time;
		object["is backward"] = entry.backward;
		object["fields"] = entry.fields;
		object["mode"] = entry.mode;
		json::Array selection;
		for (auto const& [start, end] : entry.selected_blocks) {
			json::Object block;
			block["start"] = start;
			block["end"] = end;
			selection.emplace_back(std::move(block));
		}
		object["selection"] = std::move(selection);
		result.emplace_back(std::move(object));
	}
	return result;
}

inline void WriteShiftHistory(agi::fs::path const& filename, ShiftHistory const& history) {
	auto encoded = EncodeShiftHistory(history);
	agi::io::Save output(filename);
	try {
		agi::JsonWriter::Write(encoded, output.Get());
		output.Close();
	}
	catch (...) {
		output.Cancel();
		throw;
	}
}
}
