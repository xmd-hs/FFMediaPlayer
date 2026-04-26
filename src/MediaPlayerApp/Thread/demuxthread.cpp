#include "demuxthread.h"
#include "mediademuxer.h"
#include "videothread.h"
#include "audiothread.h"
#include "audioplayer.h"
#include <iostream>
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
	std::lock_guard<std::mutex> lk(mux);
	if (demux) demux->Clear();
	if (vt) vt->Clear();
	if (at) at->Clear();
}

void DemuxThread::Seek(double pos)
{
	if (pos < 0.0) pos = 0.0;
	if (pos > 1.0) pos = 1.0;
	seekPos_ = pos;
}

void DemuxThread::doSeek(double pos)
{
	std::lock_guard<std::mutex> lk(mux);

	if (!demux || !vt)
	{
		isPause = false;
		return;
	}

	bool wasPause = isPause.load();
	isPause = true;
	if (at) at->SetPause(true);
	if (vt) vt->SetPause(true);

	if (vt) vt->Clear();
	if (at) at->Clear();
	if (demux) demux->Clear();

	long long seekPts = (long long)(pos * totalMs);

	if (!demux->Seek(pos))
	{
		cout << "[Seek] failed" << endl;
		if (!wasPause)
		{
			isPause = false;
			if (at) at->SetPause(false);
			if (vt) vt->SetPause(false);
		}
		return;
	}

	if (at) at->ResetClock(seekPts);
	if (vt) vt->ResetSync(seekPts);
	pts = seekPts;

	bool frameShown = false;
	int audioPreFill = 0;
	const int AUDIO_PRE_FILL_COUNT = 10;

	for (int i = 0; i < 80 && !isExit; i++)
	{
		if (seekPos_.load() >= 0.0) break;

		AVPacket *pkt = demux->Read();
		if (!pkt) { msleep(2); continue; }

		if (demux->IsAudio(pkt))
		{
			if (at) { at->Push(pkt); audioPreFill++; }
			else av_packet_free(&pkt);
		}
		else
		{
			if (!frameShown)
			{
				if (vt->RepaintPts(pkt))
				{
					frameShown = true;
				}
			}
			else
			{
				if (vt) vt->Push(pkt);
				else av_packet_free(&pkt);
			}
		}

		if (frameShown && audioPreFill >= AUDIO_PRE_FILL_COUNT) break;
	}

	if (!wasPause)
	{
		isPause = false;
		if (at) at->SetPause(false);
		if (vt) vt->SetPause(false);
	}
}

void DemuxThread::SetPause(bool p)
{
	isPause = p;
	std::lock_guard<std::mutex> lk(mux);
	if (at) at->SetPause(p);
	if (vt) vt->SetPause(p);
}

void DemuxThread::SetVolume(int volume)
{
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

		if (isPause) { msleep(5); continue; }

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
			av_packet_free(&pkt);
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

	if (oldVt) { oldVt->Close(); delete oldVt; }
	if (oldAt) { oldAt->Close(); delete oldAt; }
	if (oldDemux) { oldDemux->Close(); delete oldDemux; }

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

	if (vt) vt->start();
	if (at) at->start();
	mux.unlock();

	cout << "[Demux] Open OK" << endl;
	return true;
}

void DemuxThread::Close()
{
	isExit = true;
	seekPos_ = -1.0;
	mux.lock();
	if (vt) vt->isExit = true;
	if (at) at->isExit = true;
	mux.unlock();
	wait();
	mux.lock();
	if (at) { at->Close(); }
	if (vt) { vt->Close(); }
	if (demux) { demux->Close(); delete demux; demux = nullptr; }
	delete vt; vt = nullptr;
	delete at; at = nullptr;
	mux.unlock();
}

void DemuxThread::Start()
{
	QThread::start();
}
