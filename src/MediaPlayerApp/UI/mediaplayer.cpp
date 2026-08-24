#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif
#include "mediaplayer.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QUrl>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QScrollArea>
#include "demuxthread.h"
#include "mediademuxer.h"

static DemuxThread dt;

static QString lineEditStyle()
{
	return "QLineEdit,QPlainTextEdit{background:#1a1a2e;color:#fff;border:1px solid rgba(255,255,255,40);"
		"border-radius:4px;padding:6px}";
}

MediaPlayer::MediaPlayer(QWidget *parent) : QWidget(parent)
{
	setObjectName("MediaPlayer");
	setStyleSheet("#MediaPlayer{background-color:#1a1a2e}");
	setFocusPolicy(Qt::StrongFocus);
	resize(1280, 720);
	setMinimumSize(800, 480);

	auto *root = new QHBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	auto *mainCol = new QVBoxLayout();
	mainCol->setContentsMargins(0, 0, 0, 0);
	mainCol->setSpacing(0);

	videoArea = new QWidget(this);
	auto *videoLayout = new QVBoxLayout(videoArea);
	videoLayout->setContentsMargins(0, 0, 0, 0);

	videoWidget = new VideoWidget(videoArea);
	videoWidget->setMinimumHeight(200);
	videoLayout->addWidget(videoWidget, 1);

	subtitleOverlay = new SubtitleOverlay(videoArea);
	subtitleOverlay->raise();

	hintLabel = new QLabel(QString::fromUtf8("请打开文件、输入网络地址或从右侧列表选择"), videoArea);
	hintLabel->setAlignment(Qt::AlignCenter);
	hintLabel->setStyleSheet(
		"color:rgba(255,255,255,180);font-size:22px;"
		"background-color:rgba(26,26,46,220);border-radius:8px;");
	hintLabel->raise();

	statusLabel = new QLabel(videoArea);
	statusLabel->setAlignment(Qt::AlignCenter);
	statusLabel->setStyleSheet(
		"color:#fff;font-size:16px;background-color:rgba(0,0,0,160);"
		"border-radius:6px;padding:8px 16px;");
	statusLabel->hide();

	mainCol->addWidget(videoArea, 1);

	auto *controlPanel = new QWidget(this);
	controlPanel->setObjectName("controlPanel");
	controlPanel->setStyleSheet(
		"#controlPanel{background-color:#16213e;border-top:1px solid rgba(255,255,255,30)}");
	controlPanel->setFixedHeight(110);

	auto *controlLayout = new QVBoxLayout(controlPanel);
	controlLayout->setContentsMargins(12, 4, 12, 8);
	controlLayout->setSpacing(4);

	playPos = new SeekSlider(this);
	playPos->setRange(0, 9999);
	playPos->setPageStep(10);
	playPos->setOrientation(Qt::Horizontal);
	playPos->setStyleSheet(
		"QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,60);border-radius:2px}"
		"QSlider::handle:horizontal{background:#1abc9c;width:14px;height:14px;margin:-5px 0;border-radius:7px}"
		"QSlider::sub-page:horizontal{background:#1abc9c;border-radius:2px}");
	connect(playPos, &SeekSlider::sliderPressed, this, &MediaPlayer::SliderPress);
	connect(playPos, &SeekSlider::sliderReleased, this, &MediaPlayer::SliderRelease);
	controlLayout->addWidget(playPos);

	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(6);

	auto btnStyle = QString(
		"QPushButton{background-color:rgba(255,255,255,30);color:#fff;"
		"border:1px solid rgba(255,255,255,60);border-radius:4px;padding:6px 10px;font-size:13px}"
		"QPushButton:hover{background-color:rgba(255,255,255,60)}"
		"QPushButton:pressed{background-color:rgba(255,255,255,90)}");

	openFileBtn = new QPushButton(QString::fromUtf8("打开文件"), this);
	openFileBtn->setStyleSheet(btnStyle);
	openFileBtn->setFixedHeight(32);
	connect(openFileBtn, &QPushButton::clicked, this, &MediaPlayer::OpenFile);
	btnLayout->addWidget(openFileBtn);

	isplayBtn = new QPushButton(QString::fromUtf8("播 放"), this);
	isplayBtn->setStyleSheet(btnStyle);
	isplayBtn->setFixedHeight(32);
	connect(isplayBtn, &QPushButton::clicked, this, &MediaPlayer::PlayOrPause);
	btnLayout->addWidget(isplayBtn);

	auto *prevBtn = new QPushButton(QString::fromUtf8("上一首"), this);
	prevBtn->setStyleSheet(btnStyle);
	prevBtn->setFixedHeight(32);
	connect(prevBtn, &QPushButton::clicked, this, &MediaPlayer::PlayPrev);
	btnLayout->addWidget(prevBtn);

	auto *nextBtn = new QPushButton(QString::fromUtf8("下一首"), this);
	nextBtn->setStyleSheet(btnStyle);
	nextBtn->setFixedHeight(32);
	connect(nextBtn, &QPushButton::clicked, this, &MediaPlayer::PlayNext);
	btnLayout->addWidget(nextBtn);

	auto *shotBtn = new QPushButton(QString::fromUtf8("截图"), this);
	shotBtn->setStyleSheet(btnStyle);
	shotBtn->setFixedHeight(32);
	connect(shotBtn, &QPushButton::clicked, this, &MediaPlayer::TakeScreenshot);
	btnLayout->addWidget(shotBtn);

	btnLayout->addSpacing(6);

	speedDownBtn = new QPushButton("◀◀", this);
	speedDownBtn->setStyleSheet(btnStyle);
	speedDownBtn->setFixedSize(36, 32);
	connect(speedDownBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedDown);
	btnLayout->addWidget(speedDownBtn);

	speedLabel = new QLabel("1.0x", this);
	speedLabel->setAlignment(Qt::AlignCenter);
	speedLabel->setFixedSize(46, 32);
	speedLabel->setStyleSheet(
		"color:#1abc9c;font-size:13px;font-weight:bold;"
		"background-color:rgba(26,188,156,30);border-radius:4px");
	btnLayout->addWidget(speedLabel);

	speedUpBtn = new QPushButton("▶▶", this);
	speedUpBtn->setStyleSheet(btnStyle);
	speedUpBtn->setFixedSize(36, 32);
	connect(speedUpBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedUp);
	btnLayout->addWidget(speedUpBtn);

	speedResetBtn = new QPushButton("1x", this);
	speedResetBtn->setStyleSheet(btnStyle);
	speedResetBtn->setFixedSize(34, 32);
	connect(speedResetBtn, &QPushButton::clicked, this, &MediaPlayer::SpeedReset);
	btnLayout->addWidget(speedResetBtn);

	btnLayout->addSpacing(6);

	auto *volIcon = new QLabel(QString::fromUtf8("🔊"), this);
	volIcon->setFixedSize(20, 32);
	volIcon->setAlignment(Qt::AlignCenter);
	volIcon->setStyleSheet("font-size:14px;background:transparent");
	btnLayout->addWidget(volIcon);

	volumeSlider = new QSlider(Qt::Horizontal, this);
	volumeSlider->setRange(0, 100);
	volumeSlider->setValue(80);
	volumeSlider->setFixedWidth(80);
	volumeSlider->setStyleSheet(
		"QSlider::groove:horizontal{height:4px;background:rgba(255,255,255,60);border-radius:2px}"
		"QSlider::handle:horizontal{background:#3498db;width:12px;height:12px;margin:-4px 0;border-radius:6px}"
		"QSlider::sub-page:horizontal{background:#3498db;border-radius:2px}");
	connect(volumeSlider, &QSlider::valueChanged, this, &MediaPlayer::VolumeChanged);
	btnLayout->addWidget(volumeSlider);

	auto comboStyle = QString(
		"QComboBox{background:rgba(255,255,255,20);color:#fff;border:1px solid rgba(255,255,255,50);"
		"border-radius:4px;padding:4px 8px}"
		"QComboBox QAbstractItemView{background:#16213e;color:#fff;selection-background-color:#1abc9c}");

	btnLayout->addWidget(new QLabel(QString::fromUtf8("音轨"), this));
	audioCombo = new QComboBox(this);
	audioCombo->setFixedWidth(110);
	audioCombo->setStyleSheet(comboStyle);
	connect(audioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &MediaPlayer::AudioTrackChanged);
	btnLayout->addWidget(audioCombo);

	btnLayout->addWidget(new QLabel(QString::fromUtf8("字幕"), this));
	subtitleCombo = new QComboBox(this);
	subtitleCombo->setFixedWidth(110);
	subtitleCombo->setStyleSheet(comboStyle);
	subtitleCombo->addItem(QString::fromUtf8("关"), -1);
	connect(subtitleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &MediaPlayer::SubtitleTrackChanged);
	btnLayout->addWidget(subtitleCombo);

	auto *extSubBtn = new QPushButton(QString::fromUtf8("外挂SRT"), this);
	extSubBtn->setStyleSheet(btnStyle);
	extSubBtn->setFixedHeight(32);
	connect(extSubBtn, &QPushButton::clicked, this, &MediaPlayer::LoadExternalSubtitle);
	btnLayout->addWidget(extSubBtn);

	btnLayout->addStretch();

	timeLabel = new QLabel("00:00 / 00:00", this);
	timeLabel->setAlignment(Qt::AlignCenter);
	timeLabel->setStyleSheet("color:rgba(255,255,255,180);font-size:12px;background:transparent");
	timeLabel->setFixedWidth(120);
	btnLayout->addWidget(timeLabel);

	controlLayout->addLayout(btnLayout);
	mainCol->addWidget(controlPanel);

	// Side panel
	auto *sideScroll = new QScrollArea(this);
	sideScroll->setFixedWidth(300);
	sideScroll->setWidgetResizable(true);
	sideScroll->setStyleSheet("QScrollArea{background:#0f3460;border:none}");
	auto *side = new QWidget;
	side->setStyleSheet("background-color:#0f3460");
	auto *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(8, 8, 8, 8);
	sideLayout->setSpacing(6);

	auto *sideTitle = new QLabel(QString::fromUtf8("播放列表"), side);
	sideTitle->setStyleSheet("color:#1abc9c;font-size:15px;font-weight:bold;background:transparent");
	sideLayout->addWidget(sideTitle);

	playlistModel = new PlaylistModel(this);
	playlistView = new QListView(side);
	playlistView->setModel(playlistModel);
	playlistView->setMinimumHeight(160);
	playlistView->setStyleSheet(
		"QListView{background:#1a1a2e;color:#eee;border:1px solid rgba(255,255,255,40);border-radius:4px}"
		"QListView::item{padding:6px}"
		"QListView::item:selected{background:#1abc9c;color:#102a2a}");
	connect(playlistView, &QListView::doubleClicked, this, &MediaPlayer::PlayListActivated);
	sideLayout->addWidget(playlistView, 1);

	auto *listBtns = new QHBoxLayout();
	auto *rmBtn = new QPushButton(QString::fromUtf8("移除"), side);
	auto *clrBtn = new QPushButton(QString::fromUtf8("清空"), side);
	rmBtn->setStyleSheet(btnStyle);
	clrBtn->setStyleSheet(btnStyle);
	connect(rmBtn, &QPushButton::clicked, this, &MediaPlayer::RemoveSelected);
	connect(clrBtn, &QPushButton::clicked, this, &MediaPlayer::ClearPlaylist);
	listBtns->addWidget(rmBtn);
	listBtns->addWidget(clrBtn);
	sideLayout->addLayout(listBtns);

	auto *modeRow = new QHBoxLayout();
	loopCombo = new QComboBox(side);
	loopCombo->setStyleSheet(comboStyle);
	loopCombo->addItem(QString::fromUtf8("不循环"), (int)PlaylistLoopMode::Off);
	loopCombo->addItem(QString::fromUtf8("列表循环"), (int)PlaylistLoopMode::List);
	loopCombo->addItem(QString::fromUtf8("单曲循环"), (int)PlaylistLoopMode::One);
	connect(loopCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &MediaPlayer::LoopModeChanged);
	modeRow->addWidget(loopCombo);
	shuffleCheck = new QCheckBox(QString::fromUtf8("随机"), side);
	shuffleCheck->setStyleSheet("color:#fff");
	connect(shuffleCheck, &QCheckBox::toggled, this, &MediaPlayer::ShuffleToggled);
	modeRow->addWidget(shuffleCheck);
	sideLayout->addLayout(modeRow);

	hwAccelCheck = new QCheckBox(QString::fromUtf8("尝试硬件解码"), side);
	hwAccelCheck->setStyleSheet("color:#fff");
	connect(hwAccelCheck, &QCheckBox::toggled, this, &MediaPlayer::HwAccelToggled);
	sideLayout->addWidget(hwAccelCheck);

	auto *mediaUrlTitle = new QLabel(QString::fromUtf8("打开网络地址"), side);
	mediaUrlTitle->setStyleSheet("color:#1abc9c;font-size:13px;font-weight:bold;background:transparent");
	sideLayout->addWidget(mediaUrlTitle);

	mediaUrlEdit = new QLineEdit(side);
	mediaUrlEdit->setPlaceholderText("https://.../video.mp4 或 rtsp://...");
	mediaUrlEdit->setStyleSheet(lineEditStyle());
	sideLayout->addWidget(mediaUrlEdit);

	auto *playUrlBtn = new QPushButton(QString::fromUtf8("播放网络地址"), side);
	playUrlBtn->setStyleSheet(btnStyle);
	connect(playUrlBtn, &QPushButton::clicked, this, &MediaPlayer::OpenNetworkUrl);
	connect(mediaUrlEdit, &QLineEdit::returnPressed, this, &MediaPlayer::OpenNetworkUrl);
	sideLayout->addWidget(playUrlBtn);

	auto *authTitle = new QLabel(QString::fromUtf8("网络请求头 / 鉴权"), side);
	authTitle->setStyleSheet("color:#f39c12;font-size:13px;font-weight:bold;background:transparent");
	sideLayout->addWidget(authTitle);

	userAgentEdit = new QLineEdit(side);
	userAgentEdit->setPlaceholderText("User-Agent (可选)");
	userAgentEdit->setText("FFMediaPlayer/1.0");
	userAgentEdit->setStyleSheet(lineEditStyle());
	sideLayout->addWidget(userAgentEdit);

	headersEdit = new QPlainTextEdit(side);
	headersEdit->setPlaceholderText(QString::fromUtf8("自定义 Header，每行一条，例如:\nAuthorization: Bearer xxx\nCookie: a=1"));
	headersEdit->setFixedHeight(70);
	headersEdit->setStyleSheet(lineEditStyle());
	sideLayout->addWidget(headersEdit);

	auto *applyHdrBtn = new QPushButton(QString::fromUtf8("应用网络选项"), side);
	applyHdrBtn->setStyleSheet(btnStyle);
	connect(applyHdrBtn, &QPushButton::clicked, this, &MediaPlayer::ApplyNetworkOptions);
	sideLayout->addWidget(applyHdrBtn);

	auto *onlineTitle = new QLabel(QString::fromUtf8("在线列表 JSON"), side);
	onlineTitle->setStyleSheet("color:#3498db;font-size:13px;font-weight:bold;background:transparent");
	sideLayout->addWidget(onlineTitle);

	onlineUrlEdit = new QLineEdit(side);
	onlineUrlEdit->setPlaceholderText("https://example.com/catalog.json");
	onlineUrlEdit->setStyleSheet(lineEditStyle());
	sideLayout->addWidget(onlineUrlEdit);

	auto *onlineBtns = new QHBoxLayout();
	auto *fetchBtn = new QPushButton(QString::fromUtf8("拉取"), side);
	auto *fileBtn = new QPushButton(QString::fromUtf8("本地JSON"), side);
	fetchBtn->setStyleSheet(btnStyle);
	fileBtn->setStyleSheet(btnStyle);
	connect(fetchBtn, &QPushButton::clicked, this, &MediaPlayer::RefreshOnline);
	connect(fileBtn, &QPushButton::clicked, this, &MediaPlayer::LoadOnlineFile);
	onlineBtns->addWidget(fetchBtn);
	onlineBtns->addWidget(fileBtn);
	sideLayout->addLayout(onlineBtns);

	auto *hintKeys = new QLabel(QString::fromUtf8(
		"快捷键: 空格播放/暂停  ←→Seek  ↑↓音量  F全屏  S截图"), side);
	hintKeys->setWordWrap(true);
	hintKeys->setStyleSheet("color:rgba(255,255,255,140);font-size:11px;background:transparent");
	sideLayout->addWidget(hintKeys);

	sideScroll->setWidget(side);
	root->addLayout(mainCol, 1);
	root->addWidget(sideScroll);

	onlineCatalog = new OnlineCatalog(this);
	connect(onlineCatalog, &OnlineCatalog::loaded, this, &MediaPlayer::OnOnlineLoaded);
	connect(onlineCatalog, &OnlineCatalog::failed, this, &MediaPlayer::OnOnlineFailed);

	dt.Start();
	dt.SetVolume(80);
	dt.SetSubtitleCallback(subtitleOverlay);
	loadSettings();
	applyOpenOptionsToEngine();
	startTimer(40);
}

MediaPlayer::~MediaPlayer()
{
	saveSettings();
	dt.Close();
}

void MediaPlayer::closeEvent(QCloseEvent *e)
{
	saveSettings();
	QWidget::closeEvent(e);
}

void MediaPlayer::applyOpenOptionsToEngine()
{
	DemuxOpenOptions opts;
	opts.userAgent = userAgentEdit ? userAgentEdit->text().trimmed().toStdString() : "FFMediaPlayer/1.0";
	if (opts.userAgent.empty()) opts.userAgent = "FFMediaPlayer/1.0";
	opts.tryHwAccel = hwAccelCheck && hwAccelCheck->isChecked();

	QString headers;
	if (headersEdit)
	{
		const QStringList lines = headersEdit->toPlainText().split('\n', QString::SkipEmptyParts);
		for (QString line : lines)
		{
			line = line.trimmed();
			if (line.isEmpty()) continue;
			if (!line.endsWith("\r"))
				headers += line + "\r\n";
			else
				headers += line + "\n";
		}
	}
	opts.headers = headers.toStdString();
	dt.SetOpenOptions(opts);
}

void MediaPlayer::ApplyNetworkOptions()
{
	applyOpenOptionsToEngine();
	QMessageBox::information(this, QString::fromUtf8("网络选项"),
		QString::fromUtf8("已应用。下次打开网络地址时生效。"));
}

void MediaPlayer::HwAccelToggled(bool)
{
	applyOpenOptionsToEngine();
}

void MediaPlayer::layoutOverlay()
{
	if (!videoArea || !videoWidget) return;
	QRect r = videoWidget->geometry();
	if (hintLabel && hintLabel->isVisible())
		hintLabel->setGeometry(r.adjusted(40, 40, -40, -40));
	if (statusLabel && statusLabel->isVisible())
		statusLabel->setGeometry(r.center().x() - 100, r.center().y() - 20, 200, 40);
	if (subtitleOverlay)
	{
		int h = 100;
		subtitleOverlay->setGeometry(r.left() + 20, r.bottom() - h - 12, r.width() - 40, h);
		subtitleOverlay->raise();
	}
}

void MediaPlayer::updateStatusLabel()
{
	if (!statusLabel) return;
	if (dt.isOpening)
	{
		statusLabel->setText(QString::fromUtf8("正在打开…"));
		statusLabel->show();
		statusLabel->raise();
	}
	else if (dt.isBuffering && !dt.isPause && !dt.isEof)
	{
		statusLabel->setText(QString::fromUtf8("缓冲中…"));
		statusLabel->show();
		statusLabel->raise();
	}
	else
	{
		statusLabel->hide();
	}
}

void MediaPlayer::updateSeekEnabled()
{
	bool playing = hintLabel && !hintLabel->isVisible();
	bool enable = !(playing && dt.totalMs <= 0);
	playPos->setEnabled(enable);
	if (!enable)
		playPos->setToolTip(QString::fromUtf8("直播/未知时长，不支持拖动"));
	else
		playPos->setToolTip(QString());
}

void MediaPlayer::SliderPress()
{
	if (!playPos->isEnabled()) return;
	isSliderPress = true;
}

void MediaPlayer::SliderRelease()
{
	if (!playPos->isEnabled()) { isSliderPress = false; return; }
	isSliderPress = false;
	isSeeking = true;
	double pos = (double)playPos->value() / (double)playPos->maximum();
	seekTargetPos = pos;
	seekStartTime = QDateTime::currentMSecsSinceEpoch();
	playPos->setValue((int)(playPos->maximum() * pos));
	if (subtitleOverlay) subtitleOverlay->OnSubtitleClear();
	dt.Seek(pos);
}

void MediaPlayer::timerEvent(QTimerEvent *)
{
	layoutOverlay();
	updateStatusLabel();
	updateSeekEnabled();

	long long curPts = dt.GetCurrentPts();
	if (subtitleOverlay) subtitleOverlay->UpdateClock(curPts);

	if (dt.isEof)
	{
		isSeeking = false;
		if (!handledEof_)
		{
			handledEof_ = true;
			int next = playlistModel ? playlistModel->nextIndex() : -1;
			if (next >= 0)
			{
				PlayAt(next);
				return;
			}
			if (isplayBtn) isplayBtn->setText(QString::fromUtf8("播 放"));
		}
		return;
	}
	handledEof_ = false;

	if (isSliderPress) return;
	long long total = dt.totalMs;
	if (total > 0)
	{
		if (curPts > total) curPts = total;
		if (curPts < 0) curPts = 0;
		double pos = (double)curPts / (double)total;
		if (pos > 1.0) pos = 1.0;

		if (isSeeking)
		{
			playPos->setValue((int)(playPos->maximum() * seekTargetPos));
			long long now = QDateTime::currentMSecsSinceEpoch();
			if (now - seekStartTime > 500)
				isSeeking = false;
			return;
		}

		playPos->setValue((int)(playPos->maximum() * pos));
		int curSec = (int)((curPts + 500) / 1000);
		int totSec = (int)((total + 500) / 1000);
		timeLabel->setText(QString("%1:%2 / %3:%4")
			.arg(curSec / 60, 2, 10, QChar('0'))
			.arg(curSec % 60, 2, 10, QChar('0'))
			.arg(totSec / 60, 2, 10, QChar('0'))
			.arg(totSec % 60, 2, 10, QChar('0')));
	}
	else
	{
		int curSec = (int)((curPts + 500) / 1000);
		timeLabel->setText(QString("%1:%2 / LIVE")
			.arg(curSec / 60, 2, 10, QChar('0'))
			.arg(curSec % 60, 2, 10, QChar('0')));
	}
}

void MediaPlayer::mouseDoubleClickEvent(QMouseEvent *)
{
	isFullScreen() ? showNormal() : showFullScreen();
}

void MediaPlayer::resizeEvent(QResizeEvent *)
{
	layoutOverlay();
}

void MediaPlayer::keyPressEvent(QKeyEvent *e)
{
	switch (e->key())
	{
	case Qt::Key_Space:
		PlayOrPause();
		break;
	case Qt::Key_Left:
		if (dt.totalMs > 0)
		{
			double p = (double)dt.GetCurrentPts() / (double)dt.totalMs - 0.05;
			if (p < 0) p = 0;
			dt.Seek(p);
		}
		break;
	case Qt::Key_Right:
		if (dt.totalMs > 0)
		{
			double p = (double)dt.GetCurrentPts() / (double)dt.totalMs + 0.05;
			if (p > 1) p = 1;
			dt.Seek(p);
		}
		break;
	case Qt::Key_Up:
		volumeSlider->setValue(qMin(100, volumeSlider->value() + 5));
		break;
	case Qt::Key_Down:
		volumeSlider->setValue(qMax(0, volumeSlider->value() - 5));
		break;
	case Qt::Key_F:
		isFullScreen() ? showNormal() : showFullScreen();
		break;
	case Qt::Key_S:
		TakeScreenshot();
		break;
	default:
		QWidget::keyPressEvent(e);
		break;
	}
}

void MediaPlayer::PlayOrPause()
{
	if (dt.isEof)
	{
		int next = playlistModel ? playlistModel->nextIndex() : -1;
		if (next >= 0) { PlayAt(next); return; }
		if (dt.totalMs > 0)
		{
			dt.Seek(0.0);
			dt.SetPause(false);
			SetPause(false);
			handledEof_ = false;
		}
		return;
	}
	bool pause = !dt.isPause;
	SetPause(pause);
	dt.SetPause(pause);
}

void MediaPlayer::SetPause(bool pause)
{
	isplayBtn->setText(pause ? QString::fromUtf8("播 放") : QString::fromUtf8("暂 停"));
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

bool MediaPlayer::isNetworkMediaUrl(const QString &url)
{
	QString u = url.trimmed().toLower();
	return u.startsWith("http://") || u.startsWith("https://")
		|| u.startsWith("rtsp://") || u.startsWith("rtsps://")
		|| u.startsWith("rtmp://") || u.startsWith("udp://")
		|| u.startsWith("tcp://") || u.startsWith("rtp://")
		|| u.startsWith("mms://");
}

QString MediaPlayer::trackLabel(const QString &prefix, int streamIndex,
	const std::string &lang, const std::string &title, const std::string &codec)
{
	QString label = prefix + QString::number(streamIndex);
	if (!lang.empty()) label += " " + QString::fromStdString(lang);
	if (!title.empty()) label += " " + QString::fromStdString(title);
	else if (!codec.empty()) label += " (" + QString::fromStdString(codec) + ")";
	return label;
}

bool MediaPlayer::PlayUrl(const QString &url, const QString &title, bool online)
{
	QString trimmed = url.trimmed();
	if (trimmed.isEmpty()) return false;

	PlaylistItem it;
	it.url = trimmed;
	it.online = online || isNetworkMediaUrl(trimmed);
	it.title = title.trimmed();
	if (it.title.isEmpty())
	{
		QUrl qurl(trimmed);
		it.title = qurl.fileName();
		if (it.title.isEmpty()) it.title = trimmed;
	}

	playlistModel->appendItem(it);
	return PlayAt(playlistModel->count() - 1);
}

bool MediaPlayer::PlayAt(int index)
{
	if (!playlistModel || index < 0 || index >= playlistModel->count())
		return false;
	PlaylistItem it = playlistModel->itemAt(index);
	if (it.url.isEmpty()) return false;

	applyOpenOptionsToEngine();
	playlistModel->setCurrentIndex(index);
	playlistView->setCurrentIndex(playlistModel->index(index));
	setWindowTitle(it.title);

	if (subtitleOverlay) subtitleOverlay->ClearAll();
	handledEof_ = false;
	if (statusLabel)
	{
		statusLabel->setText(QString::fromUtf8("正在打开…"));
		statusLabel->show();
		statusLabel->raise();
	}

	QByteArray urlBytes = it.url.toUtf8();
	if (!dt.Open(urlBytes.constData(), videoWidget, subtitleOverlay))
	{
		QString err = QString::fromStdString(dt.LastError());
		if (err.isEmpty()) err = QString::fromUtf8("未知错误");
		QMessageBox::information(this, QString::fromUtf8("打开失败"),
			QString::fromUtf8("%1\n\n%2").arg(it.url, err));
		if (statusLabel) statusLabel->hide();
		return false;
	}
	if (hintLabel) hintLabel->hide();
	SetPause(dt.isPause);
	refreshSubtitleCombo();
	refreshAudioCombo();
	updateSeekEnabled();
	return true;
}

void MediaPlayer::OpenFile()
{
	QStringList names = QFileDialog::getOpenFileNames(
		this, QString::fromUtf8("选择视频文件"), QString(),
		QString::fromUtf8("媒体文件 (*.mp4 *.avi *.mkv *.mov *.flv *.wmv *.webm *.ts *.mp3 *.wav *.aac *.flac);;所有文件 (*.*)"));
	if (names.isEmpty()) return;

	int first = -1;
	for (const QString &name : names)
	{
		PlaylistItem it;
		it.url = name;
		it.title = QFileInfo(name).fileName();
		it.online = false;
		playlistModel->appendItem(it);
		if (first < 0) first = playlistModel->count() - 1;
	}
	PlayAt(first);
}

void MediaPlayer::OpenNetworkUrl()
{
	QString url = mediaUrlEdit ? mediaUrlEdit->text().trimmed() : QString();
	if (url.isEmpty())
	{
		QMessageBox::information(this, QString::fromUtf8("提示"),
			QString::fromUtf8("请输入网络媒体地址"));
		return;
	}
	if (!isNetworkMediaUrl(url))
	{
		QMessageBox::warning(this, QString::fromUtf8("地址无效"),
			QString::fromUtf8("仅支持 http/https/rtsp/rtmp/udp/rtp 等网络协议"));
		return;
	}
	applyOpenOptionsToEngine();
	PlayUrl(url, QString(), true);
}

void MediaPlayer::PlayListActivated(const QModelIndex &index)
{
	if (index.isValid()) PlayAt(index.row());
}

void MediaPlayer::PlayPrev()
{
	int prev = playlistModel ? playlistModel->prevIndex() : -1;
	if (prev >= 0) PlayAt(prev);
}

void MediaPlayer::PlayNext()
{
	int next = playlistModel ? playlistModel->nextIndex() : -1;
	if (next >= 0) PlayAt(next);
}

void MediaPlayer::RemoveSelected()
{
	QModelIndex idx = playlistView->currentIndex();
	if (idx.isValid()) playlistModel->removeAt(idx.row());
}

void MediaPlayer::ClearPlaylist()
{
	playlistModel->clear();
}

void MediaPlayer::LoopModeChanged(int index)
{
	if (!loopCombo || !playlistModel) return;
	playlistModel->setLoopMode((PlaylistLoopMode)loopCombo->itemData(index).toInt());
}

void MediaPlayer::ShuffleToggled(bool on)
{
	if (playlistModel) playlistModel->setShuffle(on);
}

void MediaPlayer::RefreshOnline()
{
	QString u = onlineUrlEdit->text().trimmed();
	if (u.isEmpty())
	{
		QMessageBox::information(this, QString::fromUtf8("提示"),
			QString::fromUtf8("请先填写在线 JSON 地址"));
		return;
	}
	onlineCatalog->fetch(QUrl(u));
}

void MediaPlayer::LoadOnlineFile()
{
	QString path = QFileDialog::getOpenFileName(
		this, QString::fromUtf8("选择在线目录 JSON"), QString(), "JSON (*.json);;All (*.*)");
	if (path.isEmpty()) return;
	QVector<PlaylistItem> items;
	QString err;
	if (!OnlineCatalog::loadFromFile(path, &items, &err))
	{
		QMessageBox::warning(this, QString::fromUtf8("错误"), err);
		return;
	}
	OnOnlineLoaded(items);
}

void MediaPlayer::OnOnlineLoaded(const QVector<PlaylistItem> &items)
{
	for (const PlaylistItem &it : items)
		playlistModel->appendItem(it);
	QMessageBox::information(this, QString::fromUtf8("在线列表"),
		QString::fromUtf8("已加入 %1 项").arg(items.size()));
}

void MediaPlayer::OnOnlineFailed(const QString &reason)
{
	QMessageBox::warning(this, QString::fromUtf8("在线列表失败"), reason);
}

void MediaPlayer::refreshSubtitleCombo()
{
	suppressTrackSignals_ = true;
	subtitleCombo->clear();
	subtitleCombo->addItem(QString::fromUtf8("关"), -1);
	auto tracks = dt.GetSubtitleTracks();
	for (const auto &t : tracks)
	{
		subtitleCombo->addItem(
			trackLabel(QString::fromUtf8("轨"), t.streamIndex, t.language, t.title, t.codec),
			t.streamIndex);
	}
	subtitleCombo->setCurrentIndex(0);
	suppressTrackSignals_ = false;
}

void MediaPlayer::refreshAudioCombo()
{
	suppressTrackSignals_ = true;
	audioCombo->clear();
	auto tracks = dt.GetAudioTracks();
	int active = -1;
	// Prefer currently selected by demux — first track if unknown.
	for (int i = 0; i < tracks.size(); ++i)
	{
		const auto &t = tracks[i];
		audioCombo->addItem(
			trackLabel(QString::fromUtf8("A"), t.streamIndex, t.language, t.title, t.codec),
			t.streamIndex);
		if (i == 0) active = 0;
	}
	if (audioCombo->count() == 0)
		audioCombo->addItem(QString::fromUtf8("无"), -1);
	audioCombo->setCurrentIndex(qMax(0, active));
	suppressTrackSignals_ = false;
}

void MediaPlayer::SubtitleTrackChanged(int comboIndex)
{
	if (suppressTrackSignals_ || !subtitleCombo || comboIndex < 0) return;
	int streamIndex = subtitleCombo->itemData(comboIndex).toInt();
	if (subtitleOverlay) subtitleOverlay->ClearAll();
	if (!dt.SetSubtitleTrack(streamIndex) && streamIndex >= 0)
	{
		QMessageBox::information(this, QString::fromUtf8("字幕"),
			QString::fromUtf8("切换失败:\n%1").arg(QString::fromStdString(dt.LastError())));
		suppressTrackSignals_ = true;
		subtitleCombo->setCurrentIndex(0);
		suppressTrackSignals_ = false;
	}
}

void MediaPlayer::AudioTrackChanged(int comboIndex)
{
	if (suppressTrackSignals_ || !audioCombo || comboIndex < 0) return;
	int streamIndex = audioCombo->itemData(comboIndex).toInt();
	if (streamIndex < 0) return;
	if (!dt.SetAudioTrack(streamIndex))
	{
		QMessageBox::information(this, QString::fromUtf8("音轨"),
			QString::fromUtf8("切换失败:\n%1").arg(QString::fromStdString(dt.LastError())));
	}
}

void MediaPlayer::LoadExternalSubtitle()
{
	QString path = QFileDialog::getOpenFileName(
		this, QString::fromUtf8("选择外挂字幕"), QString(), "SubRip (*.srt);;All (*.*)");
	if (path.isEmpty()) return;
	dt.SetSubtitleTrack(-1);
	suppressTrackSignals_ = true;
	subtitleCombo->setCurrentIndex(0);
	suppressTrackSignals_ = false;
	if (!subtitleOverlay->LoadExternalSrt(path))
	{
		QMessageBox::warning(this, QString::fromUtf8("字幕"), QString::fromUtf8("SRT 解析失败"));
		return;
	}
	QMessageBox::information(this, QString::fromUtf8("字幕"), QString::fromUtf8("已加载外挂字幕"));
}

void MediaPlayer::TakeScreenshot()
{
	if (!videoWidget) return;
	QImage img = videoWidget->grabFramebuffer();
	if (img.isNull())
	{
		QMessageBox::warning(this, QString::fromUtf8("截图"), QString::fromUtf8("当前无画面可截"));
		return;
	}
	QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (dir.isEmpty()) dir = QDir::homePath();
	QString path = dir + "/FFMediaPlayer_" +
		QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png";
	if (!img.save(path))
	{
		QMessageBox::warning(this, QString::fromUtf8("截图"), QString::fromUtf8("保存失败"));
		return;
	}
	QMessageBox::information(this, QString::fromUtf8("截图"),
		QString::fromUtf8("已保存:\n%1").arg(path));
}

void MediaPlayer::saveSettings()
{
	QSettings s("FFMediaPlayer", "FFMediaPlayer");
	if (playlistModel) playlistModel->saveToSettings(s);
	s.setValue("volume", volumeSlider ? volumeSlider->value() : 80);
	s.setValue("userAgent", userAgentEdit ? userAgentEdit->text() : QString());
	s.setValue("headers", headersEdit ? headersEdit->toPlainText() : QString());
	s.setValue("hwAccel", hwAccelCheck && hwAccelCheck->isChecked());
	s.setValue("onlineCatalogUrl", onlineUrlEdit ? onlineUrlEdit->text() : QString());
}

void MediaPlayer::loadSettings()
{
	QSettings s("FFMediaPlayer", "FFMediaPlayer");
	if (playlistModel) playlistModel->loadFromSettings(s);
	int vol = s.value("volume", 80).toInt();
	if (volumeSlider) volumeSlider->setValue(vol);
	dt.SetVolume(vol);
	if (userAgentEdit) userAgentEdit->setText(s.value("userAgent", "FFMediaPlayer/1.0").toString());
	if (headersEdit) headersEdit->setPlainText(s.value("headers").toString());
	if (hwAccelCheck) hwAccelCheck->setChecked(s.value("hwAccel", false).toBool());
	if (onlineUrlEdit) onlineUrlEdit->setText(s.value("onlineCatalogUrl").toString());
	if (loopCombo && playlistModel)
	{
		int mode = (int)playlistModel->loopMode();
		for (int i = 0; i < loopCombo->count(); ++i)
		{
			if (loopCombo->itemData(i).toInt() == mode)
			{
				loopCombo->setCurrentIndex(i);
				break;
			}
		}
	}
	if (shuffleCheck && playlistModel)
		shuffleCheck->setChecked(playlistModel->shuffle());
}
