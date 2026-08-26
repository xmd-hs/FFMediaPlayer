#pragma once

#include <QMainWindow>
#include <QTimer>

#include "../../../sdk/include/player.h"

class QLabel; class QPushButton; class QSlider;

class PlayerWindow final : public QMainWindow {
public: explicit PlayerWindow(QWidget *parent = nullptr);

private: void openFile(); void togglePlayback();
    QLabel *videoView_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QSlider *progress_ = nullptr;
    QTimer timer_;
    ffplayer::Player player_;
};
