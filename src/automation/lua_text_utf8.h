#pragma once

#include <cstddef>
#include <string_view>

namespace Automation4 {
inline size_t Utf8SequenceLength(std::string_view value, size_t index) {
	auto const lead = static_cast<unsigned char>(value[index]);
	if (lead < 0x80)
		return 1;
	size_t length = 0;
	if (lead >= 0xc2 && lead <= 0xdf)
		length = 2;
	else if (lead >= 0xe0 && lead <= 0xef)
		length = 3;
	else if (lead >= 0xf0 && lead <= 0xf4)
		length = 4;
	if (!length || index + length > value.size())
		return 0;
	for (size_t i = 1; i < length; ++i) {
		auto const next = static_cast<unsigned char>(value[index + i]);
		if (next < 0x80 || next > 0xbf)
			return 0;
	}
	auto const second = static_cast<unsigned char>(value[index + 1]);
	if ((lead == 0xe0 && second < 0xa0) ||
		(lead == 0xed && second > 0x9f) ||
		(lead == 0xf0 && second < 0x90) ||
		(lead == 0xf4 && second > 0x8f))
		return 0;
	return length;
}

}
