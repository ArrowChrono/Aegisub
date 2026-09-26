#pragma once

#include "automation_runtime_trace_sink.h"

#include <cstdint>

namespace Automation4 {
enum class AutomationInvocationOutcome : std::uint8_t { Completed,
														Cancelled,
														Failed };

class AutomationInvocationObserver : public AutomationRuntimeTraceSink {
	public:
	virtual void OnInvocationFinished(AutomationInvocationOutcome outcome) = 0;
};
}
