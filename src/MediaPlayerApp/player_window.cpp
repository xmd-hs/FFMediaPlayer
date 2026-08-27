#include "player_window.h"
#ifdef Q_OS_MAC
#include "adapters/metal_video_view.h"
#endif
#ifdef Q_OS_WIN
#include "adapters/d3d11_video_view.h"
#endif
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QMetaObject>
#include <QSignalBlocker>
#include <algorithm>
#include <climits>

namespace {

QString trackLabel(const ffplayer::TrackInfo& track, const QString& fallback)
{
    QStringList parts;
    if (!track.language.empty()) parts << QString::fromUtf8(track.language.c_str()).toUpper();
    if (!track.title.empty()) parts << QString::fromUtf8(track.title.c_str());
    if (!track.codec.empty()) parts << QString::fromUtf8(track.codec.c_str()).toUpper();
    return parts.isEmpty() ? fallback : parts.join(QStringLiteral(" · "));
}

class SeekSlider final : public QSlider {
public:
    explicit SeekSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        setSliderDown(true);
        setValue(valueFromPosition(event->pos()));
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!isSliderDown()) {
            QSlider::mouseMoveEvent(event);
            return;
        }
        setValue(valueFromPosition(event->pos()));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !isSliderDown()) {
            QSlider::mouseReleaseEvent(event);
            return;
        }
        setValue(valueFromPosition(event->pos()));
        setSliderDown(false);
        emit sliderReleased();
        event->accept();
    }

private:
    int valueFromPosition(const QPoint& position) const
    {
        const int span = orientation() == Qt::Horizontal ? width() : height();
        const int coordinate = orientation() == Qt::Horizontal ? position.x() : position.y();
        return QStyle::sliderValueFromPosition(minimum(), maximum(), coordinate, span,
                                               invertedAppearance());
    }
};

} // namespace

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
        }, Qt::AutoConnection);
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
    d3dHost_->hide();
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
        "padding:18px 28px 28px 28px;}");
    videoStack->addWidget(videoView_);
    videoStack->addWidget(subtitleLabel_);
    videoSink_.setView(videoView_);
    subtitleSink_.setLabel(subtitleLabel_);
    content->addWidget(videoHost, 1);
    layout->addLayout(content, 1);
    progress_ = new SeekSlider(Qt::Horizontal, root);
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
    hwAccelBox_ = new QCheckBox(QStringLiteral("硬件解码"), root);
    hwAccelBox_->setChecked(player_.hwAccelEnabled());
    audioTrackBox_ = new QComboBox(root);
    audioTrackBox_->setMinimumContentsLength(8);
    audioTrackBox_->setEnabled(false);
    subtitleTrackBox_ = new QComboBox(root);
    subtitleTrackBox_->setMinimumContentsLength(8);
    subtitleTrackBox_->setEnabled(false);
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
    controls->addWidget(hwAccelBox_);
    controls->addWidget(new QLabel(QStringLiteral("音轨"), root));
    controls->addWidget(audioTrackBox_);
    controls->addWidget(new QLabel(QStringLiteral("字幕"), root));
    controls->addWidget(subtitleTrackBox_);
    controls->addWidget(new QLabel(QStringLiteral("音量"), root));
    controls->addWidget(volume_);
    layout->addLayout(controls);
    setCentralWidget(root);

    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::openFile);
    connect(playButton_, &QPushButton::clicked, this, &PlayerWindow::togglePlayback);
    connect(progress_, &QSlider::sliderReleased, this, [this] {
        if (!player_.seek(progress_->value())) {
            progress_->setValue(static_cast<int>(std::min<ffplayer::MediaTimeMs>(
                player_.position(), INT_MAX)));
            status_->setText(QStringLiteral("跳转失败"));
        }
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
    connect(hwAccelBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        player_.setHwAccelEnabled(enabled);
        const bool active = player_.videoHwAccelActive();
        status_->setText(active ? QStringLiteral("硬解零拷贝") : QStringLiteral("软解"));
#ifdef Q_OS_WIN
        if (d3dHost_) d3dHost_->setVisible(active);
        if (videoView_) videoView_->setVisible(!active);
#endif
    });
    connect(audioTrackBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0) return;
        const int stream = audioTrackBox_->itemData(index).toInt();
        if (!player_.selectAudioTrack(stream)) {
            status_->setText(QStringLiteral("音轨切换失败"));
            refreshTrackControls();
        }
    });
    connect(subtitleTrackBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0) return;
        const int stream = subtitleTrackBox_->itemData(index).toInt();
        if (!player_.selectSubtitleTrack(stream)) {
            status_->setText(QStringLiteral("字幕切换失败"));
            refreshTrackControls();
        }
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

    progress_->setRange(0, static_cast<int>(std::min<ffplayer::MediaTimeMs>(
        player_.duration(), INT_MAX)));
    if (player_.videoHwAccelActive()) {
        status_->setText(QStringLiteral("硬解零拷贝"));
        videoView_->clear();
#ifdef Q_OS_MAC
        if (metalHost_) metalHost_->clearFrame();
#endif
#ifdef Q_OS_WIN
        if (d3dHost_) {
            d3dHost_->show();
            d3dHost_->raise();
            d3dHost_->clearFrame();
        }
#endif
    } else {
        status_->setText(QStringLiteral("软解"));
#ifdef Q_OS_WIN
        if (d3dHost_) d3dHost_->hide();
#endif
        videoView_->show();
        videoView_->raise();
        videoView_->setText(path);
    }
    if (subtitleLabel_) {
        subtitleLabel_->clear();
        subtitleLabel_->setVisible(false);
    }
    player_.setSpeed(speedBox_->currentData().toDouble());
    player_.setVolume(volume_->value() / 100.0f);
    refreshTrackControls();
    player_.play();
}

void PlayerWindow::refreshTrackControls()
{
    const QSignalBlocker audioBlocker(audioTrackBox_);
    const QSignalBlocker subtitleBlocker(subtitleTrackBox_);

    audioTrackBox_->clear();
    const auto audioTracks = player_.audioTracks();
    for (std::size_t i = 0; i < audioTracks.size(); ++i) {
        const auto& track = audioTracks[i];
        audioTrackBox_->addItem(
            trackLabel(track, QStringLiteral("音轨 %1").arg(i + 1)), track.streamIndex);
    }
    const int selectedAudio = player_.selectedAudioTrack();
    const int audioIndex = audioTrackBox_->findData(selectedAudio);
    if (audioIndex >= 0) audioTrackBox_->setCurrentIndex(audioIndex);
    audioTrackBox_->setEnabled(audioTrackBox_->count() > 1);

    subtitleTrackBox_->clear();
    subtitleTrackBox_->addItem(QStringLiteral("关闭"), -1);
    const auto subtitleTracks = player_.subtitleTracks();
    for (std::size_t i = 0; i < subtitleTracks.size(); ++i) {
        const auto& track = subtitleTracks[i];
        subtitleTrackBox_->addItem(
            trackLabel(track, QStringLiteral("字幕 %1").arg(i + 1)), track.streamIndex);
    }
    const int selectedSubtitle = player_.selectedSubtitleTrack();
    const int subtitleIndex = subtitleTrackBox_->findData(selectedSubtitle);
    subtitleTrackBox_->setCurrentIndex(subtitleIndex >= 0 ? subtitleIndex : 0);
    subtitleTrackBox_->setEnabled(subtitleTrackBox_->count() > 1);
}

void PlayerWindow::togglePlayback()
{
    if (player_.state() == ffplayer::PlaybackState::Playing) {
        player_.pause();
        return;
    }
    player_.play();
}
