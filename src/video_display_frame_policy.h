#pragma once

inline bool ShouldIgnoreVideoDisplayFrameReady(
	bool free_size,
	bool is_current_display) noexcept {
	return !free_size && !is_current_display;
}

// A paired tool snapshot stays unchanged until a new picture is ready.
// Explicit redraws and independent feedback (guides, visibility, etc.) remain
// useful even then. Live tools outside this scope retain their normal cadence.
// Mouse-driven feedback (e.g. a live cursor ray) changes the picture without
// any packet, so it must keep its paced budget.
inline bool ShouldRenderVideoDisplayInteraction(
	bool paired_snapshot,
	bool last_render_succeeded,
	bool uploadable_packet,
	bool render_requested,
	bool feedback_ready,
	bool mouse_driven_feedback) noexcept {
	return !paired_snapshot || !last_render_succeeded || uploadable_packet || render_requested || feedback_ready || mouse_driven_feedback;
}
