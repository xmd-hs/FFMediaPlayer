#pragma once
#include <mutex>
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

	long long getPts() const { return pts; }
	AVRational getTimeBase() const;

protected:
	mutable std::mutex mux;
	AVCodecContext *codec_ = nullptr;
	long long pts = 0;
};
