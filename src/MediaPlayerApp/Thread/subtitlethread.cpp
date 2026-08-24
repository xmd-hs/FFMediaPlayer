#include "subtitlethread.h"
#include "mediadecoder.h"
#include <QString>
#include <QImage>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace std;

static QString stripAss(const QString &in)
{
	QString out;
	out.reserve(in.size());
	bool inTag = false;
	for (QChar c : in)
	{
		if (c == '{') { inTag = true; continue; }
		if (c == '}') { inTag = false; continue; }
		if (!inTag) out.append(c);
	}
	int pos = out.lastIndexOf(",,");
	if (pos >= 0 && pos + 2 < out.size())
		out = out.mid(pos + 2);
	return out.replace("\\N", "\n").replace("\\n", "\n").trimmed();
}

SubtitleThread::SubtitleThread() {}
SubtitleThread::~SubtitleThread() { Close(); }

void SubtitleThread::SetCallback(ISubtitleCallback *cb)
{
	std::lock_guard<std::mutex> lk(smux_);
	callback_ = cb;
}

void SubtitleThread::SetPause(bool p)
{
	isPause = p;
}

void SubtitleThread::Clear()
{
	DecodeThread::Clear();
	std::lock_guard<std::mutex> lk(smux_);
	if (callback_) callback_->OnSubtitleClear();
}

void SubtitleThread::Close()
{
	DecodeThread::Close();
	std::lock_guard<std::mutex> lk(smux_);
	if (callback_) callback_->OnSubtitleClear();
}

bool SubtitleThread::Open(AVCodecParameters *para)
{
	if (!para) return false;
	Clear();
	bool ok = decode->Open(para);
	cout << "[Subtitle] open " << (ok ? "OK" : "FAIL") << endl;
	return ok;
}

void SubtitleThread::emitCue(const QString &text, long long startMs, long long endMs)
{
	std::lock_guard<std::mutex> lk(smux_);
	if (callback_ && !text.isEmpty())
		callback_->OnSubtitleCue(text, startMs, endMs);
}

void SubtitleThread::run()
{
	while (!isExit)
	{
		if (isPause) { msleep(5); continue; }

		AVPacket *pkt = Pop();
		if (!pkt) { msleep(1); continue; }

		long long startMs = pkt->pts > 0 ? pkt->pts : 0;
		long long endMs = startMs + 3000;
		SubtitleDecodeResult raw;
		{
			std::lock_guard<std::mutex> lk(mux);
			if (!decode)
			{
				RecyclePacket(pkt);
				continue;
			}
			raw = decode->DecodeSubtitle(pkt, &startMs, &endMs);
		}

		if (!raw.text.empty())
			emitCue(stripAss(QString::fromUtf8(raw.text.c_str())), startMs, endMs);

		if (raw.bmpW > 0 && raw.bmpH > 0 && !raw.rgba.empty())
		{
			QImage img(raw.rgba.data(), raw.bmpW, raw.bmpH, raw.bmpW * 4, QImage::Format_RGBA8888);
			QImage copy = img.copy();
			std::lock_guard<std::mutex> lk(smux_);
			if (callback_) callback_->OnSubtitleBitmap(copy, startMs, endMs);
		}

		msleep(1);
	}
}
