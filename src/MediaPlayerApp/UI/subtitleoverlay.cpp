#include "subtitleoverlay.h"
#include <QFile>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextStream>

SubtitleOverlay::SubtitleOverlay(QWidget *parent)
	: QLabel(parent)
{
	setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
	setWordWrap(true);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setStyleSheet(
		"QLabel{"
		"color:#fff;"
		"font-size:20px;"
		"font-weight:bold;"
		"background:transparent;"
		"padding:8px 16px;"
		"}"
	);
	hide();
}

void SubtitleOverlay::OnSubtitleCue(const QString &text, long long startMs, long long endMs)
{
	QMutexLocker lk(&mux_);
	if (externalMode_) return;
	SubtitleCue c;
	c.text = text.trimmed();
	c.startMs = startMs;
	c.endMs = endMs > startMs ? endMs : (startMs + 3000);
	c.isBitmap = false;
	if (c.text.isEmpty()) return;
	cues_.push_back(c);
	if (cues_.size() > 200)
		cues_.remove(0, cues_.size() - 200);
}

void SubtitleOverlay::OnSubtitleBitmap(const QImage &img, long long startMs, long long endMs)
{
	QMutexLocker lk(&mux_);
	if (externalMode_ || img.isNull()) return;
	SubtitleCue c;
	c.bitmap = img;
	c.startMs = startMs;
	c.endMs = endMs > startMs ? endMs : (startMs + 3000);
	c.isBitmap = true;
	cues_.push_back(c);
	if (cues_.size() > 64)
		cues_.remove(0, cues_.size() - 64);
}

void SubtitleOverlay::OnSubtitleClear()
{
	QMutexLocker lk(&mux_);
	if (!externalMode_)
		cues_.clear();
	currentText_.clear();
	showingBitmap_ = false;
	QMetaObject::invokeMethod(this, [this]() {
		clear();
		setPixmap(QPixmap());
		hide();
	}, Qt::QueuedConnection);
}

void SubtitleOverlay::UpdateClock(long long ptsMs)
{
	refreshDisplay(ptsMs);
}

void SubtitleOverlay::ClearAll()
{
	QMutexLocker lk(&mux_);
	cues_.clear();
	currentText_.clear();
	showingBitmap_ = false;
	externalMode_ = false;
	QMetaObject::invokeMethod(this, [this]() {
		clear();
		setPixmap(QPixmap());
		hide();
	}, Qt::QueuedConnection);
}

bool SubtitleOverlay::LoadExternalSrt(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;
	QTextStream ts(&f);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	ts.setCodec("UTF-8");
#endif
	QString content = ts.readAll();
	QVector<SubtitleCue> parsed;
	if (!parseSrt(content, &parsed))
		return false;

	QMutexLocker lk(&mux_);
	cues_ = parsed;
	externalMode_ = true;
	currentText_.clear();
	showingBitmap_ = false;
	return true;
}

void SubtitleOverlay::resizeEvent(QResizeEvent *e)
{
	QLabel::resizeEvent(e);
}

void SubtitleOverlay::refreshDisplay(long long ptsMs)
{
	SubtitleCue active;
	bool found = false;
	{
		QMutexLocker lk(&mux_);
		for (int i = cues_.size() - 1; i >= 0; --i)
		{
			const SubtitleCue &c = cues_.at(i);
			if (ptsMs >= c.startMs && ptsMs <= c.endMs)
			{
				active = c;
				found = true;
				break;
			}
		}
		if (!found)
		{
			if (currentText_.isEmpty() && !showingBitmap_) return;
			currentText_.clear();
			showingBitmap_ = false;
		}
		else if (!active.isBitmap && active.text == currentText_ && !showingBitmap_)
		{
			return;
		}
		else if (active.isBitmap)
		{
			showingBitmap_ = true;
			currentText_.clear();
		}
		else
		{
			currentText_ = active.text;
			showingBitmap_ = false;
		}
	}

	QMetaObject::invokeMethod(this, [this, found, active]() {
		if (!found)
		{
			clear();
			setPixmap(QPixmap());
			hide();
			return;
		}
		if (active.isBitmap)
		{
			QPixmap pm = QPixmap::fromImage(active.bitmap);
			if (width() > 0 && pm.width() > width())
				pm = pm.scaledToWidth(width() - 20, Qt::SmoothTransformation);
			setPixmap(pm);
			setText(QString());
			show();
			raise();
		}
		else
		{
			setPixmap(QPixmap());
			setText(QString("<div style='background:rgba(0,0,0,140);padding:4px 10px;border-radius:4px;'>%1</div>")
				.arg(active.text.toHtmlEscaped().replace('\n', "<br/>")));
			show();
			raise();
		}
	}, Qt::QueuedConnection);
}

bool SubtitleOverlay::parseSrt(const QString &content, QVector<SubtitleCue> *out)
{
	if (!out) return false;
	out->clear();
	QRegularExpression timeRe(
		R"((\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3}))");
	QStringList blocks = content.split(QRegularExpression(R"(\r?\n\r?\n)"), QString::SkipEmptyParts);
	auto toMs = [](int h, int m, int s, int ms) -> long long {
		if (ms < 10) ms *= 100;
		else if (ms < 100) ms *= 10;
		return ((long long)h * 3600 + m * 60 + s) * 1000 + ms;
	};

	for (const QString &block : blocks)
	{
		QStringList lines = block.split(QRegularExpression(R"(\r?\n)"), QString::SkipEmptyParts);
		if (lines.size() < 2) continue;
		int timeLine = 0;
		if (lines[0].trimmed().contains(QRegularExpression(R"(^\d+$)")))
			timeLine = 1;
		if (timeLine >= lines.size()) continue;
		auto m = timeRe.match(lines[timeLine]);
		if (!m.hasMatch()) continue;
		SubtitleCue c;
		c.startMs = toMs(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt(), m.captured(4).toInt());
		c.endMs = toMs(m.captured(5).toInt(), m.captured(6).toInt(), m.captured(7).toInt(), m.captured(8).toInt());
		QStringList textLines;
		for (int i = timeLine + 1; i < lines.size(); ++i)
			textLines << lines[i].trimmed();
		c.text = textLines.join('\n');
		c.isBitmap = false;
		if (!c.text.isEmpty())
			out->push_back(c);
	}
	return !out->isEmpty();
}
