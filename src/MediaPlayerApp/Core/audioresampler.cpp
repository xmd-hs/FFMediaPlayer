#include "audioresampler.h"
#include "MemoryPool.h"
#include "GlobalThreadPool.h"
extern "C" {
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}
#include <iostream>
#include <vector>
using namespace std;

AudioResampler::AudioResampler() {}

AudioResampler::~AudioResampler() { Close(); }

void AudioResampler::Close()
{
	mux.lock();
	if (actx) swr_free(&actx);
	if (inChLayout)
	{
		av_channel_layout_uninit(inChLayout);
		Kama_memoryPool::MemoryPool::deallocate(inChLayout, sizeof(AVChannelLayout));
		inChLayout = nullptr;
	}
	if (tempBuf_ && tempBufSize_ > 0)
	{
		Kama_memoryPool::MemoryPool::deallocate(tempBuf_, tempBufSize_);
		tempBuf_ = nullptr;
		tempBufSize_ = 0;
	}
	mux.unlock();
}

bool AudioResampler::Open(AVCodecParameters *para, bool isClearPara)
{
	if (!para) return false;
	mux.lock();

	inSampleRate = para->sample_rate;
	inFmt = (AVSampleFormat)para->format;
	outSampleRate = para->sample_rate;
	curSpeed = 1.0;

	if (inChLayout)
	{
		av_channel_layout_uninit(inChLayout);
		Kama_memoryPool::MemoryPool::deallocate(inChLayout, sizeof(AVChannelLayout));
	}
	inChLayout = (AVChannelLayout*)Kama_memoryPool::MemoryPool::allocate(sizeof(AVChannelLayout));
	av_channel_layout_copy(inChLayout, &para->ch_layout);

	if (actx) swr_free(&actx);
	actx = swr_alloc();

	AVChannelLayout outCh;
	av_channel_layout_default(&outCh, 2);

	av_opt_set_chlayout(actx, "out_chlayout", &outCh, 0);
	av_opt_set_int(actx, "out_sample_rate", outSampleRate, 0);
	av_opt_set_sample_fmt(actx, "out_sample_fmt", outFormat, 0);
	av_opt_set_chlayout(actx, "in_chlayout", &para->ch_layout, 0);
	av_opt_set_int(actx, "in_sample_rate", para->sample_rate, 0);
	av_opt_set_sample_fmt(actx, "in_sample_fmt", (AVSampleFormat)para->format, 0);

	av_channel_layout_uninit(&outCh);

	if (isClearPara) avcodec_parameters_free(&para);

	int re = swr_init(actx);
	if (re != 0)
	{
		swr_free(&actx);
		mux.unlock();
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		cout << "swr_init failed: " << buf << endl;
		return false;
	}

	mux.unlock();
	return true;
}

bool AudioResampler::rebuildCtx()
{
	if (actx) swr_free(&actx);
	actx = swr_alloc();

	AVChannelLayout outCh;
	av_channel_layout_default(&outCh, 2);

	av_opt_set_chlayout(actx, "out_chlayout", &outCh, 0);
	av_opt_set_int(actx, "out_sample_rate", outSampleRate, 0);
	av_opt_set_sample_fmt(actx, "out_sample_fmt", outFormat, 0);

	if (inChLayout)
		av_opt_set_chlayout(actx, "in_chlayout", inChLayout, 0);
	else
	{
		AVChannelLayout defIn;
		av_channel_layout_default(&defIn, 2);
		av_opt_set_chlayout(actx, "in_chlayout", &defIn, 0);
		av_channel_layout_uninit(&defIn);
	}
	av_opt_set_int(actx, "in_sample_rate", inSampleRate, 0);
	av_opt_set_sample_fmt(actx, "in_sample_fmt", inFmt, 0);

	av_channel_layout_uninit(&outCh);

	int re = swr_init(actx);
	if (re != 0)
	{
		swr_free(&actx);
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		cout << "swr_init(speed) failed: " << buf << endl;
		return false;
	}
	cout << "Speed: " << curSpeed << "x" << endl;
	return true;
}

bool AudioResampler::SetSpeed(double s)
{
	if (s <= 0 || s > 8.0) return false;
	mux.lock();
	curSpeed = s;
	bool ok = rebuildCtx();
	mux.unlock();
	return ok;
}

int AudioResampler::Resample(AVFrame *indata, unsigned char *d, int dataSize)
{
	if (!indata) return 0;
	if (!d || dataSize <= 0) { av_frame_free(&indata); return 0; }

	mux.lock();
	if (!actx) { mux.unlock(); av_frame_free(&indata); return 0; }

	int outSamples = (int)(indata->nb_samples * (outSampleRate / (double)inSampleRate));
	if (outSamples < indata->nb_samples) outSamples = indata->nb_samples;

	int bytesPerSample = av_get_bytes_per_sample(outFormat);
	int sampleSize = 2 * bytesPerSample;
	size_t needSize = outSamples * sampleSize;

	if (needSize > tempBufSize_)
	{
		if (tempBuf_ && tempBufSize_ > 0)
			Kama_memoryPool::MemoryPool::deallocate(tempBuf_, tempBufSize_);
		tempBufSize_ = needSize;
		tempBuf_ = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(tempBufSize_);
		if (!tempBuf_)
		{
			tempBufSize_ = 0;
			mux.unlock();
			av_frame_free(&indata);
			return 0;
		}
	}

	uint8_t *data[2] = { 0 };
	data[0] = tempBuf_;
	int re = swr_convert(actx, data, outSamples,
		(const uint8_t**)indata->data, indata->nb_samples);
	mux.unlock();

	if (re <= 0) {
		av_frame_free(&indata);
		return 0;
	}

	int finalSamples = re;
	if (curSpeed != 1.0) {
		finalSamples = (int)(re / curSpeed);
		if (finalSamples < 1) finalSamples = 1;

		int maxSamples = dataSize / sampleSize;
		if (finalSamples > maxSamples) finalSamples = maxSamples;

		int maxValidDst = (int)((double)re / curSpeed) - 1;
		if (maxValidDst < 0) maxValidDst = 0;
		if (finalSamples > maxValidDst + 1) finalSamples = maxValidDst + 1;

		if (finalSamples > 512) {
			auto& pool = GlobalThreadPool::Instance();
			int numChunks = std::min((int)std::thread::hardware_concurrency(), (finalSamples + 255) / 256);
			if (numChunks < 2) numChunks = 2;
			int chunkSize = (finalSamples + numChunks - 1) / numChunks;
			double speed = curSpeed;
			unsigned char* srcBuf = tempBuf_;
			std::vector<std::future<void>> futures;
			for (int c = 0; c < numChunks; c++) {
				int startIdx = c * chunkSize;
				int endIdx = std::min(startIdx + chunkSize, finalSamples);
				futures.push_back(pool.submitTask([d, srcBuf, re, sampleSize, speed, startIdx, endIdx]() {
					for (int dstPos = startIdx; dstPos < endIdx; dstPos++) {
						int srcIndex = (int)(dstPos * speed);
						if (srcIndex >= re) break;
						memcpy(d + dstPos * sampleSize, srcBuf + srcIndex * sampleSize, sampleSize);
					}
				}));
			}
			for (auto& f : futures) f.get();
		} else {
			double step = curSpeed;
			double pos = 0.0;
			int dstPos = 0;
			while (pos < re && dstPos < finalSamples) {
				int srcIndex = (int)pos;
				if (srcIndex >= re) break;
				memcpy(d + dstPos * sampleSize, tempBuf_ + srcIndex * sampleSize, sampleSize);
				dstPos++;
				pos += step;
			}
			finalSamples = dstPos;
		}
	} else {
		int copyBytes = re * sampleSize;
		if (copyBytes > dataSize) copyBytes = dataSize;
		memcpy(d, tempBuf_, copyBytes);
	}

	av_frame_free(&indata);
	return finalSamples * sampleSize;
}
