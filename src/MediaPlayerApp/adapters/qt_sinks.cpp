#include "qt_sinks.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QAudioOutput>
#include <QAudioFormat>
#include <QBuffer>
#include <QMetaObject>
#include <QPointer>
#include <QMutexLocker>
#include <cstring>

QtAudioBuffer::QtAudioBuffer(QObject *parent)
    : QIODevice(parent)
{
    open(QIODevice::ReadOnly);
}

qint64 QtAudioBuffer::readData(char *data, qint64 maxSize)
{
    QMutexLocker lock(&mutex_);
    const qint64 count = qMin(maxSize, static_cast<qint64>(buffer_.size()));
    if (count > 0) {
        memcpy(data, buffer_.constData(), static_cast<size_t>(count));
        buffer_.remove(0, static_cast<int>(count));
    }
    return count;
}

qint64 QtAudioBuffer::writeData(const char *data, qint64 size)
{
    if (!data || size <= 0) {
        return 0;
    }

    int count = 0;
    {
        QMutexLocker lock(&mutex_);
        const int available = qMax(0, kMaxBufferBytes - buffer_.size());
        count = qMin(static_cast<int>(size), available);
        if (count > 0) {
            buffer_.append(data, count);
        }
    }

    if (count > 0) {
        QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
    }
    return count;
}

qint64 QtAudioBuffer::bytesAvailable() const
{
    QMutexLocker lock(&mutex_);
    return buffer_.size() + QIODevice::bytesAvailable();
}

void QtAudioBuffer::clear()
{
    QMutexLocker lock(&mutex_);
    buffer_.clear();
}

void QtVideoSink::onVideoFrame(const ffplayer::VideoFrame &frame)
{
    if (!view_ || !frame.data[0] || frame.width <= 0 || frame.height <= 0)
        return;
    QImage image(frame.width, frame.height, QImage::Format_RGB32);
    for (int y = 0; y < frame.height; ++y) {
        auto *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
        const auto *yPlane = frame.data[0] + y * frame.linesize[0];
        const auto *uPlane = frame.data[1] + (y / 2) * frame.linesize[1];
        const auto *vPlane = frame.data[2] + (y / 2) * frame.linesize[2];
        for (int x = 0; x < frame.width; ++x) {
            const int Y = yPlane[x] - 16;
            const int U = uPlane[x / 2] - 128;
            const int V = vPlane[x / 2] - 128;
            const int r = qBound(0, (298 * Y + 409 * V + 128) / 256, 255);
            const int g = qBound(0, (298 * Y - 100 * U - 208 * V + 128) / 256, 255);
            const int b = qBound(0, (298 * Y + 516 * U + 128) / 256, 255);
            dst[x] = qRgb(r, g, b);
        }
    }
    QPointer<QLabel> view = view_;
    QMetaObject::invokeMethod(view_, [view, image] {
        if (!view) {
            return;
        }
        view->setPixmap(QPixmap::fromImage(image).scaled(
            view->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }, Qt::QueuedConnection);
}

QtAudioSink::QtAudioSink()
{
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    output_ = new QAudioOutput(format);
    output_->setVolume(1.0);
    buffer_ = new QtAudioBuffer(output_);
    output_->start(buffer_);
    device_ = buffer_;
    QObject::connect(output_, &QAudioOutput::stateChanged,
                     [this](QAudio::State state) {
        if ((state == QAudio::IdleState || state == QAudio::StoppedState) &&
            buffer_ && buffer_->bytesAvailable() > 0) {
            output_->start(buffer_);
        }
    });
}

QtAudioSink::~QtAudioSink() { delete output_; }
void QtAudioSink::setVolume(int percent) { if (output_) output_->setVolume(qBound(0, percent, 100) / 100.0); }

void QtAudioSink::flush()
{
    if (buffer_) {
        buffer_->clear();
    }
    if (output_ && buffer_) {
        output_->stop();
        output_->start(buffer_);
    }
}

bool QtAudioSink::onAudioChunk(const ffplayer::AudioChunk &chunk)
{
    if (!output_ || !chunk.data || chunk.size <= 0)
        return false;
    if (!device_)
        return false;
    return buffer_->writeData(reinterpret_cast<const char *>(chunk.data), chunk.size) == chunk.size;
}
