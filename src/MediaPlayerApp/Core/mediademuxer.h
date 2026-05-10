#pragma once
#include <mutex>
#include <functional>
struct AVFormatContext;
struct AVPacket;
struct AVCodecParameters;

class MediaDemuxer
{
public:
	virtual bool Open(const char *url);
	virtual void Close();
	virtual AVPacket* Read();
	virtual AVPacket* ReadVideo();
	virtual bool IsAudio(AVPacket *pkt);
	virtual AVCodecParameters* CopyVPara();
	virtual AVCodecParameters* CopyAPara();
	bool Seek(double pos);
	void Clear();

	MediaDemuxer();
	virtual ~MediaDemuxer();

	int totalMs = 0;
	int width = 0;
	int height = 0;
	int sampleRate = 0;
	int channels = 0;

	void SetPacketAllocator(std::function<AVPacket*()> alloc);

protected:
	std::mutex mux;
	AVFormatContext *ic = nullptr;
	int videoStream = -1;
	int audioStream = -1;
	std::function<AVPacket*()> packetAlloc_;
};
