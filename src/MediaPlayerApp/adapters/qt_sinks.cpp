#include "qt_sinks.h"
#ifdef Q_OS_MAC
#include "metal_video_view.h"
#endif
#ifdef Q_OS_WIN
#include "d3d11_video_view.h"
#endif
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <QAudioOutput>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QMetaObject>
#include <QPointer>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <algorithm>
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

int QtAudioBuffer::bufferedBytes() const
{
    QMutexLocker lock(&mutex_);
    return buffer_.size();
}

void QtAudioBuffer::clear()
{
    QMutexLocker lock(&mutex_);
    buffer_.clear();
}

bool QtVideoSink::supportsHwVideo() const
{
#if defined(Q_OS_MAC)
    return metalHost_ != nullptr;
#elif defined(Q_OS_WIN)
    return d3dHost_ != nullptr;
#else
    return false;
#endif
}

void QtVideoSink::onHwVideoFrame(const ffplayer::HwVideoFrame &frame)
{
#if defined(Q_OS_MAC)
    if (!metalHost_ || frame.backend != ffplayer::HwVideoBackend::VideoToolbox || !frame.nativeHandle) {
        return;
    }
    auto keep = frame.keepAlive;
    void *handle = frame.nativeHandle;
    MetalVideoViewHost *host = metalHost_;
    QTimer::singleShot(0, host, [host, keep, handle] {
        if (!host) return;
        host->setPixelBuffer(handle);
        (void)keep;
    });
#elif defined(Q_OS_WIN)
    if (!d3dHost_ || frame.backend != ffplayer::HwVideoBackend::D3D11 || !frame.nativeHandle) {
        return;
    }
    auto keep = frame.keepAlive;
    void *tex = frame.nativeHandle;
    const int slice = frame.subresourceIndex;
    D3d11VideoViewHost *host = d3dHost_;
    QTimer::singleShot(0, host, [host, keep, tex, slice] {
        if (!host) return;
        host->setDecodeTexture(tex, slice, keep);
    });
#else
    (void)frame;
#endif
}

void QtVideoSink::onVideoFrame(const ffplayer::VideoFrame &frame)
{
    if (!softView_ || !frame.data[0] || frame.width <= 0 || frame.height <= 0)
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
    QPointer<QLabel> view = softView_;
    QMetaObject::invokeMethod(softView_, [view, image] {
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

    const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
    if (device.isNull()) {
        ready_ = false;
        return;
    }
    if (!device.isFormatSupported(format)) {
        format = device.nearestFormat(format);
        // SDK always emits 48kHz stereo S16LE — refuse mismatched devices rather than play garbage.
        if (format.sampleRate() != 48000 || format.channelCount() != 2 ||
            format.sampleSize() != 16 || format.sampleType() != QAudioFormat::SignedInt) {
            ready_ = false;
            return;
        }
    }

    sampleRate_ = format.sampleRate();
    channels_ = format.channelCount();
    output_ = new QAudioOutput(device, format);
    output_->setVolume(1.0);
    buffer_ = new QtAudioBuffer(output_);
    output_->start(buffer_);
    ready_ = output_->error() == QAudio::NoError ||
             output_->state() == QAudio::ActiveState ||
             output_->state() == QAudio::IdleState;

    QObject::connect(output_, &QAudioOutput::stateChanged,
                     [this](QAudio::State state) {
        QMutexLocker lock(&mutex_);
        if (closing_ || !output_ || !buffer_) return;
        if ((state == QAudio::IdleState || state == QAudio::StoppedState) &&
            buffer_->bufferedBytes() > 0) {
            output_->start(buffer_);
        }
    });
}

QtAudioSink::~QtAudioSink()
{
    QMutexLocker lock(&mutex_);
    closing_ = true;
    ready_ = false;
    if (output_) {
        output_->stop();
        delete output_;
        output_ = nullptr;
        buffer_ = nullptr;
    }
}

bool QtAudioSink::isReady() const
{
    QMutexLocker lock(&mutex_);
    return ready_ && output_ && buffer_;
}

void QtAudioSink::setVolume(int percent)
{
    QMutexLocker lock(&mutex_);
    if (output_) output_->setVolume(qBound(0, percent, 100) / 100.0);
}

void QtAudioSink::flush()
{
    QMutexLocker lock(&mutex_);
    if (closing_ || !output_ || !buffer_) return;
    buffer_->clear();
    output_->stop();
    output_->start(buffer_);
}

ffplayer::MediaTimeMs QtAudioSink::bufferedDurationMs() const
{
    QMutexLocker lock(&mutex_);
    if (!buffer_ || sampleRate_ <= 0 || channels_ <= 0) return 0;
    const int bytesPerSecond = sampleRate_ * channels_ * 2;
    return static_cast<ffplayer::MediaTimeMs>(buffer_->bufferedBytes()) * 1000 / bytesPerSecond;
}

bool QtAudioSink::onAudioChunk(const ffplayer::AudioChunk &chunk)
{
    if (!chunk.data || chunk.size <= 0) return false;

    int offset = 0;
    while (offset < chunk.size) {
        qint64 written = 0;
        {
            QMutexLocker lock(&mutex_);
            if (closing_) return offset > 0;
            // Device unavailable — reject so the SDK does not advance the audio clock.
            if (!ready_ || !buffer_) return false;
            written = buffer_->writeData(
                reinterpret_cast<const char *>(chunk.data) + offset,
                chunk.size - offset);
        }
        if (written > 0) {
            offset += static_cast<int>(written);
            continue;
        }
        {
            QMutexLocker lock(&mutex_);
            // Closing after a partial write: treat as success so the SDK does not
            // resubmit the entire chunk and duplicate already-buffered samples.
            if (closing_) return offset > 0;
        }
        // Ring buffer full — wait for the device to drain before retrying remainder.
        QThread::msleep(5);
    }
    return true;
}

QString QtSubtitleSink::sanitizeSubtitle(const std::string &text)
{
    QString result = QString::fromUtf8(text.c_str());
    result.replace(QRegularExpression(QStringLiteral(R"(\{[^}]*\})")), QString());
    result.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
    result.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    result.replace(QStringLiteral("\\h"), QStringLiteral(" "));
    const int dialogue = result.indexOf(QStringLiteral("Dialogue:"), 0, Qt::CaseInsensitive);
    if (dialogue >= 0) {
        int commas = 0;
        int pos = dialogue;
        while (pos < result.size() && commas < 9) {
            if (result.at(pos) == QLatin1Char(',')) ++commas;
            ++pos;
        }
        if (commas >= 9) result = result.mid(pos);
    }
    return result.trimmed();
}

void QtSubtitleSink::showForDuration(const QString &text, const QImage &image, int durationMs)
{
    if (!label_) return;
    durationMs = std::max(500, durationMs);
    QPointer<QLabel> label = label_;
    QMetaObject::invokeMethod(label_, [label, text, image, durationMs] {
        if (!label) return;
        const int token = label->property("subToken").toInt() + 1;
        label->setProperty("subToken", token);
        if (!image.isNull()) {
            label->setPixmap(QPixmap::fromImage(image));
            label->setText(QString());
        } else {
            label->setPixmap(QPixmap());
            label->setText(text);
        }
        label->setVisible(!text.isEmpty() || !image.isNull());
        if (text.isEmpty() && image.isNull()) return;
        QTimer::singleShot(durationMs, label, [label, token] {
            if (!label || label->property("subToken").toInt() != token) return;
            label->clear();
            label->setPixmap(QPixmap());
            label->setVisible(false);
        });
    }, Qt::QueuedConnection);
}

void QtSubtitleSink::onSubtitle(const std::string &text,
                                ffplayer::MediaTimeMs startMs,
                                ffplayer::MediaTimeMs endMs)
{
    const QString display = sanitizeSubtitle(text);
    int durationMs = 3000;
    if (endMs > startMs) {
        durationMs = static_cast<int>(std::min<ffplayer::MediaTimeMs>(endMs - startMs, 15000));
    }
    showForDuration(display, QImage(), durationMs);
}

void QtSubtitleSink::onSubtitleImage(const ffplayer::SubtitleImage &image)
{
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return;
    }
    QImage qimage(image.width, image.height, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(qimage.scanLine(y),
                    image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4,
                    static_cast<std::size_t>(image.width) * 4);
    }
    int durationMs = 3000;
    if (image.endMs > image.startMs) {
        durationMs = static_cast<int>(std::min<ffplayer::MediaTimeMs>(image.endMs - image.startMs, 15000));
    }
    showForDuration(QString(), qimage, durationMs);
}

void QtSubtitleSink::onSubtitleClear()
{
    if (!label_) return;
    QPointer<QLabel> label = label_;
    QMetaObject::invokeMethod(label_, [label] {
        if (!label) return;
        label->setProperty("subToken", label->property("subToken").toInt() + 1);
        label->clear();
        label->setPixmap(QPixmap());
        label->setVisible(false);
    }, Qt::QueuedConnection);
}
