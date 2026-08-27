#include "player_window.h"
#ifdef Q_OS_MAC
#include "adapters/metal_video_view.h"
#endif
#ifdef Q_OS_WIN
#include "adapters/d3d11_video_view.h"
#endif
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QMetaObject>
#include <algorithm>
#include <climits>

PlayerWindow::PlayerWindow(QWidget *parent) : QMainWindow(parent)
{
    player_.setVideoSink(&videoSink_);
    player_.setAudioSink(&audioSink_);
    player_.setSubtitleSink(&subtitleSink_);
    player_.setStateCallback([this](ffplayer::PlaybackState state) {
        QMetaObject::invokeMethod(this, [this, state] {
            const bool playing = state == ffplayer::PlaybackState::Playing;
            playButton_->setText(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
            switch (state) {
            case ffplayer::PlaybackState::Playing:
                status_->setText(QStringLiteral("播放中"));
                break;
            case ffplayer::PlaybackState::Paused:
                status_->setText(QStringLiteral("已暂停"));
                break;
            case ffplayer::PlaybackState::Buffering:
                status_->setText(QStringLiteral("缓冲中"));
                break;
            case ffplayer::PlaybackState::Seeking:
                status_->setText(QStringLiteral("跳转中"));
                break;
            case ffplayer::PlaybackState::Ended:
                status_->setText(QStringLiteral("播放结束"));
                break;
            case ffplayer::PlaybackState::Error:
                status_->setText(QStringLiteral("错误"));
                break;
            default:
                status_->setText(QStringLiteral("就绪"));
                break;
            }
        }, Qt::QueuedConnection);
    });
    player_.setErrorCallback([this](const std::string &message) {
        const QString text = QString::fromUtf8(message.c_str());
        QMetaObject::invokeMethod(this, [this, text] {
            status_->setText(QStringLiteral("错误: ") + text);
            playButton_->setText(QStringLiteral("播放"));
        }, Qt::QueuedConnection);
    });
    player_.setFinishedCallback([this] {
        QMetaObject::invokeMethod(this, [this] {
            status_->setText(QStringLiteral("播放结束"));
            playButton_->setText(QStringLiteral("播放"));
            if (subtitleLabel_) {
                subtitleLabel_->clear();
                subtitleLabel_->setVisible(false);
            }
        }, Qt::QueuedConnection);
    });
    setWindowTitle(QStringLiteral("FFMediaPlayer"));
    resize(1280, 800);
    setStyleSheet("QMainWindow{background:#050505;color:#f0f0f0;} QWidget{background:#050505;color:#f0f0f0;} QPushButton{background:#171717;color:#f0f0f0;border:1px solid #383838;border-radius:5px;padding:8px 14px;} QPushButton:hover{background:#252525;border-color:#3d8bfd;} QComboBox{background:#171717;color:#f0f0f0;border:1px solid #383838;border-radius:5px;padding:6px 10px;} QSlider::groove:horizontal{height:4px;background:#303030;} QSlider::sub-page:horizontal{background:#3d8bfd;} QSlider::handle:horizontal{width:14px;margin:-5px 0;background:#ff7b22;border-radius:7px;}");
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

    auto *videoHost = new QWidget(root);
    videoHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoHost->setMinimumSize(640, 360);
    auto *videoStack = new QStackedLayout(videoHost);
    videoStack->setStackingMode(QStackedLayout::StackAll);
#ifdef Q_OS_MAC
    metalHost_ = new MetalVideoViewHost(videoHost);
    metalHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    metalHost_->setMinimumSize(640, 360);
    videoStack->addWidget(metalHost_);
    videoSink_.setMetalHost(metalHost_);
#endif
#ifdef Q_OS_WIN
    d3dHost_ = new D3d11VideoViewHost(videoHost);
    d3dHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    d3dHost_->setMinimumSize(640, 360);
    videoStack->addWidget(d3dHost_);
    videoSink_.setD3d11Host(d3dHost_);
#endif
    videoView_ = new QLabel(QStringLiteral("打开文件开始播放"), videoHost);
    videoView_->setAlignment(Qt::AlignCenter);
    videoView_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    videoView_->setAttribute(Qt::WA_TransparentForMouseEvents);
    videoView_->setStyleSheet("background:transparent;color:#aaa;");
#else
    videoView_->setStyleSheet("background:#000;border:1px solid #242424;border-radius:6px;color:#aaa;");
#endif
    subtitleLabel_ = new QLabel(videoHost);
    subtitleLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    subtitleLabel_->setWordWrap(true);
    subtitleLabel_->setVisible(false);
    subtitleLabel_->setStyleSheet(
        "QLabel{background:transparent;color:#f5f5f5;font-size:20px;font-weight:600;"
        "padding:18px 28px 28px 28px;text-shadow:0 1px 2px #000;}");
    videoStack->addWidget(videoView_);
    videoStack->addWidget(subtitleLabel_);
    videoSink_.setView(videoView_);
    subtitleSink_.setLabel(subtitleLabel_);
    content->addWidget(videoHost, 1);
    layout->addLayout(content, 1);
    progress_ = new QSlider(Qt::Horizontal, root);
    progress_->setRange(0, 0);
    layout->addWidget(progress_);

    auto *controls = new QHBoxLayout;
    auto *openButton = new QPushButton(QStringLiteral("打开文件"), root);
    playButton_ = new QPushButton(QStringLiteral("播放"), root);
    status_ = new QLabel(QStringLiteral("就绪"), root);
    speedBox_ = new QComboBox(root);
    speedBox_->addItem(QStringLiteral("0.5x"), 0.5);
    speedBox_->addItem(QStringLiteral("0.75x"), 0.75);
    speedBox_->addItem(QStringLiteral("1.0x"), 1.0);
    speedBox_->addItem(QStringLiteral("1.25x"), 1.25);
    speedBox_->addItem(QStringLiteral("1.5x"), 1.5);
    speedBox_->addItem(QStringLiteral("2.0x"), 2.0);
    speedBox_->setCurrentIndex(2);
    volume_ = new QSlider(Qt::Horizontal, root);
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(120);

    controls->addWidget(openButton);
    controls->addWidget(playButton_);
    controls->addWidget(status_);
    controls->addStretch();
    controls->addWidget(new QLabel(QStringLiteral("倍速"), root));
    controls->addWidget(speedBox_);
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
            const auto position = player_.position();
            const auto duration = player_.duration();
            if (duration > 0 && progress_->maximum() != static_cast<int>(duration)) {
                progress_->setRange(0, static_cast<int>(std::min<ffplayer::MediaTimeMs>(duration, INT_MAX)));
            }
            progress_->setValue(static_cast<int>(std::min<ffplayer::MediaTimeMs>(position, INT_MAX)));
        }
    });
    timer_.start();
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        player_.setVolume(value / 100.0f);
    });
    connect(speedBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        const double speed = speedBox_->itemData(index).toDouble();
        if (speed > 0.0) player_.setSpeed(speed);
    });
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

    if (!audioSink_.isReady()) {
        status_->setText(QStringLiteral("警告: 音频设备不可用，将仅尝试视频"));
    }

    progress_->setRange(0, static_cast<int>(player_.duration()));
    if (player_.videoHwAccelActive()) {
        status_->setText(QStringLiteral("硬解零拷贝"));
        videoView_->clear();
#ifdef Q_OS_MAC
        if (metalHost_) metalHost_->clearFrame();
#endif
#ifdef Q_OS_WIN
        if (d3dHost_) d3dHost_->clearFrame();
#endif
    } else {
        status_->setText(QStringLiteral("软解"));
        videoView_->setText(path);
    }
    if (subtitleLabel_) {
        subtitleLabel_->clear();
        subtitleLabel_->setVisible(false);
    }
    player_.setSpeed(speedBox_->currentData().toDouble());
    player_.setVolume(volume_->value() / 100.0f);
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
