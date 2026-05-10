#include "decodethread.h"
#include "mediadecoder.h"
#include "LockFreeQueue.hpp"
#include "LockFreeStack.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
}

void DecodeThread::Close()
{
	isExit = true;
	Clear();
	wait();
	if (decode)
	{
		decode->Close();
		delete decode;
		decode = nullptr;
	}
	AVPacket *pkt = nullptr;
	while (freePackets_.pop(pkt))
		av_packet_free(&pkt);
}

void DecodeThread::Clear()
{
	isClearing = true;
	std::lock_guard<std::mutex> lk(mux);
	if (decode) decode->Clear();
	AVPacket *pkt = nullptr;
	while (packs_.pop(pkt))
	{
		av_packet_unref(pkt);
		freePackets_.push(pkt);
	}
	packs_.reset();
	isClearing = false;
}

AVPacket *DecodeThread::Pop()
{
	if (isClearing.load()) return nullptr;
	AVPacket *pkt = nullptr;
	if (packs_.pop(pkt))
		return pkt;
	return nullptr;
}

void DecodeThread::Push(AVPacket *pkt)
{
	if (!pkt) return;
	while (!packs_.push(pkt))
	{
		if (isExit) { RecyclePacket(pkt); return; }
		std::this_thread::yield();
	}
}

AVPacket *DecodeThread::AllocPacket()
{
	AVPacket *pkt = nullptr;
	if (freePackets_.pop(pkt))
		return pkt;
	return av_packet_alloc();
}

void DecodeThread::RecyclePacket(AVPacket *pkt)
{
	if (!pkt) return;
	av_packet_unref(pkt);
	freePackets_.push(pkt);
}

void DecodeThread::SetDecoderRecycler(std::function<void(AVPacket*)> recycler)
{
	if (decode) decode->SetPacketRecycler(std::move(recycler));
}

int DecodeThread::GetPackCount()
{
	return (int)packs_.size();
}

DecodeThread::DecodeThread()
{
	decode = new MediaDecoder();
}

DecodeThread::~DecodeThread()
{
	isExit = true;
	wait();
}
