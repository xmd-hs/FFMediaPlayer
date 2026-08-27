#pragma once

#include <QMainWindow>
#include <QTimer>

#include "../../sdk/include/player.h"
#include "adapters/qt_sinks.h"

class QLabel;
class QPushButton;
class QSlider;
class QComboBox;
class QCheckBox;
class MetalVideoViewHost;
class D3d11VideoViewHost;

class PlayerWindow final : public QMainWindow {
public:
    explicit PlayerWindow(QWidget *parent = nullptr);
    ~PlayerWindow() override;

private:
    void openFile();
    void togglePlayback();
    void refreshTrackControls();

    QLabel *videoView_ = nullptr;
#if defined(Q_OS_MAC)
    MetalVideoViewHost *metalHost_ = nullptr;
#endif
#if defined(Q_OS_WIN)
    D3d11VideoViewHost *d3dHost_ = nullptr;
#endif
    QLabel *subtitleLabel_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QSlider *progress_ = nullptr;
    QSlider *volume_ = nullptr;
    QComboBox *speedBox_ = nullptr;
    QComboBox *audioTrackBox_ = nullptr;
    QComboBox *subtitleTrackBox_ = nullptr;
    QCheckBox *hwAccelBox_ = nullptr;
    QTimer timer_;
    ffplayer::Player player_;
    QtVideoSink videoSink_;
    QtAudioSink audioSink_;
    QtSubtitleSink subtitleSink_;
};
