#include "decodethread.h"
#include "mediadecoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

void DecodeThread::Close()
{
	Clear();
	isExit = true;
	wait();
	if (decode)
	{
		decode->Close();
		{
			std::lock_guard<std::mutex> lk(mux);
			delete decode;
			decode = NULL;
		}
	}
}

void DecodeThread::Clear()
{
	std::lock_guard<std::mutex> lk(mux);
	if (decode) decode->Clear();
	while (!packs.empty())
	{
		AVPacket *pkt = packs.front();
		XFreePacket(&pkt);
		packs.pop_front();
	}
}

AVPacket *DecodeThread::Pop()
{
	std::lock_guard<std::mutex> lk(mux);
	if (packs.empty()) return NULL;
	AVPacket *pkt = packs.front();
	packs.pop_front();
	return pkt;
}

void DecodeThread::Push(AVPacket *pkt)
{
	if (!pkt) return;
	while (!isExit)
	{
		{
			std::lock_guard<std::mutex> lk(mux);
			if (packs.size() < (size_t)maxList)
			{
				packs.push_back(pkt);
				break;
			}
		}
		msleep(1);
	}
	if (isExit) av_packet_free(&pkt);
}

int DecodeThread::GetPackCount()
{
	std::lock_guard<std::mutex> lk(mux);
	return (int)packs.size();
}

DecodeThread::DecodeThread()
{
	if (!decode) decode = new MediaDecoder();
}

DecodeThread::~DecodeThread()
{
	isExit = true;
	wait();
}
