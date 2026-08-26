#include "player_window.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

PlayerWindow::PlayerWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("FFMediaPlayer"));
    resize(1280, 800);
    setStyleSheet(QStringLiteral("QMainWindow{background:#000;color:#f5f5f5;} QWidget{background:#000;} QPushButton{background:#111;color:#f5f5f5;border:1px solid #303030;border-radius:4px;padding:8px 14px;} QPushButton:hover{background:#1d1d1d;border-color:#4a8dff;} QPushButton:pressed{background:#164b9b;} QSlider::groove:horizontal{height:4px;background:#252525;border-radius:2px;} QSlider::sub-page:horizontal{background:#3d8bfd;} QSlider::handle:horizontal{width:14px;margin:-5px 0;background:#ff7b22;border-radius:7px;}"));

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *content = new QHBoxLayout;
    auto *sidebar = new QVBoxLayout;
    for (const char *label : {"Menu", "Search", "Home", "Music", "Video", "List", "Settings"})
        sidebar->addWidget(new QPushButton(QString::fromLatin1(label), root));
    sidebar->addStretch();
    content->addLayout(sidebar);

    videoView_ = new QLabel(QStringLiteral("打开文件开始播放"), root);
    videoView_->setAlignment(Qt::AlignCenter);
    videoView_->setStyleSheet(QStringLiteral("background:#000;border:1px solid #242424;border-radius:6px;font-size:18px;color:#9b9b9b;"));
    content->addWidget(videoView_, 1);
    layout->addLayout(content, 1);

    progress_ = new QSlider(Qt::Horizontal, root);
    progress_->setRange(0, 0);
    layout->addWidget(progress_);

    auto *controls = new QHBoxLayout;
    auto *openButton = new QPushButton(QStringLiteral("打开文件"), root);
    playButton_ = new QPushButton(QStringLiteral("播放"), root);
    status_ = new QLabel(QStringLiteral("就绪"), root);
    controls->addWidget(openButton); controls->addWidget(playButton_); controls->addWidget(status_); controls->addStretch();
    layout->addLayout(controls);
    setCentralWidget(root);

    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::openFile);
    connect(playButton_, &QPushButton::clicked, this, &PlayerWindow::togglePlayback);
    connect(progress_, &QSlider::sliderMoved, this, [this](int position) { player_.seek(position); });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] { progress_->setValue(static_cast<int>(player_.position())); });
    timer_.start();
}

void PlayerWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开文件"));
    if (path.isEmpty() || !player_.open(path.toStdString())) return;
    progress_->setRange(0, static_cast<int>(player_.duration()));
    player_.play(); videoView_->setText(path); playButton_->setText(QStringLiteral("暂停"));
}

void PlayerWindow::togglePlayback()
{
    const bool playing = player_.state() == ffplayer::PlaybackState::Playing;
    if (playing) { player_.pause(); playButton_->setText(QStringLiteral("播放")); }
    else { player_.play(); playButton_->setText(QStringLiteral("暂停")); }
}
