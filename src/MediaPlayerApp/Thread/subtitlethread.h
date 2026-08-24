#pragma once

#include "decodethread.h"
#include "isubtitlecallback.h"
#include <atomic>
#include <mutex>

struct AVCodecParameters;

class SubtitleThread : public DecodeThread
{
public:
	SubtitleThread();
	~SubtitleThread() override;

	bool Open(AVCodecParameters *para);
	void Close() override;
	void Clear() override;
	void run() override;

	void SetCallback(ISubtitleCallback *cb);
	void SetPause(bool p);
	std::atomic_bool isPause = {false};

private:
	std::mutex smux_;
	ISubtitleCallback *callback_ = nullptr;
	void emitCue(const QString &text, long long startMs, long long endMs);
};
