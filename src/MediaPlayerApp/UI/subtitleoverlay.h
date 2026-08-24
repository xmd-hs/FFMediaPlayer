#pragma once

#include "isubtitlecallback.h"
#include <QLabel>
#include <QVector>
#include <QMutex>
#include <QPixmap>

struct SubtitleCue
{
	QString text;
	QImage bitmap;
	long long startMs = 0;
	long long endMs = 0;
	bool isBitmap = false;
};

class SubtitleOverlay : public QLabel, public ISubtitleCallback
{
	Q_OBJECT
public:
	explicit SubtitleOverlay(QWidget *parent = nullptr);

	void OnSubtitleCue(const QString &text, long long startMs, long long endMs) override;
	void OnSubtitleBitmap(const QImage &img, long long startMs, long long endMs) override;
	void OnSubtitleClear() override;

	void UpdateClock(long long ptsMs);
	void ClearAll();
	bool LoadExternalSrt(const QString &path);

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void refreshDisplay(long long ptsMs);
	static bool parseSrt(const QString &content, QVector<SubtitleCue> *out);

	QMutex mux_;
	QVector<SubtitleCue> cues_;
	QString currentText_;
	bool showingBitmap_ = false;
	bool externalMode_ = false;
};
