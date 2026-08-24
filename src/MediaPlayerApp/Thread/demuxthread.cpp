#include "demuxthread.h"
#include "mediademuxer.h"
#include "videothread.h"
#include "audiothread.h"
#include "subtitlethread.h"
#include "audioplayer.h"
#include "GlobalThreadPool.h"
#include <iostream>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace std;

DemuxThread::DemuxThread() {}
DemuxThread::~DemuxThread() { Close(); }

void DemuxThread::SetOpenOptions(const DemuxOpenOptions &opts)
{
	std::lock_guard<std::mutex> lk(mux);
	openOpts_ = opts;
}

DemuxOpenOptions DemuxThread::GetOpenOptions() const
{
	// const cast for mutex - use mutable would be better; copy under lock via const_cast pattern
	auto *self = const_cast<DemuxThread*>(this);
	std::lock_guard<std::mutex> lk(self->mux);
	return openOpts_;
}

std::string DemuxThread::LastError() const
{
	auto *self = const_cast<DemuxThread*>(this);
	std::lock_guard<std::mutex> lk(self->mux);
	return lastError_;
}

void DemuxThread::SetSubtitleCallback(ISubtitleCallback *cb)
{
	std::lock_guard<std::mutex> lk(mux);
	subCallback_ = cb;
	if (st) st->SetCallback(cb);
}

std::vector<SubtitleTrackInfo> DemuxThread::GetSubtitleTracks()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!demux) return {};
	return demux->ListSubtitleTracks();
}

std::vector<AudioTrackInfo> DemuxThread::GetAudioTracks()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!demux) return {};
	return demux->ListAudioTracks();
}

bool DemuxThread::openSubtitleLocked()
{
	if (st)
	{
		st->isExit = true;
		st->Close();
		delete st;
		st = nullptr;
	}
	if (!demux || demux->CurrentSubtitleStream() < 0)
		return false;

	AVCodecParameters *spara = demux->CopySPara();
	if (!spara) return false;

	st = new SubtitleThread();
	st->SetCallback(subCallback_);
	if (!st->Open(spara))
	{
		delete st;
		st = nullptr;
		return false;
	}
	auto *capturedSt = st;
	st->SetDecoderRecycler([capturedSt](AVPacket *p) { capturedSt->RecyclePacket(p); });
	st->start();
	return true;
}

bool DemuxThread::openAudioLocked()
{
	if (at)
	{
		at->isExit = true;
		at->Close();
		delete at;
		at = nullptr;
	}
	if (!demux || demux->sampleRate <= 0 || demux->channels <= 0)
		return false;

	AVCodecParameters *apara = demux->CopyAPara();
	if (!apara) return false;

	at = new AudioThread();
	if (!at->Open(apara, demux->sampleRate, demux->channels))
	{
		delete at;
		at = nullptr;
		return false;
	}
	auto *capturedAt = at;
	at->SetDecoderRecycler([capturedAt](AVPacket *p) { capturedAt->RecyclePacket(p); });
	if (at->ap) at->ap->SetVolume(volume_);
	at->SetSpeed(speed_.load());
	at->start();
	return true;
}

bool DemuxThread::SetSubtitleTrack(int streamIndex)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!demux) return false;
	if (!demux->SelectSubtitle(streamIndex))
	{
		lastError_ = demux->LastError();
		return false;
	}
	if (streamIndex < 0)
	{
		if (st)
		{
			st->isExit = true;
			st->Close();
			delete st;
			st = nullptr;
		}
		if (subCallback_) subCallback_->OnSubtitleClear();
		return true;
	}
	return openSubtitleLocked();
}

bool DemuxThread::SetAudioTrack(int streamIndex)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!demux) return false;
	if (!demux->SelectAudio(streamIndex))
	{
		lastError_ = demux->LastError();
		return false;
	}
	bool ok = openAudioLocked();
	if (!ok) lastError_ = "failed to open selected audio track";
	return ok;
}

void DemuxThread::Clear()
{
	MediaDemuxer *curDemux = nullptr;
	VideoThread *curVt = nullptr;
	AudioThread *curAt = nullptr;
	SubtitleThread *curSt = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		curDemux = demux;
		curVt = vt;
		curAt = at;
		curSt = st;
	}
	if (curDemux) curDemux->Clear();
	if (curVt) curVt->Clear();
	if (curAt) curAt->Clear();
	if (curSt) curSt->Clear();
}

void DemuxThread::Seek(double pos)
{
	if (totalMs <= 0) return;
	if (pos < 0.0) pos = 0.0;
	if (pos > 1.0) pos = 1.0;
	seekPos_ = pos;
}

void DemuxThread::doSeek(double pos)
{
	MediaDemuxer *curDemux = nullptr;
	VideoThread *curVt = nullptr;
	AudioThread *curAt = nullptr;
	SubtitleThread *curSt = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		if (!demux || (!vt && !at))
		{
			isPause = false;
			return;
		}
		curDemux = demux;
		curVt = vt;
		curAt = at;
		curSt = st;
	}

	isEof = false;
	isBuffering = true;
	bool wasPause = isPause.load();
	double curSpeed = speed_.load();
	isPause = true;
	if (curAt) curAt->SetPause(true);
	if (curVt) curVt->SetPause(true);
	if (curSt) curSt->SetPause(true);

	{
		auto& pool = GlobalThreadPool::Instance();
		auto f1 = pool.submitTask([](VideoThread *t) { if(t) t->Clear(); }, curVt);
		auto f2 = pool.submitTask([](AudioThread *t) { if(t) t->Clear(); }, curAt);
		auto f3 = pool.submitTask([](SubtitleThread *t) { if(t) t->Clear(); }, curSt);
		f1.get(); f2.get(); f3.get();
	}
	{
		std::lock_guard<std::mutex> lk(mux);
		if (demux) demux->Clear();
	}

	long long seekPts = (long long)(pos * totalMs);

	if (!curDemux->Seek(pos))
	{
		cout << "[Seek] failed" << endl;
		{
			std::lock_guard<std::mutex> lk(mux);
			lastError_ = curDemux->LastError();
		}
		isBuffering = false;
		if (!wasPause)
		{
			isPause = false;
			pauseCv_.notify_one();
			if (curAt) curAt->SetPause(false);
			if (curVt) curVt->SetPause(false);
			if (curSt) curSt->SetPause(false);
		}
		return;
	}

	if (curAt) curAt->ResetClock(seekPts);
	if (curVt) curVt->ResetSync(seekPts);
	pts = seekPts;

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
	std::vector<AVPacket*> pendingSub;
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
		else if (curDemux->IsVideo(pkt))
		{
			if (!curVt)
			{
				av_packet_free(&pkt);
				continue;
			}
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
		else if (curDemux->IsSubtitle(pkt))
		{
			if (curSt) pendingSub.push_back(pkt);
			else av_packet_free(&pkt);
		}
		else
		{
			av_packet_free(&pkt);
		}

		if (frameShown && audioPreFill >= AUDIO_PRE_FILL_COUNT) break;
		if (!curAt && frameShown) break;
		if (!curVt && audioPreFill >= AUDIO_PRE_FILL_COUNT) break;
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
	auto subFuture = pool.submitTask([curSt, pendingSub]() {
		for (auto* p : pendingSub)
		{
			if (curSt) curSt->Push(p);
			else av_packet_free(&p);
		}
	});
	audioFuture.get();
	videoFuture.get();
	subFuture.get();

	isBuffering = false;
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
		if (curSt) curSt->SetPause(false);
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
	if (st) st->SetPause(p);
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
		SubtitleThread *curSt = nullptr;
		{
			std::lock_guard<std::mutex> lk(mux);
			curDemux = demux;
			curVt = vt;
			curAt = at;
			curSt = st;
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

		int vq = curVt ? curVt->GetPackCount() : 0;
		int aq = curAt ? curAt->GetPackCount() : 0;
		if ((curVt && vq > curVt->maxList) || (curAt && aq > curAt->maxList) ||
			(curSt && curSt->GetPackCount() > curSt->maxList))
		{
			isBuffering = false;
			msleep(1);
			continue;
		}

		// Starving decode queues => show buffering for network streams.
		if ((curVt || curAt) && vq == 0 && aq == 0)
			isBuffering = true;

		AVPacket *pkt = curDemux->Read();
		if (!pkt)
		{
			if (curDemux->IsEof())
			{
				isEof = true;
				isPause = true;
				isBuffering = false;
				if (curAt) curAt->SetPause(true);
				if (curVt) curVt->SetPause(true);
				if (curSt) curSt->SetPause(true);
			}
			else
			{
				isBuffering = true;
				msleep(5);
			}
			continue;
		}

		isBuffering = false;

		if (seekPos_.load() >= 0.0)
		{
			if (curAt) curAt->RecyclePacket(pkt);
			else if (curVt) curVt->RecyclePacket(pkt);
			else if (curSt) curSt->RecyclePacket(pkt);
			else av_packet_free(&pkt);
			continue;
		}

		if (curDemux->IsAudio(pkt))
		{
			if (curAt) curAt->Push(pkt);
			else av_packet_free(&pkt);
		}
		else if (curDemux->IsVideo(pkt))
		{
			if (curVt) curVt->Push(pkt);
			else av_packet_free(&pkt);
		}
		else if (curDemux->IsSubtitle(pkt))
		{
			if (curSt) curSt->Push(pkt);
			else av_packet_free(&pkt);
		}
		else
		{
			if (curAt) curAt->RecyclePacket(pkt);
			else if (curVt) curVt->RecyclePacket(pkt);
			else av_packet_free(&pkt);
		}
	}
}

bool DemuxThread::Open(const char *url, IVideoCallback *call, ISubtitleCallback *subCall)
{
	if (!url || url[0] == '\0')
	{
		lastError_ = "empty url";
		return false;
	}

	isOpening = true;
	isBuffering = true;

	mux.lock();
	if (subCall) subCallback_ = subCall;
	DemuxOpenOptions opts = openOpts_;
	auto *oldVt = vt;
	auto *oldAt = at;
	auto *oldSt = st;
	auto *oldDemux = demux;
	if (oldVt) oldVt->isExit = true;
	if (oldAt) oldAt->isExit = true;
	if (oldSt) oldSt->isExit = true;
	vt = nullptr;
	at = nullptr;
	st = nullptr;
	demux = nullptr;
	mux.unlock();

	if (oldVt || oldAt || oldSt || oldDemux)
	{
		auto& pool = GlobalThreadPool::Instance();
		auto f1 = pool.submitTask([](VideoThread *t) { if(t) { t->Close(); delete t; } }, oldVt);
		auto f2 = pool.submitTask([](AudioThread *t) { if(t) { t->Close(); delete t; } }, oldAt);
		auto f3 = pool.submitTask([](SubtitleThread *t) { if(t) { t->Close(); delete t; } }, oldSt);
		auto f4 = pool.submitTask([](MediaDemuxer *t) { if(t) { t->Close(); delete t; } }, oldDemux);
		f1.get(); f2.get(); f3.get(); f4.get();
	}

	mux.lock();
	seekPos_ = -1.0;
	isEof = false;
	isPause = false;
	lastError_.clear();

	demux = new MediaDemuxer();
	if (!demux->Open(url, opts))
	{
		lastError_ = demux->LastError();
		mux.unlock();
		delete demux; demux = nullptr;
		isOpening = false;
		isBuffering = false;
		return false;
	}

	totalMs = demux->totalMs;

	if (demux->width > 0 && demux->height > 0)
	{
		vt = new VideoThread();
		AVCodecParameters *vpara = demux->CopyVPara();
		if (vpara)
		{
			if (!vt->Open(vpara, call, demux->width, demux->height, opts.tryHwAccel))
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
		lastError_ = "no playable audio/video stream";
		mux.unlock();
		delete demux; demux = nullptr;
		isOpening = false;
		isBuffering = false;
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

	openSubtitleLocked();

	demux->SetPacketAllocator([this]() -> AVPacket* {
		AVPacket *pkt = nullptr;
		if (at) pkt = at->AllocPacket();
		else if (vt) pkt = vt->AllocPacket();
		else if (st) pkt = st->AllocPacket();
		return pkt ? pkt : av_packet_alloc();
	});

	if (vt) vt->start();
	if (at) at->start();

	if (at && at->ap) at->ap->SetVolume(volume_);
	double curSpeed = speed_.load();
	if (vt) vt->speed = curSpeed;
	if (at) at->SetSpeed(curSpeed);

	mux.unlock();

	isOpening = false;
	isBuffering = false;
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
		if (st) st->isExit = true;
	}
	wait();
	AudioThread *oldAt = nullptr;
	VideoThread *oldVt = nullptr;
	SubtitleThread *oldSt = nullptr;
	MediaDemuxer *oldDemux = nullptr;
	{
		std::lock_guard<std::mutex> lk(mux);
		oldAt = at; at = nullptr;
		oldVt = vt; vt = nullptr;
		oldSt = st; st = nullptr;
		oldDemux = demux; demux = nullptr;
	}

	auto& pool = GlobalThreadPool::Instance();
	auto f1 = pool.submitTask([](AudioThread *t) { if (t) t->Close(); }, oldAt);
	auto f2 = pool.submitTask([](VideoThread *t) { if (t) t->Close(); }, oldVt);
	auto f3 = pool.submitTask([](SubtitleThread *t) { if (t) t->Close(); }, oldSt);
	auto f4 = pool.submitTask([](MediaDemuxer *t) { if (t) t->Close(); }, oldDemux);
	f1.get(); f2.get(); f3.get(); f4.get();

	delete oldAt;
	delete oldVt;
	delete oldSt;
	delete oldDemux;
	isBuffering = false;
	isOpening = false;
}

void DemuxThread::Start()
{
	QThread::start();
}
