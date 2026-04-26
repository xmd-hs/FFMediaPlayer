#include "seekslider.h"

void SeekSlider::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton)
	{
		emit sliderPressed();
		int w = width();
		if (w > 0)
		{
			double pos = (double)e->pos().x() / (double)w;
			if (pos < 0.0) pos = 0.0;
			if (pos > 1.0) pos = 1.0;
			setValue((int)(pos * maximum()));
		}
		e->accept();
		return;
	}
	QSlider::mousePressEvent(e);
}

void SeekSlider::mouseMoveEvent(QMouseEvent *e)
{
	if (e->buttons() & Qt::LeftButton)
	{
		int w = width();
		if (w > 0)
		{
			double pos = (double)e->pos().x() / (double)w;
			if (pos < 0.0) pos = 0.0;
			if (pos > 1.0) pos = 1.0;
			setValue((int)(pos * maximum()));
		}
		e->accept();
		return;
	}
	QSlider::mouseMoveEvent(e);
}

void SeekSlider::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton)
	{
		emit sliderReleased();
		e->accept();
		return;
	}
	QSlider::mouseReleaseEvent(e);
}

SeekSlider::SeekSlider(QWidget *parent) : QSlider(parent) {}
SeekSlider::~SeekSlider() {}
