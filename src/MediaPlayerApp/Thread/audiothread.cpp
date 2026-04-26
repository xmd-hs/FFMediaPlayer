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

AudioThread::AudioThread() {}

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
	ap->channels = channels;
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
	long long playedMs = 0;
	if (ap) playedMs = ap->GetPlayedMs();
	double oldSpeed = speed.load();
	long long hwDelta = playedMs - baseHwTime;
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
	const size_t PCM_BUFFER_SIZE = 1024 * 1024;
	unsigned char *pcm = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(PCM_BUFFER_SIZE);
	if (!pcm) return;

	while (!isExit)
	{
		if (isPause) { msleep(5); continue; }

		AVPacket *pkt = Pop();
		if (!pkt) { msleep(1); continue; }

		amux.lock();
		bool hasRes = (res != nullptr);
		bool hasAp = (ap != nullptr);
		amux.unlock();

		if (!hasRes || !hasAp)
		{
			av_packet_free(&pkt);
			msleep(1);
			continue;
		}

		if (!decode->Send(pkt))
		{
			msleep(1);
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
				firstFrame = false;
			}
			pts = frame->pts;

			int len = 0;
			amux.lock();
			if (res && ap) len = res->Resample(frame, pcm, (int)PCM_BUFFER_SIZE);
			else av_frame_free(&frame);
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
				bool writeOk = false;
				if (ap) writeOk = ap->Write(pcm, len);
				amux.unlock();
				if (!writeOk)
				{
					cout << "[Audio] Write failed, len=" << len << endl;
				}
			}
		}
		msleep(1);
	}

	Kama_memoryPool::MemoryPool::deallocate(pcm, PCM_BUFFER_SIZE);
}
