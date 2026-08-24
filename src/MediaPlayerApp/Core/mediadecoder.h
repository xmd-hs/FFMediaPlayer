#pragma once
#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct AVCodecContext;
struct AVBufferRef;
struct AVRational;

struct SubtitleDecodeResult
{
	std::string text;
	std::vector<uint8_t> rgba; // optional bitmap, RGBA8888
	int bmpW = 0;
	int bmpH = 0;
};

class MediaDecoder
{
public:
	virtual bool Open(AVCodecParameters *para);
	virtual bool Open(AVCodecParameters *para, bool tryHwAccel);
	virtual void Close();
	virtual bool Send(AVPacket *pkt);
	virtual AVFrame* Recv();
	virtual void Clear();
	SubtitleDecodeResult DecodeSubtitle(AVPacket *pkt, long long *startMs, long long *endMs);

	MediaDecoder();
	virtual ~MediaDecoder();

	long long getPts() const { return pts.load(); }
	bool UsingHwAccel() const { return hwDeviceCtx_ != nullptr; }
	std::string LastError() const;

	void SetPacketRecycler(std::function<void(AVPacket*)> recycler);

protected:
	mutable std::mutex mux;
	AVCodecContext *codec_ = nullptr;
	AVBufferRef *hwDeviceCtx_ = nullptr;
	int hwPixFmt_ = -1;
	std::atomic<long long> pts = {0};
	std::string lastError_;
	std::function<void(AVPacket*)> packetRecycler_;
	bool openInternal(AVCodecParameters *para, bool tryHwAccel);
};
