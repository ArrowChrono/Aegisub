#pragma once

#include <memory>
#include <span>

class AssDialogue;
class AssDialogueBlock;

namespace aegisub::visual_shear {

[[nodiscard]] bool IsStatic(std::span<std::unique_ptr<AssDialogueBlock> const> blocks);
[[nodiscard]] bool IsStatic(AssDialogue const& line);

}
