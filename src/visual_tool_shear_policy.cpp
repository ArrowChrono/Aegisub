#include "visual_tool_shear_policy.h"

#include "ass_dialogue.h"
#include "ass_tag_scanner.h"

#include <cmath>
#include <string_view>

namespace aegisub::visual_shear {
namespace {
bool HasAnimatedShear(std::string_view body, unsigned depth = 0) {
	using namespace aegisub::ass_tag_scanner;
	bool animated = false;
	ScanRawTags(body, [&](RawTag const& tag) {
		if (depth && (NameHasPrefix(tag.name, "fax") || NameHasPrefix(tag.name, "fay")))
			animated = true;
		if (NameHasPrefix(tag.name, "t") && tag.has_paren)
			animated |= depth >= 32 || HasAnimatedShear(tag.args, depth + 1);
	});
	return animated;
}
}

bool IsStatic(std::span<std::unique_ptr<AssDialogueBlock> const> blocks) {
	using namespace aegisub::ass_tag_scanner;
	bool fax_seen = false;
	bool fay_seen = false;
	for (std::size_t index = 0; index < blocks.size(); ++index) {
		auto const& block = blocks[index];
		if (block->GetType() != AssBlockType::OVERRIDE) {
			continue;
		}
		auto const& body = block->GetRawText();
		if (HasAnimatedShear(body)) return false;
		bool unsupported = false;
		ScanRawTags(body, [&](RawTag const& tag) {
			bool const fax = NameHasPrefix(tag.name, "fax");
			bool const fay = NameHasPrefix(tag.name, "fay");
			bool const shear = fax || fay;
			bool const reset = NameHasPrefix(tag.name, "r");
			if ((index != 0 && (shear || reset)) || (reset && (fax_seen || fay_seen)))
				unsupported = true;
			if ((fax && fax_seen) || (fay && fay_seen))
				unsupported = true;
			if (shear && (tag.has_paren || (!tag.bytes.starts_with("\\fax") && !tag.bytes.starts_with("\\fay"))))
				unsupported = true;
			if (shear && !std::isfinite(static_cast<float>(ArgToDouble(tag.name.substr(3)))))
				unsupported = true;
			fax_seen |= fax;
			fay_seen |= fay;
		});
		if (unsupported) return false;
	}
	return true;
}

bool IsStatic(AssDialogue const& line) {
	auto const blocks = line.ParseTags();
	return IsStatic(blocks);
}
}
