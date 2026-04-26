#include "audioresampler.h"
extern "C" {
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}
#include <iostream>
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
		delete inChLayout;
		inChLayout = nullptr;
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
		delete inChLayout;
	}
	inChLayout = new AVChannelLayout();
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
	tempBuffer.resize(outSamples * sampleSize);

	uint8_t *data[2] = { 0 };
	data[0] = tempBuffer.data();
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

		double step = curSpeed;
		double pos = 0.0;
		int dstPos = 0;

		while (pos < re && dstPos < finalSamples) {
			int srcIndex = (int)pos;
			if (srcIndex >= re) break;

			memcpy(d + dstPos * sampleSize,
			       tempBuffer.data() + srcIndex * sampleSize,
			       sampleSize);

			dstPos++;
			pos += step;
		}
		finalSamples = dstPos;
	} else {
		int copyBytes = re * sampleSize;
		if (copyBytes > dataSize) copyBytes = dataSize;
		memcpy(d, tempBuffer.data(), copyBytes);
	}

	av_frame_free(&indata);
	return finalSamples * sampleSize;
}
