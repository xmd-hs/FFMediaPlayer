#include "videothread.h"
#include "mediadecoder.h"
#include <iostream>
#include <algorithm>
extern "C" {
#include <libavutil/frame.h>
}
using namespace std;

VideoThread::VideoThread() {}
VideoThread::~VideoThread() { Close(); }

long long VideoThread::getPts() { return decode->getPts(); }

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

		long long audioPts;
		bool curFirstFrame;
		{
			std::lock_guard<std::mutex> lk(vmux);
			audioPts = synpts;
			curFirstFrame = firstFrame;
		}

		{
			std::lock_guard<std::mutex> lk(mux);
			if (!decode || !decode->Send(pkt))
			{
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
				}
				else
				{
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
			}

			{
				std::lock_guard<std::mutex> lk(vmux);
				lastPts = framePts;
				lastFrameTime = steady_clock::now();
				firstFrame = false;
			}

			if (call) call->Repaint(frame);
			else av_frame_free(&frame);
		}

		msleep(1);
	}
}

bool VideoThread::RepaintPts(AVPacket *pkt)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!decode || !decode->Send(pkt))
	{
		return false;
	}

	for (int i = 0; i < 10; i++)
	{
		AVFrame *frame = decode->Recv();
		if (frame)
		{
			if (call) call->Repaint(frame);
			else av_frame_free(&frame);
			return true;
		}
	}

	return false;
}
