#pragma once
#include <QObject>
#include <QMouseEvent>
#include <QSlider>

class SeekSlider : public QSlider
{
	Q_OBJECT

public:
	SeekSlider(QWidget *parent = NULL);
	~SeekSlider();
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
};
