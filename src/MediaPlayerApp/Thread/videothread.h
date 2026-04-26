#pragma once

struct AVPacket;
struct AVCodecParameters;
class MediaDecoder;
#include "ivideocallback.h"
#include "decodethread.h"
#include <chrono>

class VideoThread : public DecodeThread
{
public:
	bool RepaintPts(AVPacket *pkt);
	void ResetSync(long long seekPts = 0);
	bool Open(AVCodecParameters *para, IVideoCallback *call, int width, int height);
	void run() override;
	void Clear() override;
	VideoThread();
	~VideoThread() override;

	long long synpts = 0;
	long long getPts();
	void SetPause(bool isPause);
	std::atomic_bool isPause = {false};
	std::atomic<double> speed = {1.0};

protected:
	IVideoCallback *call = nullptr;
	std::mutex vmux;
	std::chrono::steady_clock::time_point lastFrameTime;
	long long lastPts = 0;
	bool firstFrame = true;
};
