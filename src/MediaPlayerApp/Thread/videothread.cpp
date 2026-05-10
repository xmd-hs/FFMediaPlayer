#include "videothread.h"
#include "mediadecoder.h"
#include <iostream>
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
}
using namespace std;

VideoThread::VideoThread() {}
VideoThread::~VideoThread() { Close(); }

void VideoThread::Close()
{
	isExit = true;
	DecodeThread::Close();
	if (repaintFuture_.valid()) repaintFuture_.get();
}

long long VideoThread::getPts()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!decode) return 0;
	return decode->getPts();
}

void VideoThread::Clear()
{
	DecodeThread::Clear();
	std::lock_guard<std::mutex> lk(vmux);
	synpts = 0;
	firstFrame = true;
	lastPts = 0;
	lastFrameTime = std::chrono::steady_clock::now();
}

bool VideoThread::Open(AVCodecParameters *para, IVideoCallback *call, int width, int height)
{
	if (!para) return false;
	Clear();

	{
		std::lock_guard<std::mutex> lk(vmux);
		synpts = 0;
		this->call = call;
		if (call) call->Init(width, height);
	}

	bool ok = decode->Open(para);
	cout << "[Video] open " << (ok ? "OK" : "FAIL") << endl;
	return ok;
}

void VideoThread::SetPause(bool p)
{
	std::lock_guard<std::mutex> lk(vmux);
	isPause = p;
}

void VideoThread::ResetSync(long long seekPts)
{
	std::lock_guard<std::mutex> lk(vmux);
	synpts = seekPts;
	firstFrame = true;
	lastPts = 0;
	lastFrameTime = std::chrono::steady_clock::now();
}

void VideoThread::run()
{
	using namespace std::chrono;

	while (!isExit)
	{
		if (isPause) { msleep(5); continue; }

		double curSpeed = speed.load();

		AVPacket *pkt = Pop();
		if (!pkt) { msleep(1); continue; }

		long long audioPts = synpts.load();
		bool curFirstFrame = firstFrame.load();

		{
			std::lock_guard<std::mutex> lk(mux);
			if (!decode)
			{
				RecyclePacket(pkt);
				continue;
			}
			if (!decode->Send(pkt))
			{
				RecyclePacket(pkt);
				continue;
			}
		}

		while (!isExit)
		{
			AVFrame *frame;
			{
				std::lock_guard<std::mutex> lk(mux);
				if (!decode) break;
				frame = decode->Recv();
			}

			if (!frame) break;

			long long framePts = frame->pts;
			if (framePts <= 0)
			{
				std::lock_guard<std::mutex> lk(mux);
				if (decode) framePts = decode->getPts();
			}

			if (!curFirstFrame)
			{
				if (audioPts > 0 && framePts > 0)
				{
					long long diff = framePts - audioPts;

					if (diff > 60)
					{
						long long waitMs = curSpeed > 0 ? (long long)(diff / curSpeed) : diff;
						msleep((unsigned long)std::min(waitMs, 200LL));
					}
					else if (diff >= -200)
					{
						waitForFrame(framePts, curSpeed);
					}
				}
				else
				{
					waitForFrame(framePts, curSpeed);
				}
			}

			{
				std::lock_guard<std::mutex> lk(vmux);
				lastPts = framePts;
				lastFrameTime = steady_clock::now();
				firstFrame = false;
			}

			if (repaintFuture_.valid()) repaintFuture_.get();
			{
				std::lock_guard<std::mutex> lk(vmux);
				if (call)
				{
					auto& pool = GlobalThreadPool::Instance();
					IVideoCallback *cb = call;
					repaintFuture_ = pool.submitTask([cb, frame]() { cb->Repaint(frame); });
				}
				else
				{
					av_frame_free(&frame);
				}
			}
		}

		msleep(1);
	}

	if (repaintFuture_.valid()) repaintFuture_.get();
}

void VideoThread::waitForFrame(long long framePts, double curSpeed)
{
	using namespace std::chrono;

	long long curLastPts;
	steady_clock::time_point curLastFrameTime;
	{
		std::lock_guard<std::mutex> lk(vmux);
		curLastPts = lastPts;
		curLastFrameTime = lastFrameTime;
	}

	if (curLastPts > 0 && framePts > curLastPts)
	{
		long long frameDiff = framePts - curLastPts;
		if (frameDiff > 0 && frameDiff < 500)
		{
			auto now = steady_clock::now();
			auto elapsed = duration_cast<milliseconds>(now - curLastFrameTime);
			long long frameWaitMs = curSpeed > 0 ? (long long)(frameDiff / curSpeed) : frameDiff;
			long long waitMs = frameWaitMs - elapsed.count();
			if (waitMs > 0 && waitMs < 500)
			{
				msleep((unsigned long)waitMs);
			}
		}
	}
}

bool VideoThread::RepaintPts(AVPacket *pkt, long long *outPts)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!decode)
	{
		RecyclePacket(pkt);
		return false;
	}
	if (!decode->Send(pkt))
	{
		RecyclePacket(pkt);
		return false;
	}

	for (int i = 0; i < 10; i++)
	{
		AVFrame *frame = decode->Recv();
		if (frame)
		{
			if (outPts)
			{
				long long framePts = frame->pts;
				if (framePts <= 0) framePts = decode->getPts();
				*outPts = framePts;
			}
			if (call) call->Repaint(frame);
			else av_frame_free(&frame);
			return true;
		}
	}

	return false;
}
