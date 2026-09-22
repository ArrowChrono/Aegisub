#include "async_video_trace.h"

#include <atomic>
#include <chrono>

namespace aegisub::async_video_trace {
namespace {

std::atomic<Sink*> active_sink{nullptr};

}

void SetSink(Sink *sink) {
	active_sink.store(sink, std::memory_order_release);
}

void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) {
	if (auto *sink = active_sink.load(std::memory_order_acquire))
		sink->ObserveFrameResult(frame, time, delivered, immediate);
}

void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms) {
	if (auto *sink = active_sink.load(std::memory_order_acquire))
		sink->ObserveVideoFrameRenderDuration(frame, time, delivered, immediate, duration_ms);
}

void ObservePipelineEvent(PipelineEvent const& event) {
	if (auto *sink = active_sink.load(std::memory_order_acquire))
		sink->ObservePipelineEvent(event);
}

std::int64_t CaptureTimestamp() noexcept {
	if (!active_sink.load(std::memory_order_acquire))
		return 0;
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
			   std::chrono::steady_clock::now().time_since_epoch())
		.count();
}
}

