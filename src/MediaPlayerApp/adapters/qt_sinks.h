#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QRecursiveMutex>
#include <QString>
#include <QImage>
#include "../../../sdk/include/player.h"

class QLabel;
class QAudioOutput;
class MetalVideoViewHost;
class D3d11VideoViewHost;

class QtAudioBuffer final : public QIODevice {
public:
    explicit QtAudioBuffer(QObject *parent = nullptr);
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;
    qint64 bytesAvailable() const override;
    int bufferedBytes() const;
    void clear();
private:
    static constexpr int kMaxBufferBytes = 48000 * 2 * 2 * 2;
    QByteArray buffer_;
    mutable QMutex mutex_;
};

class QtVideoSink final : public ffplayer::IVideoSink {
public:
    explicit QtVideoSink(QLabel *view = nullptr) : softView_(view) {}
    void setView(QLabel *view) { softView_ = view; }
#if defined(Q_OS_MAC)
    void setMetalHost(MetalVideoViewHost *host) { metalHost_ = host; }
#endif
#if defined(Q_OS_WIN)
    void setD3d11Host(D3d11VideoViewHost *host) { d3dHost_ = host; }
#endif
    bool supportsHwVideo() const override;
    void onVideoFrame(const ffplayer::VideoFrame &frame) override;
    void onHwVideoFrame(const ffplayer::HwVideoFrame &frame) override;
private:
    QLabel *softView_ = nullptr;
#if defined(Q_OS_MAC)
    MetalVideoViewHost *metalHost_ = nullptr;
#endif
#if defined(Q_OS_WIN)
    D3d11VideoViewHost *d3dHost_ = nullptr;
#endif
};

class QtAudioSink final : public ffplayer::IAudioSink {
public:
    QtAudioSink();
    ~QtAudioSink() override;
    bool onAudioChunk(const ffplayer::AudioChunk &chunk) override;
    ffplayer::MediaTimeMs bufferedDurationMs() const override;
    void flush() override;
    void setVolume(int percent);
    bool isReady() const;
private:
    mutable QRecursiveMutex mutex_;
    QAudioOutput *output_ = nullptr;
    QtAudioBuffer *buffer_ = nullptr;
    bool closing_ = false;
    bool ready_ = false;
    int sampleRate_ = 48000;
    int channels_ = 2;
};

class QtSubtitleSink final : public ffplayer::ISubtitleSink {
public:
    explicit QtSubtitleSink(QLabel *label = nullptr) : label_(label) {}
    void setLabel(QLabel *label) { label_ = label; }
    void onSubtitle(const std::string &text,
                    ffplayer::MediaTimeMs startMs,
                    ffplayer::MediaTimeMs endMs) override;
    void onSubtitleImage(const ffplayer::SubtitleImage &image) override;
    void onSubtitleClear() override;
private:
    static QString sanitizeSubtitle(const std::string &text);
    void showForDuration(const QString &text, const QImage &image, int durationMs);
    QLabel *label_ = nullptr;
};
