#pragma once

#include <QString>
#include <QImage>

class ISubtitleCallback
{
public:
	virtual ~ISubtitleCallback() {}
	virtual void OnSubtitleCue(const QString &text, long long startMs, long long endMs) = 0;
	virtual void OnSubtitleBitmap(const QImage &img, long long startMs, long long endMs) = 0;
	virtual void OnSubtitleClear() = 0;
};
