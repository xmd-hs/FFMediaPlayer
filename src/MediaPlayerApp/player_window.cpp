#include "player_window.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QMetaObject>

PlayerWindow::PlayerWindow(QWidget *parent) : QMainWindow(parent)
{
    player_.setVideoSink(&videoSink_);
    player_.setAudioSink(&audioSink_);
    player_.setStateCallback([this](ffplayer::PlaybackState state) {
        QMetaObject::invokeMethod(this, [this, state] {
            const bool playing = state == ffplayer::PlaybackState::Playing;
            playButton_->setText(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
            status_->setText(playing ? QStringLiteral("播放中") : QStringLiteral("已暂停"));
        }, Qt::QueuedConnection);
    });
    setWindowTitle(QStringLiteral("FFMediaPlayer"));
    resize(1280, 800);
    setStyleSheet("QMainWindow{background:#050505;color:#f0f0f0;} QWidget{background:#050505;color:#f0f0f0;} QPushButton{background:#171717;color:#f0f0f0;border:1px solid #383838;border-radius:5px;padding:8px 14px;} QPushButton:hover{background:#252525;border-color:#3d8bfd;} QSlider::groove:horizontal{height:4px;background:#303030;} QSlider::sub-page:horizontal{background:#3d8bfd;} QSlider::handle:horizontal{width:14px;margin:-5px 0;background:#ff7b22;border-radius:7px;}");
    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *content = new QHBoxLayout;
    auto *sidebar = new QVBoxLayout;
    const QStringList navigationLabels = {
        QStringLiteral("\u83dc\u5355"), QStringLiteral("\u641c\u7d22"),
        QStringLiteral("\u9996\u9875"), QStringLiteral("\u97f3\u4e50"),
        QStringLiteral("\u89c6\u9891"), QStringLiteral("\u5217\u8868"),
        QStringLiteral("\u8bbe\u7f6e")};
    for (const QString &label : navigationLabels) {
        sidebar->addWidget(new QPushButton(label, root));
    }
    sidebar->addStretch(); content->addLayout(sidebar);
    videoView_ = new QLabel(QStringLiteral("打开文件开始播放"), root);
    videoView_->setAlignment(Qt::AlignCenter);
    videoView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoView_->setMinimumSize(640, 360);
    videoView_->setStyleSheet("background:#000;border:1px solid #242424;border-radius:6px;color:#aaa;");
    videoSink_.setView(videoView_);
    content->addWidget(videoView_, 1);
    layout->addLayout(content, 1);
    progress_ = new QSlider(Qt::Horizontal, root);
    progress_->setRange(0, 0);
    layout->addWidget(progress_);

    auto *controls = new QHBoxLayout;
    auto *openButton = new QPushButton(QStringLiteral("打开文件"), root);
    playButton_ = new QPushButton(QStringLiteral("播放"), root);
    status_ = new QLabel(QStringLiteral("就绪"), root);
    volume_ = new QSlider(Qt::Horizontal, root);
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(120);

    controls->addWidget(openButton);
    controls->addWidget(playButton_);
    controls->addWidget(status_);
    controls->addStretch();
    controls->addWidget(new QLabel(QStringLiteral("音量"), root));
    controls->addWidget(volume_);
    layout->addLayout(controls);
    setCentralWidget(root);

    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::openFile);
    connect(playButton_, &QPushButton::clicked, this, &PlayerWindow::togglePlayback);
    connect(progress_, &QSlider::sliderReleased, this, [this] {
        player_.seek(progress_->value());
    });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (!progress_->isSliderDown()) {
            progress_->setValue(static_cast<int>(player_.position()));
        }
    });
    timer_.start();
    connect(volume_, &QSlider::valueChanged, this, [this](int value) { audioSink_.setVolume(value); });
}
PlayerWindow::~PlayerWindow()
{
    player_.close();
}

void PlayerWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开文件"));
    if (path.isEmpty() || !player_.open(path.toStdString())) {
        return;
    }

    progress_->setRange(0, static_cast<int>(player_.duration()));
    videoView_->setText(path);
    player_.play();
}

void PlayerWindow::togglePlayback()
{
    if (player_.state() == ffplayer::PlaybackState::Playing) {
        player_.pause();
        return;
    }
    player_.play();
}
