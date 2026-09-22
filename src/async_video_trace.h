#pragma once

#include "video_render_packet.h"

namespace aegisub::async_video_trace {

struct PipelineEvent {
	char const *stage = nullptr;
	VideoRenderDeliveryVersion version;
	VideoRenderDeliveryClass delivery_class = VideoRenderDeliveryClass::EveryFrame;
	std::uint64_t visual_interaction_id = 0;
	int frame = -1;
	std::int64_t timestamp_ns = 0;
	double duration_ms = -1.0;
};

class Sink {
public:
	virtual ~Sink() = default;
	virtual void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) = 0;
	virtual void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms) = 0;
	virtual void ObservePipelineEvent(PipelineEvent const&) {}
};

void SetSink(Sink *sink);
void ObserveFrameResult(int frame, double time, bool delivered, bool immediate);
void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms);
void ObservePipelineEvent(PipelineEvent const& event);
std::int64_t CaptureTimestamp() noexcept;
}

