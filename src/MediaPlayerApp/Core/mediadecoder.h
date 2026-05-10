#pragma once
#include <mutex>
#include <atomic>
#include <functional>
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct AVCodecContext;
struct AVRational;
void XFreePacket(AVPacket **pkt);
void XFreeFrame(AVFrame **frame);

class MediaDecoder
{
public:
	virtual bool Open(AVCodecParameters *para);
	virtual void Close();
	virtual bool Send(AVPacket *pkt);
	virtual AVFrame* Recv();
	virtual void Clear();

	MediaDecoder();
	virtual ~MediaDecoder();

	long long getPts() const { return pts.load(); }
	AVRational getTimeBase() const;

	void SetPacketRecycler(std::function<void(AVPacket*)> recycler);

protected:
	mutable std::mutex mux;
	AVCodecContext *codec_ = nullptr;
	std::atomic<long long> pts = {0};
	std::function<void(AVPacket*)> packetRecycler_;
};
