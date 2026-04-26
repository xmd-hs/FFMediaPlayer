#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif
#include "mediaplayer.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "demuxthread.h"

static DemuxThread dt;

MediaPlayer::MediaPlayer(QWidget *parent) : QWidget(parent)
{
	setObjectName("MediaPlayer");
	setStyleSheet("#MediaPlayer{background-color:#1a1a2e}");
	resize(960, 640);
	setMinimumSize(480, 360);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	// 视频显示区域
	videoWidget = new VideoWidget(this);
	videoWidget->setMinimumHeight(200);
	mainLayout->addWidget(videoWidget, 1);

	// 提示标签（未打开文件时显示）
	hintLabel = new QLabel(QString::fromUtf8("请打开文件"), this);
	hintLabel->setAlignment(Qt::AlignCenter);
	hintLabel->setStyleSheet(
		"color:rgba(255,255,255,180);"
		"font-size:28px;"
		"background-color:rgba(26,26,46,220);"
		"border-radius:8px;"
	);

	// 控制面板
	auto *controlPanel = new QWidget(this);
	controlPanel->setObjectName("controlPanel");
	controlPanel->setStyleSheet(
		"#controlPanel{background-color:#16213e;border-top:1px solid rgba(255,255,255,30)}"
	);
	controlPanel->setFixedHeight(90);

	auto *controlLayout = new QVBoxLayout(controlPanel);
	controlLayout->setContentsMargins(12, 4, 12, 8);
	controlLayout->setSpacing(4);

	// 进度条
	playPos = new SeekSlider(this);
	playPos->setRange(0, 999);
	playPos->setPageStep(1);
	playPos->setOrientation(Qt::Horizontal);
	playPos->setStyleSheet(
		"QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,60);border-radius:2px}"
		"QSlider::handle:horizontal{background:#1abc9c;width:14px;height:14px;margin:-5px 0;border-radius:7px}"
		"QSlider::sub-page:horizontal{background:#1abc9c;border-radius:2px}"
	);
	connect(playPos, &SeekSlider::sliderPressed, this, &MediaPlayer::SliderPress);
	connect(playPos, &SeekSlider::sliderReleased, this, &MediaPlayer::SliderRelease);
	controlLayout->addWidget(playPos);

	// 按钮布局
	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(8);
	btnLayout->setContentsMargins(0, 0, 0, 0);

	auto btnStyle = QString(
		"QPushButton{background-color:rgba(255,255,255,30);color:#fff;"
		"border:1px solid rgba(255,255,255,60);border-radius:4px;padding:6px 14px;font-size:13px}"
		"QPushButton:hover{background-color:rgba(255,255,255,60)}"
		"QPushButton:pressed{background-color:rgba(255,255,255,90)}"
	);

	// 打开文件按钮
	openFileBtn = new QPushButton("打开文件", this);
	openFileBtn->setStyleSheet(btnStyle);
	openFileBtn->setFixedHeight(32);
	connect(openFileBtn, &QPushButton::clicked, this, &MediaPlayer::OpenFile);
	btnLayout->addWidget(openFileBtn);

	// 播放/暂停按钮
	isplayBtn = new QPushButton("播 放", this);
	isplayBtn->setStyleSheet(btnStyle);
	isplayBtn->setFixedHeight(32);
	connect(isplayBtn, &QPushButton::clicked, this, &MediaPlayer::PlayOrPause);
	btnLayout->addWidget(isplayBtn);

	btnLayout->addSpacing(12);

	// 倍速控制按钮
	speedDownBtn = new QPushButton("◀◀", this);
	speedDownBtn->setStyleSheet(btnStyle);
	speedDownBtn->setFixedSize(40, 32);
	connect(speedDownBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedDown);
	btnLayout->addWidget(speedDownBtn);

	speedLabel = new QLabel("1.0x", this);
	speedLabel->setAlignment(Qt::AlignCenter);
	speedLabel->setFixedSize(50, 32);
	speedLabel->setStyleSheet(
		"color:#1abc9c;font-size:13px;font-weight:bold;"
		"background-color:rgba(26,188,156,30);border-radius:4px"
	);
	btnLayout->addWidget(speedLabel);

	speedUpBtn = new QPushButton("▶▶", this);
	speedUpBtn->setStyleSheet(btnStyle);
	speedUpBtn->setFixedSize(40, 32);
	connect(speedUpBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedUp);
	btnLayout->addWidget(speedUpBtn);

	speedResetBtn = new QPushButton("1x", this);
	speedResetBtn->setStyleSheet(btnStyle);
	speedResetBtn->setFixedSize(36, 32);
	connect(speedResetBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedReset);
	btnLayout->addWidget(speedResetBtn);

	btnLayout->addSpacing(12);

	// 音量控制
	auto *volIcon = new QLabel("🔊", this);
	volIcon->setFixedSize(20, 32);
	volIcon->setAlignment(Qt::AlignCenter);
	volIcon->setStyleSheet("font-size:14px;background:transparent");
	btnLayout->addWidget(volIcon);

	volumeSlider = new QSlider(Qt::Horizontal, this);
	volumeSlider->setRange(0, 100);
	volumeSlider->setValue(80);
	volumeSlider->setFixedWidth(100);
	volumeSlider->setStyleSheet(
		"QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,60);border-radius:2px}"
		"QSlider::handle:horizontal{background:#3498db;width:12px;height:12px;margin:-4px 0;border-radius:6px}"
		"QSlider::sub-page:horizontal{background:#3498db;border-radius:2px}"
	);
	connect(volumeSlider, &QSlider::valueChanged, this, &MediaPlayer::VolumeChanged);
	btnLayout->addWidget(volumeSlider);

	btnLayout->addStretch();

	// 时间显示
	timeLabel = new QLabel("00:00 / 00:00", this);
	timeLabel->setAlignment(Qt::AlignCenter);
	timeLabel->setStyleSheet("color:rgba(255,255,255,180);font-size:12px;background:transparent");
	timeLabel->setFixedWidth(120);
	btnLayout->addWidget(timeLabel);

	controlLayout->addLayout(btnLayout);
	mainLayout->addWidget(controlPanel);

	// 启动解复用线程
	dt.Start();
	dt.SetVolume(80);
	startTimer(40);
}

MediaPlayer::~MediaPlayer() { dt.Close(); }

void MediaPlayer::SliderPress() { isSliderPress = true; }

void MediaPlayer::SliderRelease()
{
	isSliderPress = false;
	double pos = (double)playPos->value() / (double)playPos->maximum();
	dt.Seek(pos);
}

// 定时器更新 - 刷新进度条和时间显示
void MediaPlayer::timerEvent(QTimerEvent *)
{
	if (hintLabel && hintLabel->isVisible())
		hintLabel->setGeometry(videoWidget->rect());
	if (isSliderPress) return;
	long long total = dt.totalMs;
	if (total > 0)
	{
		long long curPts = dt.GetCurrentPts();
		if (curPts > total) curPts = total;
		if (curPts < 0) curPts = 0;
		double pos = (double)curPts / (double)total;
		if (pos > 1.0) pos = 1.0;
		playPos->setValue((int)(playPos->maximum() * pos));

		int curSec = (int)(curPts / 1000);
		int totSec = (int)(total / 1000);
		QString timeText = QString("%1:%2 / %3:%4")
			.arg(curSec / 60, 2, 10, QChar('0'))
			.arg(curSec % 60, 2, 10, QChar('0'))
			.arg(totSec / 60, 2, 10, QChar('0'))
			.arg(totSec % 60, 2, 10, QChar('0'));
		timeLabel->setText(timeText);
	}
}

// 双击切换全屏
void MediaPlayer::mouseDoubleClickEvent(QMouseEvent *)
{
	isFullScreen() ? showNormal() : showFullScreen();
}

void MediaPlayer::resizeEvent(QResizeEvent *)
{
	if (hintLabel && hintLabel->isVisible())
		hintLabel->setGeometry(videoWidget->rect());
}

void MediaPlayer::PlayOrPause()
{
	bool pause = !dt.isPause;
	SetPause(pause);
	dt.SetPause(pause);
}

void MediaPlayer::SetPause(bool pause)
{
	isplayBtn->setText(pause ? "播 放" : "暂 停");
}

void MediaPlayer::VolumeChanged(int val) { dt.SetVolume(val); }

void MediaPlayer::SpeedUp()
{
	double s = dt.GetSpeed();
	if (s < 4.0) { s += 0.5; dt.SetSpeed(s); speedLabel->setText(QString::number(s, 'f', 1) + "x"); }
}

void MediaPlayer::SpeedDown()
{
	double s = dt.GetSpeed();
	if (s > 0.5) { s -= 0.5; dt.SetSpeed(s); speedLabel->setText(QString::number(s, 'f', 1) + "x"); }
}

void MediaPlayer::SpeedReset() { dt.SetSpeed(1.0); speedLabel->setText("1.0x"); }

void MediaPlayer::OpenFile()
{
	QString name = QFileDialog::getOpenFileName(
		this, "选择视频文件", QString(),
		"视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.wmv *.webm *.ts *.mp3 *.wav *.aac *.flac);;所有文件 (*.*)"
	);
	if (name.isEmpty()) return;
	setWindowTitle(name);
	if (!dt.Open(name.toUtf8().constData(), videoWidget))
	{
		QMessageBox::information(this, "错误", "打开文件失败!\n请检查文件格式是否受支持。");
		return;
	}
	if (hintLabel) hintLabel->hide();
	SetPause(dt.isPause);
}
