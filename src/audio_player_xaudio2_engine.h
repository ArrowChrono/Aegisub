#pragma once

#include <atomic>

#ifndef XAUDIO2_REDIST
#include <xaudio2.h>
#else
#include <xaudio2redist.h>
#endif

class XAudio2EngineCallback final : public IXAudio2EngineCallback {
	std::atomic<HRESULT>& critical_error;
	HANDLE error_event;

	public:
	XAudio2EngineCallback(std::atomic<HRESULT>& critical_error, HANDLE error_event)
		: critical_error(critical_error), error_event(error_event) {}

	void STDMETHODCALLTYPE OnProcessingPassStart() override {}
	void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
	void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override {
		critical_error.store(error, std::memory_order_release);
		SetEvent(error_event);
	}

	bool CheckFailure(HRESULT result) {
		if (result == XAUDIO2_E_DEVICE_INVALIDATED)
			OnCriticalError(result);
		return FAILED(result);
	}
};
