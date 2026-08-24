#pragma once
#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
struct AVFormatContext;
struct AVPacket;
struct AVCodecParameters;

struct StreamTrackInfo
{
	int streamIndex = -1;
	std::string language;
	std::string codec;
	std::string title;
};

// Backward-compatible alias used by subtitle UI.
using SubtitleTrackInfo = StreamTrackInfo;
using AudioTrackInfo = StreamTrackInfo;

struct DemuxOpenOptions
{
	std::string userAgent = "FFMediaPlayer/1.0";
	std::string headers; // raw "Key: Value\r\nKey2: Value2\r\n"
	bool tryHwAccel = false;
};

class MediaDemuxer
{
public:
	virtual bool Open(const char *url);
	virtual bool Open(const char *url, const DemuxOpenOptions &opts);
	virtual void Close();
	virtual AVPacket* Read();
	virtual bool IsAudio(AVPacket *pkt);
	virtual bool IsVideo(AVPacket *pkt);
	virtual bool IsSubtitle(AVPacket *pkt);
	virtual AVCodecParameters* CopyVPara();
	virtual AVCodecParameters* CopyAPara();
	virtual AVCodecParameters* CopySPara();
	bool Seek(double pos);
	void Clear();
	bool IsEof() const { return eof_.load(); }
	bool IsLive() const { return totalMs <= 0; }
	std::string LastError() const;

	std::vector<SubtitleTrackInfo> ListSubtitleTracks();
	std::vector<AudioTrackInfo> ListAudioTracks();
	bool SelectSubtitle(int streamIndex); // -1 disables
	bool SelectAudio(int streamIndex);
	int CurrentSubtitleStream() const { return subtitleStream; }
	int CurrentAudioStream() const { return audioStream; }

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
	int subtitleStream = -1;
	std::vector<SubtitleTrackInfo> subtitleTracks_;
	std::vector<AudioTrackInfo> audioTracks_;
	std::atomic_bool eof_{false};
	std::string lastError_;
	std::function<AVPacket*()> packetAlloc_;
	void setError(const std::string &msg);
};
