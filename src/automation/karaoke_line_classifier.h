#pragma once

#include <cstdint>
#include <string_view>

namespace Automation4 {
enum class KaraokeLineKind : std::uint8_t { None,
											Code,
											Template };
enum KaraokeCodeScope : std::uint8_t {
	KaraokeOnce = 1,
	KaraokeLine = 2,
	KaraokeSyllable = 4,
	KaraokeFurigana = 8
};

struct KaraokeLineClassification {
	KaraokeLineKind kind = KaraokeLineKind::None;
	unsigned scopes = 0;
	bool all_styles = false;
	bool no_blank = false;
	double repeat = 1;
};

KaraokeLineClassification ClassifyKaraokeLine(bool comment, std::string_view effect);
bool IsKaraokeCodeLine(bool comment, std::string_view effect);
}
