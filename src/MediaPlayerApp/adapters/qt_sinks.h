#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include "../../../sdk/include/player.h"

class QLabel; class QPushButton; class QSlider;
class QAudioOutput;
class QIODevice;
class QMutex;
class QWaitCondition;

class QtAudioBuffer final : public QIODevice {
public:
    explicit QtAudioBuffer(QObject *parent = nullptr);
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;
    qint64 bytesAvailable() const override;
private:
    QByteArray buffer_;
    mutable QMutex mutex_;
};

class QtVideoSink final : public ffplayer::IVideoSink {
public:
    explicit QtVideoSink(QLabel *view = nullptr) : view_(view) {}
    void setView(QLabel *view) { view_ = view; }
    void onVideoFrame(const ffplayer::VideoFrame &frame) override;
private:
    QLabel *view_;
};

class QtAudioSink final : public ffplayer::IAudioSink {
public:
    QtAudioSink();
    ~QtAudioSink() override;
    bool onAudioChunk(const ffplayer::AudioChunk &chunk) override;
    void setVolume(int percent);
private:
    QAudioOutput *output_ = nullptr;
    QIODevice *device_ = nullptr;
    QtAudioBuffer *buffer_ = nullptr;
};
