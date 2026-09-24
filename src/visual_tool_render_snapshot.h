#pragma once

#include <memory>
#include <optional>
#include <cstddef>
#include <utility>

class VideoOverlayDrawContext;
class Vector2D;

// Identity of one display/tool coordinate context. Snapshots own only CPU
// values: their last reference may be released on the subtitle worker.
struct VisualToolRenderContext final {};

struct VisualToolFeatureKey {
	std::optional<int> line_id;
	int type = 0;
	std::size_t index = 0;
};

class VisualToolRenderSnapshot {
	public:
	explicit VisualToolRenderSnapshot(std::shared_ptr<const VisualToolRenderContext> context)
		: context(std::move(context)) {}
	virtual ~VisualToolRenderSnapshot() = default;

	std::shared_ptr<const VisualToolRenderContext> const context;
	virtual void Draw(VideoOverlayDrawContext& target) const = 0;
	[[nodiscard]] virtual std::optional<VisualToolFeatureKey> HitTest(Vector2D const&) const { return {}; }
	virtual void DrawLiveFeedback(Vector2D const&) const {}
	[[nodiscard]] virtual bool HasMouseDrivenFeedback() const { return false; }
};
