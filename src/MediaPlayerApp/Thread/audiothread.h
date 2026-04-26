#pragma once

struct AVCodecParameters;
class AudioPlayer;
class AudioResampler;
#include "decodethread.h"

class AudioThread : public DecodeThread
{
public:
	long long pts = 0;
	long long audioStartPts = 0;
	bool firstFrame = true;
	bool Open(AVCodecParameters *para, int sampleRate, int channels);
	void Close() override;
	void Clear() override;
	void run() override;
	AudioThread();
	~AudioThread() override;

	void SetPause(bool isPause);
	void SetSpeed(double speed);
	std::atomic_bool isPause = {false};
	std::atomic<double> speed = {1.0};
	AudioPlayer *ap = nullptr;

	long long GetAudioClock();
	void ResetClock(long long seekPts = 0);

protected:
	std::mutex amux;
	AudioResampler *res = nullptr;
	long long baseMediaTime = 0;
	long long baseHwTime = 0;
};
