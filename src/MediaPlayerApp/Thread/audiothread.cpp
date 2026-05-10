#include "audiothread.h"
#include "mediadecoder.h"
#include "audioplayer.h"
#include "audioresampler.h"
#include "MemoryPool.h"
#include <iostream>
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
}
using namespace std;

AudioThread::AudioThread()
{
}

AudioThread::~AudioThread() { Close(); }

void AudioThread::Clear()
{
	DecodeThread::Clear();
	std::lock_guard<std::mutex> lk(amux);
	if (ap) ap->Clear();
	audioStartPts = 0;
	baseMediaTime = 0;
	baseHwTime = 0;
	firstFrame = true;
}

void AudioThread::Close()
{
	DecodeThread::Close();
	std::lock_guard<std::mutex> lk(amux);
	if (ap) { ap->Close(); ap = nullptr; }
	if (res) { delete res; res = nullptr; }
}

bool AudioThread::Open(AVCodecParameters *para, int sampleRate, int channels)
{
	if (!para) return false;
	Clear();

	std::lock_guard<std::mutex> lk(amux);
	pts = 0;

	if (res) { delete res; }
	res = new AudioResampler();
	ap = AudioPlayer::Get();

	if (!res || !ap)
	{
		avcodec_parameters_free(&para);
		return false;
	}

	if (!res->Open(para, false))
	{
		cout << "[Audio] resampler open failed!" << endl;
		delete res; res = nullptr;
		avcodec_parameters_free(&para);
		return false;
	}

	ap->sampleRate = sampleRate;
	ap->channels = 2;
	ap->sampleSize = 16;
	if (!ap->Open())
	{
		cout << "[Audio] audio output open failed!" << endl;
		delete res; res = nullptr;
		avcodec_parameters_free(&para);
		return false;
	}

	if (!decode->Open(para))
	{
		cout << "[Audio] decoder open failed!" << endl;
		delete res; res = nullptr;
		ap->Close();
		return false;
	}

	cout << "[Audio] open OK sr=" << sampleRate << " ch=" << channels << endl;
	return true;
}

void AudioThread::SetPause(bool p)
{
	isPause = p;
	std::lock_guard<std::mutex> lk(amux);
	if (ap) ap->SetPause(p);
}

void AudioThread::SetSpeed(double s)
{
	if (s <= 0 || s > 8.0) return;
	std::lock_guard<std::mutex> lk(amux);
	if (firstFrame)
	{
		speed = s;
		if (res) res->SetSpeed(s);
		if (ap) ap->SetSpeed(s);
		return;
	}
	long long playedMs = 0;
	if (ap) playedMs = ap->GetPlayedMs();
	double oldSpeed = speed.load();
	long long hwDelta = playedMs - baseHwTime;
	if (hwDelta > 0)
		baseMediaTime += (long long)(hwDelta * oldSpeed);
	baseHwTime = playedMs;
	speed = s;
	if (res) res->SetSpeed(s);
	if (ap) ap->SetSpeed(s);
}

long long AudioThread::GetAudioClock()
{
	std::lock_guard<std::mutex> lk(amux);
	if (firstFrame)
	{
		return baseMediaTime;
	}
	long long playedMs = 0;
	if (ap) playedMs = ap->GetPlayedMs();
	double curSpeed = speed.load();
	long long hwDelta = playedMs - baseHwTime;
	if (curSpeed > 0) hwDelta = (long long)(hwDelta * curSpeed);
	return baseMediaTime + hwDelta;
}

void AudioThread::ResetClock(long long seekPts)
{
	std::lock_guard<std::mutex> lk(amux);
	baseMediaTime = seekPts;
	baseHwTime = 0;
	audioStartPts = seekPts;
	firstFrame = true;
}

void AudioThread::run()
{
	const size_t PCM_BUFFER_SIZE = 256 * 1024; // MemoryPool MAX_BYTES
	unsigned char *pcm = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(PCM_BUFFER_SIZE);
	if (!pcm)
	{
		std::cerr << "[Audio] PCM buffer allocation failed!" << std::endl;
		return;
	}

	while (!isExit)
	{
		if (isPause) { msleep(5); continue; }

		AVPacket *pkt = Pop();
		if (!pkt) { msleep(1); continue; }

		bool hasRes, hasAp;
		{
			std::lock_guard<std::mutex> lk(amux);
			hasRes = (res != nullptr);
			hasAp = (ap != nullptr);
		}

		if (!hasRes || !hasAp)
		{
			RecyclePacket(pkt);
			msleep(1);
			continue;
		}

		if (!decode->Send(pkt))
		{
			RecyclePacket(pkt);
			continue;
		}

		while (!isExit)
		{
			AVFrame *frame = decode->Recv();
			if (!frame) break;

			if (firstFrame)
			{
				std::lock_guard<std::mutex> lk(amux);
				if (frame->pts > 0)
				{
					baseMediaTime = frame->pts;
					audioStartPts = frame->pts;
				}
				if (ap) baseHwTime = ap->GetPlayedMs();
				double curSpeed = speed.load();
				if (res) res->SetSpeed(curSpeed);
				if (ap) ap->SetSpeed(curSpeed);
				firstFrame = false;
			}
			pts = frame->pts;

			int len = 0;
			amux.lock();
			if (res && ap) len = res->Resample(frame, pcm, (int)PCM_BUFFER_SIZE);
			else { av_frame_free(&frame); amux.unlock(); continue; }
			amux.unlock();

			if (len > 0)
			{
				for (int retry = 0; retry < 100 && !isExit; retry++)
				{
					amux.lock();
					int freeSpace = (ap ? ap->GetFree() : 0);
					amux.unlock();
					if (freeSpace >= len) break;
					msleep(2);
				}
				amux.lock();
				if (ap) ap->Write(pcm, len);
				amux.unlock();
			}
		}
		msleep(1);
	}

	Kama_memoryPool::MemoryPool::deallocate(pcm, PCM_BUFFER_SIZE);
}
