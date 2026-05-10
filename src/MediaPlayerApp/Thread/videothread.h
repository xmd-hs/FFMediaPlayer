#pragma once

struct AVPacket;
struct AVCodecParameters;
class MediaDecoder;
#include "ivideocallback.h"
#include "decodethread.h"
#include "GlobalThreadPool.h"
#include <chrono>
#include <atomic>
#include <future>

class VideoThread : public DecodeThread
{
public:
	bool RepaintPts(AVPacket *pkt, long long *outPts = nullptr);
	void ResetSync(long long seekPts = 0);
	bool Open(AVCodecParameters *para, IVideoCallback *call, int width, int height);
	void run() override;
	void Clear() override;
	void Close() override;
	VideoThread();
	~VideoThread() override;

	std::atomic<long long> synpts = {0};
	long long getPts();
	void SetPause(bool isPause);
	std::atomic_bool isPause = {false};
	std::atomic<double> speed = {1.0};

protected:
	IVideoCallback *call = nullptr;
	std::mutex vmux;
	std::chrono::steady_clock::time_point lastFrameTime;
	long long lastPts = 0;
	std::atomic_bool firstFrame = {true};
	void waitForFrame(long long framePts, double curSpeed);
	std::future<void> repaintFuture_;
};
