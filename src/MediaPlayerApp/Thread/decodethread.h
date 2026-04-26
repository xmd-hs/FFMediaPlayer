#pragma once
struct AVPacket;
class MediaDecoder;
#include <list>
#include <mutex>
#include <atomic>
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
	int GetPackCount();
	int maxList = 100;
	std::atomic_bool isExit = {false};
protected:
	MediaDecoder *decode = 0;
	std::list<AVPacket *> packs;
	std::mutex mux;
};
