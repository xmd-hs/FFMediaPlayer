#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QImage>
#include <QIODevice>
#include <QByteArray>
#include <QMutex>

#include "../../sdk/include/player.h"

class QLabel; class QPushButton; class QSlider;
class QAudioOutput;
class QIODevice;
class QMutex;
class QWaitCondition;

#include "adapters/qt_sinks.h"

class PlayerWindow final : public QMainWindow {
public:
    explicit PlayerWindow(QWidget *parent = nullptr);
    ~PlayerWindow() override;

private:
    void openFile();
    void togglePlayback();
    QLabel *videoView_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QSlider *progress_ = nullptr;
    QSlider *volume_ = nullptr;
    QTimer timer_;
    ffplayer::Player player_;
    QtVideoSink videoSink_;
    QtAudioSink audioSink_;
};
