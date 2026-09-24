#pragma once

#include "visual_tool_render_snapshot.h"

#include <cstdint>
#include <memory>
#include <utility>

// Retain only the baseline which was visible when an interaction began. New
// frames carry their own snapshots; this state does not cache packet history.
class VisualToolPresentation final {
	std::shared_ptr<const VisualToolRenderContext> context =
		std::make_shared<const VisualToolRenderContext>();
	std::shared_ptr<const VisualToolRenderSnapshot> baseline;
	std::uint64_t interaction_id = 0;

	public:
	[[nodiscard]] std::shared_ptr<const VisualToolRenderContext> const& Context() const noexcept {
		return context;
	}

	[[nodiscard]] bool IsActive() const noexcept { return interaction_id != 0; }
	[[nodiscard]] std::uint64_t InteractionId() const noexcept { return interaction_id; }

	[[nodiscard]] bool Matches(std::shared_ptr<const VisualToolRenderSnapshot> const& snapshot) const noexcept {
		return snapshot && snapshot->context == context;
	}

	[[nodiscard]] bool MatchesActiveInteraction(std::uint64_t id, std::shared_ptr<const VisualToolRenderSnapshot> const& snapshot) const noexcept {
		return IsActive() && id == interaction_id && Matches(snapshot);
	}

	bool Begin(std::uint64_t id, std::shared_ptr<const VisualToolRenderSnapshot> snapshot) noexcept {
		if (!id || !Matches(snapshot))
			return false;
		baseline = std::move(snapshot);
		interaction_id = id;
		return true;
	}

	[[nodiscard]] std::shared_ptr<const VisualToolRenderSnapshot> Select(
		std::shared_ptr<const VisualToolRenderSnapshot> const& packet_snapshot) const noexcept {
		if (!IsActive())
			return {};
		return Matches(packet_snapshot) ? packet_snapshot : baseline;
	}

	bool OnFinalPresented(std::uint64_t id) {
		if (!IsActive() || id != interaction_id)
			return false;
		Reset();
		return true;
	}

	void Reset() {
		context = std::make_shared<const VisualToolRenderContext>();
		baseline.reset();
		interaction_id = 0;
	}
};
