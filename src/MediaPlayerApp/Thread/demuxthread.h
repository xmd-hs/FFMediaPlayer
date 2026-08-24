#pragma once

#include <QThread>
#include "ivideocallback.h"
#include "isubtitlecallback.h"
#include "mediademuxer.h"
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>

struct AVPacket;
class MediaDemuxer;
class VideoThread;
class AudioThread;
class SubtitleThread;

class DemuxThread : public QThread
{
public:
	bool Open(const char *url, IVideoCallback *call, ISubtitleCallback *subCall = nullptr);
	void Start();
	void Close();
	void Clear();
	void Seek(double pos);
	void run();
	DemuxThread();
	~DemuxThread() override;

	std::atomic_bool isExit = {false};
	std::atomic<long long> pts = {0};
	long long totalMs = 0;
	void SetPause(bool isPause);
	std::atomic_bool isPause = {false};
	std::atomic_bool isEof = {false};
	std::atomic_bool isBuffering = {false};
	std::atomic_bool isOpening = {false};
	void SetVolume(int volume);
	int GetVolume();
	int volume_ = 80;
	void SetSpeed(double speed);
	double GetSpeed();
	long long GetCurrentPts();
	bool IsLive() const { return totalMs <= 0; }
	std::string LastError() const;

	void SetOpenOptions(const DemuxOpenOptions &opts);
	DemuxOpenOptions GetOpenOptions() const;

	std::vector<SubtitleTrackInfo> GetSubtitleTracks();
	std::vector<AudioTrackInfo> GetAudioTracks();
	bool SetSubtitleTrack(int streamIndex); // -1 = off
	bool SetAudioTrack(int streamIndex);
	void SetSubtitleCallback(ISubtitleCallback *cb);

protected:
	std::mutex mux;
	MediaDemuxer *demux = nullptr;
	VideoThread *vt = nullptr;
	AudioThread *at = nullptr;
	SubtitleThread *st = nullptr;
	ISubtitleCallback *subCallback_ = nullptr;
	DemuxOpenOptions openOpts_;
	std::string lastError_;
	std::atomic<double> speed_ = {1.0};
	std::atomic<double> seekPos_ = {-1.0};
	std::condition_variable pauseCv_;
	std::mutex pauseMux_;
	void doSeek(double pos);
	bool openSubtitleLocked();
	bool openAudioLocked();
};
