#pragma once

#include <QtWidgets/QWidget>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include "videowidget.h"
#include "seekslider.h"

class MediaPlayer : public QWidget
{
	Q_OBJECT

public:
	MediaPlayer(QWidget *parent = Q_NULLPTR);
	~MediaPlayer();

	void timerEvent(QTimerEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

public slots:
	void OpenFile();
	void PlayOrPause();
	void SliderPress();
	void SliderRelease();
	void VolumeChanged(int val);
	void SpeedUp();
	void SpeedDown();
	void SpeedReset();

private:
	void SetPause(bool isPause);

	bool isSliderPress = false;
	VideoWidget *videoWidget = nullptr;
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
};
