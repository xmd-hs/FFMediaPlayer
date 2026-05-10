#pragma once

struct AVCodecParameters;
struct AVFrame;
struct SwrContext;
struct AVChannelLayout;
#include <mutex>
#include <atomic>
extern "C" {
#include <libavutil/samplefmt.h>
}

class AudioResampler
{
public:
	bool Open(AVCodecParameters *para, bool isClearPara = false);
	void Close();
	int Resample(AVFrame *indata, unsigned char *data, int dataSize);
	bool SetSpeed(double speed);
	AudioResampler();
	~AudioResampler();

	AVSampleFormat outFormat = AV_SAMPLE_FMT_S16;

protected:
	std::mutex mux;
	SwrContext *actx = nullptr;
	int outSampleRate = 44100;
	int inSampleRate = 44100;
	AVChannelLayout *inChLayout = nullptr;
	AVSampleFormat inFmt = AV_SAMPLE_FMT_S16;
	double curSpeed = 1.0;
	unsigned char *tempBuf_ = nullptr;
	size_t tempBufSize_ = 0;
	bool rebuildCtx();
};
