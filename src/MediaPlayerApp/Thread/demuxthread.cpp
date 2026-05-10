#include "demuxthread.h"
#include "mediademuxer.h"
#include "videothread.h"
#include "audiothread.h"
#include "audioplayer.h"
#include "GlobalThreadPool.h"
#include <iostream>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace std;

DemuxThread::DemuxThread()
{
}

DemuxThread::~DemuxThread()
{
	Close();
}

void DemuxThread::Clear()
{
	MediaDemuxer *curDemux = nullptr;
	VideoThread *curVt = nullptr;
	AudioThread *curAt = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		curDemux = demux;
		curVt = vt;
		curAt = at;
	}
	if (curDemux) curDemux->Clear();
	if (curVt) curVt->Clear();
	if (curAt) curAt->Clear();
}

void DemuxThread::Seek(double pos)
{
	if (pos < 0.0) pos = 0.0;
	if (pos > 1.0) pos = 1.0;
	seekPos_ = pos;
}

void DemuxThread::doSeek(double pos)
{
	MediaDemuxer *curDemux = nullptr;
	VideoThread *curVt = nullptr;
	AudioThread *curAt = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		if (!demux || !vt)
		{
			isPause = false;
			return;
		}
		curDemux = demux;
		curVt = vt;
		curAt = at;
	}

	bool wasPause = isPause.load();
	double curSpeed = speed_.load();
	isPause = true;
	if (curAt) curAt->SetPause(true);
	if (curVt) curVt->SetPause(true);

	{
		auto& pool = GlobalThreadPool::Instance();
		auto f1 = pool.submitTask([](VideoThread *t) { if(t) t->Clear(); }, curVt);
		auto f2 = pool.submitTask([](AudioThread *t) { if(t) t->Clear(); }, curAt);
		f1.get(); f2.get();
	}
	{
		std::lock_guard<std::mutex> lk(mux);
		if (demux) demux->Clear();
	}

	long long seekPts = (long long)(pos * totalMs);

	if (!curDemux->Seek(pos))
	{
		cout << "[Seek] failed" << endl;
		if (!wasPause)
		{
			isPause = false;
			pauseCv_.notify_one();
			if (curAt) curAt->SetPause(false);
			if (curVt) curVt->SetPause(false);
		}
		return;
	}

	if (curAt) curAt->ResetClock(seekPts);
	if (curVt) curVt->ResetSync(seekPts);
	pts = seekPts;

	// 立即更新进度条位置，避免显示旧值导致"先退后进"的闪烁
	if (curAt) {
		long long audioClock = curAt->GetAudioClock();
		if (audioClock > 0) pts = audioClock;
	}

	bool frameShown = false;
	long long actualPts = seekPts;
	int audioPreFill = 0;
	const int AUDIO_PRE_FILL_COUNT = 10;
	std::vector<AVPacket*> pendingAudio;
	std::vector<AVPacket*> pendingVideo;
	pendingAudio.reserve(80);
	pendingVideo.reserve(80);

	for (int i = 0; i < 80 && !isExit; i++)
	{
		if (seekPos_.load() >= 0.0) break;

		AVPacket *pkt = curDemux->Read();
		if (!pkt) { msleep(2); continue; }

		if (curDemux->IsAudio(pkt))
		{
			pendingAudio.push_back(pkt);
			audioPreFill++;
		}
		else
		{
			if (!frameShown)
			{
				long long decodedPts = 0;
				if (curVt->RepaintPts(pkt, &decodedPts))
				{
					frameShown = true;
					if (decodedPts > 0)
					{
						actualPts = decodedPts;
						pts = actualPts;
						if (curAt) curAt->ResetClock(actualPts);
						if (curVt) curVt->ResetSync(actualPts);
					}
				}
			}
			else
			{
				pendingVideo.push_back(pkt);
			}
		}

		if (frameShown && audioPreFill >= AUDIO_PRE_FILL_COUNT) break;
	}

	auto& pool = GlobalThreadPool::Instance();
	auto audioFuture = pool.submitTask([curAt, pendingAudio]() {
		for (auto* p : pendingAudio)
		{
			if (curAt) curAt->Push(p);
			else av_packet_free(&p);
		}
	});
	auto videoFuture = pool.submitTask([curVt, pendingVideo]() {
		for (auto* p : pendingVideo)
		{
			if (curVt) curVt->Push(p);
			else av_packet_free(&p);
		}
	});
	audioFuture.get();
	videoFuture.get();

	if (!wasPause)
	{
		isPause = false;
		pauseCv_.notify_one();
		if (curAt)
		{
			curAt->speed = curSpeed;
			curAt->SetSpeed(curSpeed);
			curAt->SetPause(false);
		}
		if (curVt)
		{
			curVt->speed = curSpeed;
			curVt->SetPause(false);
		}
	}
	else
	{
		if (curAt)
		{
			curAt->speed = curSpeed;
			curAt->SetSpeed(curSpeed);
		}
		if (curVt)
		{
			curVt->speed = curSpeed;
		}
	}
}

void DemuxThread::SetPause(bool p)
{
	isPause = p;
	if (!p) pauseCv_.notify_one();
	std::lock_guard<std::mutex> lk(mux);
	if (at) at->SetPause(p);
	if (vt) vt->SetPause(p);
}

void DemuxThread::SetVolume(int volume)
{
	volume_ = volume;
	std::lock_guard<std::mutex> lk(mux);
	if (at && at->ap) at->ap->SetVolume(volume);
}

int DemuxThread::GetVolume()
{
	std::lock_guard<std::mutex> lk(mux);
	int vol = 0;
	if (at && at->ap) vol = at->ap->GetVolume();
	return vol;
}

void DemuxThread::SetSpeed(double s)
{
	speed_ = s;
	std::lock_guard<std::mutex> lk(mux);
	if (vt) vt->speed = s;
	if (at) at->SetSpeed(s);
}
double DemuxThread::GetSpeed() { return speed_; }

long long DemuxThread::GetCurrentPts()
{
	std::lock_guard<std::mutex> lk(mux);
	long long curPts = 0;
	if (at)
	{
		curPts = at->GetAudioClock();
	}
	else if (vt)
	{
		curPts = vt->getPts();
	}
	if (curPts < 0) curPts = 0;
	pts = curPts;
	return curPts;
}

void DemuxThread::run()
{
	while (!isExit)
	{
		double pendingSeek = seekPos_.exchange(-1.0);
		if (pendingSeek >= 0.0)
		{
			doSeek(pendingSeek);
			continue;
		}

		if (isPause)
		{
			std::unique_lock<std::mutex> lk(pauseMux_);
			pauseCv_.wait_for(lk, std::chrono::milliseconds(50));
			continue;
		}

		MediaDemuxer *curDemux = nullptr;
		VideoThread *curVt = nullptr;
		AudioThread *curAt = nullptr;
		{
			std::lock_guard<std::mutex> lk(mux);
			curDemux = demux;
			curVt = vt;
			curAt = at;
		}
		if (!curDemux) { msleep(5); continue; }

		if (curAt)
		{
			long long curPts = curAt->GetAudioClock();
			pts = curPts;
			if (curVt) curVt->synpts = curPts;
		}
		else if (curVt)
		{
			pts = curVt->getPts();
		}

		if (curVt && curVt->GetPackCount() > curVt->maxList)
		{
			msleep(1);
			continue;
		}
		if (curAt && curAt->GetPackCount() > curAt->maxList)
		{
			msleep(1);
			continue;
		}

		AVPacket *pkt = curDemux->Read();
		if (!pkt) { msleep(5); continue; }

		if (seekPos_.load() >= 0.0)
		{
			if (curAt) curAt->RecyclePacket(pkt);
			else if (curVt) curVt->RecyclePacket(pkt);
			else av_packet_free(&pkt);
			continue;
		}

		if (curDemux->IsAudio(pkt))
		{
			if (curAt) curAt->Push(pkt);
			else av_packet_free(&pkt);
		}
		else
		{
			if (curVt) curVt->Push(pkt);
			else av_packet_free(&pkt);
		}
	}
}

bool DemuxThread::Open(const char *url, IVideoCallback *call)
{
	if (!url || url[0] == '\0') return false;

	mux.lock();
	auto *oldVt = vt;
	auto *oldAt = at;
	auto *oldDemux = demux;
	if (oldVt) oldVt->isExit = true;
	if (oldAt) oldAt->isExit = true;
	vt = nullptr;
	at = nullptr;
	demux = nullptr;
	mux.unlock();

	if (oldVt || oldAt || oldDemux)
	{
		auto& pool = GlobalThreadPool::Instance();
		auto f1 = pool.submitTask([](VideoThread *t) { if(t) { t->Close(); delete t; } }, oldVt);
		auto f2 = pool.submitTask([](AudioThread *t) { if(t) { t->Close(); delete t; } }, oldAt);
		auto f3 = pool.submitTask([](MediaDemuxer *t) { if(t) { t->Close(); delete t; } }, oldDemux);
		f1.get(); f2.get(); f3.get();
	}

	mux.lock();
	seekPos_ = -1.0;

	demux = new MediaDemuxer();
	if (!demux->Open(url))
	{
		mux.unlock();
		delete demux; demux = nullptr;
		return false;
	}

	totalMs = demux->totalMs;

	if (demux->width > 0 && demux->height > 0)
	{
		vt = new VideoThread();
		AVCodecParameters *vpara = demux->CopyVPara();
		if (vpara)
		{
			if (!vt->Open(vpara, call, demux->width, demux->height))
			{
				delete vt; vt = nullptr;
			}
		}
		else { delete vt; vt = nullptr; }
	}

	if (demux->sampleRate > 0 && demux->channels > 0)
	{
		at = new AudioThread();
		AVCodecParameters *apara = demux->CopyAPara();
		if (apara)
		{
			if (!at->Open(apara, demux->sampleRate, demux->channels))
			{
				delete at; at = nullptr;
			}
		}
		else { delete at; at = nullptr; }
	}

	if (!vt && !at)
	{
		mux.unlock();
		delete demux; demux = nullptr;
		return false;
	}

	if (vt)
	{
		auto *capturedVt = vt;
		vt->SetDecoderRecycler([capturedVt](AVPacket *p) { capturedVt->RecyclePacket(p); });
	}
	if (at)
	{
		auto *capturedAt = at;
		at->SetDecoderRecycler([capturedAt](AVPacket *p) { capturedAt->RecyclePacket(p); });
	}

	demux->SetPacketAllocator([this]() -> AVPacket* {
		AVPacket *pkt = nullptr;
		if (at) pkt = at->AllocPacket();
		else if (vt) pkt = vt->AllocPacket();
		return pkt ? pkt : av_packet_alloc();
	});

	if (vt) vt->start();
	if (at) at->start();

	if (at && at->ap) at->ap->SetVolume(volume_);
	double curSpeed = speed_.load();
	if (vt) vt->speed = curSpeed;
	if (at) at->SetSpeed(curSpeed);

	mux.unlock();

	cout << "[Demux] Open OK" << endl;
	return true;
}

void DemuxThread::Close()
{
	isExit = true;
	seekPos_ = -1.0;
	{
		std::lock_guard<std::mutex> lk(mux);
		if (vt) vt->isExit = true;
		if (at) at->isExit = true;
	}
	wait();
	AudioThread *oldAt = nullptr;
	VideoThread *oldVt = nullptr;
	MediaDemuxer *oldDemux = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		oldAt = at; at = nullptr;
		oldVt = vt; vt = nullptr;
		oldDemux = demux; demux = nullptr;
	}

	auto& pool = GlobalThreadPool::Instance();
	auto f1 = pool.submitTask([](AudioThread *t) { if (t) t->Close(); }, oldAt);
	auto f2 = pool.submitTask([](VideoThread *t) { if (t) t->Close(); }, oldVt);
	auto f3 = pool.submitTask([](MediaDemuxer *t) { if (t) t->Close(); }, oldDemux);
	f1.get(); f2.get(); f3.get();

	delete oldAt;
	delete oldVt;
	delete oldDemux;
}

void DemuxThread::Start()
{
	QThread::start();
}
