#include "audioplayer.h"
#include <QtGlobal>
#include <mutex>
#include <iostream>
#include <thread>
#include <chrono>
using namespace std;

extern "C" {
#include <libavutil/log.h>
}

#include <QAudioOutput>
#include <QAudioDeviceInfo>
#include <QAudioFormat>

class CAudioPlay : public AudioPlayer
{
public:
	bool Open() override
	{
		Close();

		static std::once_flag logFlag;
		std::call_once(logFlag, []() {
			av_log_set_level(AV_LOG_ERROR);
		});

		cout << "[Audio] Opening audio device..." << endl;

		QAudioFormat fmt;
		fmt.setSampleRate(sampleRate);
		fmt.setChannelCount(channels);
		fmt.setSampleSize(16);
		fmt.setByteOrder(QAudioFormat::LittleEndian);
		fmt.setSampleType(QAudioFormat::SignedInt);
		fmt.setCodec("audio/pcm");

		QAudioDeviceInfo devInfo = QAudioDeviceInfo::defaultOutputDevice();
		if (!devInfo.isNull())
		{
			cout << "[Audio] Using default device: " << devInfo.deviceName().toStdString() << endl;
		}
		else
		{
			QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
			if (!devices.isEmpty())
			{
				devInfo = devices.first();
				cout << "[Audio] Using first available: " << devInfo.deviceName().toStdString() << endl;
			}
			else
			{
				cout << "[Audio] ERROR: No audio output device!" << endl;
				return false;
			}
		}

		if (!devInfo.isFormatSupported(fmt))
		{
			fmt = devInfo.nearestFormat(fmt);
		}

		mux.lock();
		output = new QAudioOutput(devInfo, fmt);
		int bufSize = sampleRate * channels;
		if (bufSize < 8192) bufSize = 8192;
		output->setBufferSize(bufSize);
		io = output->start();
		playStartElapsed = output->elapsedUSecs() / 1000;
		mux.unlock();

		if (!io)
		{
			cout << "[Audio] ERROR: output->start() returned null!" << endl;
			mux.lock();
			delete output;
			output = nullptr;
			mux.unlock();
			return false;
		}

		cout << "Audio opened, bufsize=" << bufSize << " sr=" << sampleRate << " ch=" << channels << endl;
		return true;
	}

	void Close() override
	{
		mux.lock();
		if (output)
		{
			output->stop();
			QAudioOutput *old = output;
			output = nullptr;
			io = nullptr;
			mux.unlock();
			this_thread::sleep_for(chrono::milliseconds(50));
			delete old;
		}
		else
		{
			mux.unlock();
		}
	}

	void Clear() override
	{
		std::lock_guard<std::mutex> lk(mux);
		if (output)
		{
			output->reset();
			io = output->start();
			playStartElapsed = 0;
		}
	}

	long long GetPlayedMs() override
	{
		std::lock_guard<std::mutex> lk(mux);
		long long playedMs = 0;
		if (output && io && sampleRate > 0 && channels > 0)
		{
			long long processedMs = output->processedUSecs() / 1000;
			playedMs = processedMs - playStartElapsed;
			if (playedMs < 0) playedMs = 0;
			if (playedMs > 3600000) playedMs = 0; // 防止异常值
		}
		return playedMs;
	}

	bool Write(const unsigned char *data, int datasize) override
	{
		if (!data || datasize <= 0) return false;
		mux.lock();
		if (!output) { mux.unlock(); return false; }
		if (!io) { io = output->start(); }
		if (!io)
		{
			static bool warned = false;
			if (!warned)
			{
				cout << "[Audio] ERROR: Failed to start audio output (no audio device?)" << endl;
				warned = true;
			}
			mux.unlock();
			return false;
		}
		int written = io->write((const char *)data, datasize);
		mux.unlock();
		return written > 0;
	}

	int GetFree() override
	{
		mux.lock();
		if (!output) { mux.unlock(); return 0; }
		if (!io) { mux.unlock(); return output->bufferSize(); }
		int f = output->bytesFree();
		mux.unlock();
		return f;
	}

	void SetPause(bool isPause) override
	{
		std::lock_guard<std::mutex> lk(mux);
		if (output)
		{
			if (isPause) output->suspend();
			else
			{
				if (!io) io = output->start();
				else output->resume();
			}
		}
	}

	void SetVolume(int volume) override
	{
		mux.lock();
		if (output) output->setVolume(qreal(volume) / 100.0);
		mux.unlock();
	}

	int GetVolume() override
	{
		mux.lock();
		int v = 0;
		if (output) v = int(output->volume() * 100.0);
		mux.unlock();
		return v;
	}

	void SetSpeed(double s) override
	{
		if (s <= 0 || s > 8.0) return;
		speed = s;
		// 音频输出设备保持原始采样率，不需要重新初始化
		// 倍速由重采样器处理
	}

	CAudioPlay() {}
	~CAudioPlay() { Close(); }

private:
	QAudioOutput *output = nullptr;
	QIODevice *io = nullptr;
	std::mutex mux;
	long long playStartElapsed = 0;
};

AudioPlayer::AudioPlayer() {}
AudioPlayer::~AudioPlayer() {}
AudioPlayer *AudioPlayer::Get() { static CAudioPlay p; return &p; }
