#pragma once
class AudioPlayer
{
public:
	int sampleRate = 44100;
	int sampleSize = 16;
	int channels = 2;
	double speed = 1.0;

	virtual bool Open() = 0;
	virtual void Close() = 0;
	virtual void Clear() = 0;
	virtual bool Write(const unsigned char *data, int datasize) = 0;
	virtual int GetFree() = 0;
	virtual void SetPause(bool isPause) = 0;
	virtual void SetVolume(int volume) = 0;
	virtual int GetVolume() = 0;
	virtual void SetSpeed(double s) = 0;
	// 获取音频实际播放时间（毫秒）
	virtual long long GetPlayedMs() = 0;

	static AudioPlayer *Get();
	AudioPlayer();
	virtual ~AudioPlayer();
};
