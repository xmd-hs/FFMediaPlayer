#pragma once

#include <QThread>
#include "ivideocallback.h"
#include <mutex>
#include <atomic>

struct AVPacket;
class MediaDemuxer;
class VideoThread;
class AudioThread;

class DemuxThread : public QThread
{
public:
	bool Open(const char *url, IVideoCallback *call);
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
	void SetVolume(int volume);
	int GetVolume();
	void SetSpeed(double speed);
	double GetSpeed();
	long long GetCurrentPts();

protected:
	std::mutex mux;
	MediaDemuxer *demux = nullptr;
	VideoThread *vt = nullptr;
	AudioThread *at = nullptr;
	std::atomic<double> speed_ = {1.0};
	std::atomic<double> seekPos_ = {-1.0};
	void doSeek(double pos);
};
