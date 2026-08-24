#pragma once

#include <QtWidgets/QWidget>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include "videowidget.h"
#include "seekslider.h"
#include "playlistmodel.h"
#include "onlinecatalog.h"
#include "subtitleoverlay.h"

class MediaPlayer : public QWidget
{
	Q_OBJECT

public:
	MediaPlayer(QWidget *parent = Q_NULLPTR);
	~MediaPlayer();

	void timerEvent(QTimerEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

public slots:
	void OpenFile();
	void PlayOrPause();
	void SliderPress();
	void SliderRelease();
	void VolumeChanged(int val);
	void SpeedUp();
	void SpeedDown();
	void SpeedReset();
	void PlayListActivated(const QModelIndex &index);
	void PlayPrev();
	void PlayNext();
	void RemoveSelected();
	void ClearPlaylist();
	void RefreshOnline();
	void LoadOnlineFile();
	void OnOnlineLoaded(const QVector<PlaylistItem> &items);
	void OnOnlineFailed(const QString &reason);
	void SubtitleTrackChanged(int comboIndex);
	void AudioTrackChanged(int comboIndex);
	void LoadExternalSubtitle();
	void OpenNetworkUrl();
	void ApplyNetworkOptions();
	void LoopModeChanged(int index);
	void ShuffleToggled(bool on);
	void HwAccelToggled(bool on);
	void TakeScreenshot();

private:
	void SetPause(bool isPause);
	bool PlayAt(int index);
	bool PlayUrl(const QString &url, const QString &title, bool online);
	void refreshSubtitleCombo();
	void refreshAudioCombo();
	void layoutOverlay();
	void updateStatusLabel();
	void updateSeekEnabled();
	void saveSettings();
	void loadSettings();
	void applyOpenOptionsToEngine();
	static bool isNetworkMediaUrl(const QString &url);
	static QString trackLabel(const QString &prefix, int streamIndex,
		const std::string &lang, const std::string &title, const std::string &codec);

	bool isSliderPress = false;
	bool isSeeking = false;
	bool handledEof_ = false;
	bool suppressTrackSignals_ = false;
	double seekTargetPos = 0.0;
	long long seekStartTime = 0;

	VideoWidget *videoWidget = nullptr;
	SubtitleOverlay *subtitleOverlay = nullptr;
	SeekSlider *playPos = nullptr;
	QPushButton *openFileBtn = nullptr;
	QPushButton *isplayBtn = nullptr;
	QSlider *volumeSlider = nullptr;
	QPushButton *speedUpBtn = nullptr;
	QPushButton *speedDownBtn = nullptr;
	QPushButton *speedResetBtn = nullptr;
	QLabel *speedLabel = nullptr;
	QLabel *timeLabel = nullptr;
	QLabel *hintLabel = nullptr;
	QLabel *statusLabel = nullptr;
	QComboBox *subtitleCombo = nullptr;
	QComboBox *audioCombo = nullptr;
	QComboBox *loopCombo = nullptr;
	QCheckBox *shuffleCheck = nullptr;
	QCheckBox *hwAccelCheck = nullptr;

	QListView *playlistView = nullptr;
	PlaylistModel *playlistModel = nullptr;
	QLineEdit *mediaUrlEdit = nullptr;
	QLineEdit *onlineUrlEdit = nullptr;
	QLineEdit *userAgentEdit = nullptr;
	QPlainTextEdit *headersEdit = nullptr;
	OnlineCatalog *onlineCatalog = nullptr;
	QWidget *videoArea = nullptr;
};
