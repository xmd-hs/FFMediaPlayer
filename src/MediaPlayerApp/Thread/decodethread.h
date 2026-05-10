#pragma once
struct AVPacket;
class MediaDecoder;
#include "LockFreeQueue.hpp"
#include "LockFreeStack.hpp"
#include <mutex>
#include <atomic>
#include <thread>
#include <QThread>

class DecodeThread : public QThread
{
public:
	DecodeThread();
	virtual ~DecodeThread();
	virtual void Push(AVPacket *pkt);
	virtual void Clear();
	virtual void Close();
	virtual AVPacket *Pop();
	AVPacket *AllocPacket();
	void RecyclePacket(AVPacket *pkt);
	void SetDecoderRecycler(std::function<void(AVPacket*)> recycler);
	int GetPackCount();
	int maxList = 128;
	std::atomic_bool isExit = {false};
	std::atomic_bool isClearing = {false};
protected:
	MediaDecoder *decode = 0;
	LockFreeQueue<AVPacket*, 128> packs_;
	LockFreeStack<AVPacket*> freePackets_;
	std::mutex mux;
};