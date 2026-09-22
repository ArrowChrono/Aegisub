// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "async_video_provider_host.h"
#include "async_video_trace.h"

#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <utility>

namespace {
aegisub::async_video_trace::PipelineEvent PacketTraceEvent(
	char const *stage, VideoRenderPacket const& packet) {
	return {
		.stage = stage,
		.version = packet.delivery_version,
		.delivery_class = packet.delivery_class,
		.visual_interaction_id = packet.visual_interaction_id,
		.frame = packet.frame_number,
		.timestamp_ns = aegisub::async_video_trace::CaptureTimestamp()};
}

struct PendingFrameDelivery {
	VideoRenderPacket packet;
	double time = 0.0;
};

struct VisualDeliveryBatch {
	std::optional<PendingFrameDelivery> packet;
};

struct VisualBatchDeliveryState {
	std::mutex mutex;
	std::shared_ptr<VisualDeliveryBatch> open_visual_batch;
	std::uint64_t visual_interaction_id = 0;
	bool has_visual_interaction = false;
	bool visual_interaction_final = false;
	agi::ui::WeakLifetime lifetime;
	std::function<void(VideoRenderPacket, double)> callback;
};

void QueueVisualBatchDelivery(
	std::shared_ptr<VisualBatchDeliveryState> const& state,
	std::shared_ptr<VisualDeliveryBatch> const& batch) {
	agi::ui::MainAsyncIfAlive(state->lifetime, [state, batch] {
		std::optional<PendingFrameDelivery> pending;
		aegisub::async_video_trace::PipelineEvent dequeued;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			pending = std::move(batch->packet);
			batch->packet.reset();
			if (state->open_visual_batch == batch)
				state->open_visual_batch.reset();
			if (pending)
				dequeued = PacketTraceEvent("host_dequeue", pending->packet);
		}

		if (pending) {
			aegisub::async_video_trace::ObservePipelineEvent(dequeued);
			state->callback(std::move(pending->packet), pending->time);
		}
	});
}

void QueueEveryFrameDelivery(
	std::shared_ptr<VisualBatchDeliveryState> const& state,
	VideoRenderPacket packet,
	double time) {
	auto const enqueued = PacketTraceEvent("host_enqueue", packet);
	agi::ui::MainAsyncIfAlive(state->lifetime, [state, packet = std::move(packet), time]() mutable {
		aegisub::async_video_trace::ObservePipelineEvent(PacketTraceEvent("host_dequeue", packet));
		state->callback(std::move(packet), time);
	});
	aegisub::async_video_trace::ObservePipelineEvent(enqueued);
}
}

AsyncVideoProviderEventSink CreateAsyncVideoProviderMainThreadSink(
	agi::ui::WeakLifetime event_lifetime,
	AsyncVideoProviderEventSink sink,
	AsyncVideoFrameDeliveryMode frame_delivery_mode) {
	AsyncVideoProviderEventSink main_thread_sink;

	if (sink.on_frame_ready) {
		if (frame_delivery_mode == AsyncVideoFrameDeliveryMode::VisualSubtitleBatches) {
			auto state = std::make_shared<VisualBatchDeliveryState>();
			state->lifetime = event_lifetime;
			state->callback = std::move(sink.on_frame_ready);
			main_thread_sink.on_frame_ready = [state](VideoRenderPacket packet, double time) {
				if (packet.delivery_class == VideoRenderDeliveryClass::EveryFrame) {
					// EveryFrame is deliberately kept on the original one-task-per-packet
					// path. Closing the current visual batch before queuing this callback
					// makes the main-thread FIFO a sequence barrier without storing normal
					// packets in a second queue.
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						state->open_visual_batch.reset();
					}
					QueueEveryFrameDelivery(state, std::move(packet), time);
					return;
				}

				std::shared_ptr<VisualDeliveryBatch> batch_to_schedule;
				aegisub::async_video_trace::PipelineEvent enqueued;
				aegisub::async_video_trace::PipelineEvent replaced;
				{
					std::unique_lock<std::mutex> lock(state->mutex);
					std::uint64_t const interaction_id = packet.visual_interaction_id;
					if (state->has_visual_interaction
						&& interaction_id < state->visual_interaction_id) {
						// A result from an older interaction cannot overwrite the
						// latest interaction's state.
						auto const dropped = PacketTraceEvent("host_drop_old_interaction", packet);
						lock.unlock();
						aegisub::async_video_trace::ObservePipelineEvent(dropped);
						return;
					}
					if (state->has_visual_interaction
						&& interaction_id == state->visual_interaction_id
						&& state->visual_interaction_final
						&& packet.delivery_class == VideoRenderDeliveryClass::VisualSubtitleIntermediate) {
						// A final packet closes this interaction. Do not let a late worker
						// intermediate overwrite it.
						auto const dropped = PacketTraceEvent("host_drop_closed", packet);
						lock.unlock();
						aegisub::async_video_trace::ObservePipelineEvent(dropped);
						return;
					}

					if (!state->has_visual_interaction || interaction_id != state->visual_interaction_id) {
						state->visual_interaction_id = interaction_id;
						state->has_visual_interaction = true;
						state->visual_interaction_final = false;
					}
					if (!state->open_visual_batch) {
						state->open_visual_batch = std::make_shared<VisualDeliveryBatch>();
						batch_to_schedule = state->open_visual_batch;
					}
					if (state->open_visual_batch->packet)
						replaced = PacketTraceEvent("host_replace", state->open_visual_batch->packet->packet);
					enqueued = PacketTraceEvent("host_enqueue", packet);
					state->open_visual_batch->packet = PendingFrameDelivery{std::move(packet), time};
					if (state->open_visual_batch->packet->packet.delivery_class == VideoRenderDeliveryClass::VisualSubtitleFinal)
						state->visual_interaction_final = true;
				}
				if (batch_to_schedule)
					QueueVisualBatchDelivery(state, batch_to_schedule);
				if (replaced.stage)
					aegisub::async_video_trace::ObservePipelineEvent(replaced);
				aegisub::async_video_trace::ObservePipelineEvent(enqueued);
			};
		}
		else {
			main_thread_sink.on_frame_ready =
				[event_lifetime, callback = std::move(sink.on_frame_ready)](VideoRenderPacket packet, double time) mutable {
					auto const enqueued = PacketTraceEvent("host_enqueue", packet);
					agi::ui::MainAsyncIfAlive(event_lifetime, [callback, packet = std::move(packet), time]() mutable {
						aegisub::async_video_trace::ObservePipelineEvent(PacketTraceEvent("host_dequeue", packet));
						callback(std::move(packet), time);
					});
					aegisub::async_video_trace::ObservePipelineEvent(enqueued);
				};
		}
	}

	if (sink.on_video_error) {
		main_thread_sink.on_video_error =
			[event_lifetime, callback = std::move(sink.on_video_error)](std::string const& message) mutable {
				agi::ui::MainAsyncIfAlive(event_lifetime, [callback, message]() mutable {
					callback(message);
				});
			};
	}

	if (sink.on_subtitles_error) {
		main_thread_sink.on_subtitles_error =
			[event_lifetime, callback = std::move(sink.on_subtitles_error)](std::string const& message) mutable {
				agi::ui::MainAsyncIfAlive(event_lifetime, [callback, message]() mutable {
					callback(message);
				});
			};
	}

	return main_thread_sink;
}
