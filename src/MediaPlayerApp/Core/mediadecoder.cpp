#include "mediadecoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}
#include <iostream>
#include <thread>
using namespace std;

static enum AVPixelFormat s_hwPixFmt = AV_PIX_FMT_NONE;

static enum AVPixelFormat getHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	(void)ctx;
	for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++)
	{
		if (*p == s_hwPixFmt)
			return *p;
	}
	return AV_PIX_FMT_NONE;
}

MediaDecoder::MediaDecoder() {}
MediaDecoder::~MediaDecoder() { Close(); }

std::string MediaDecoder::LastError() const
{
	return lastError_;
}

void MediaDecoder::Close()
{
	std::lock_guard<std::mutex> lk(mux);
	if (codec_) avcodec_free_context(&codec_);
	if (hwDeviceCtx_)
	{
		av_buffer_unref(&hwDeviceCtx_);
		hwDeviceCtx_ = nullptr;
	}
	hwPixFmt_ = -1;
	pts = 0;
}

void MediaDecoder::Clear()
{
	std::lock_guard<std::mutex> lk(mux);
	if (codec_) avcodec_flush_buffers(codec_);
}

bool MediaDecoder::Open(AVCodecParameters *para)
{
	return Open(para, false);
}

bool MediaDecoder::Open(AVCodecParameters *para, bool tryHwAccel)
{
	if (!para) return false;
	Close();
	return openInternal(para, tryHwAccel);
}

bool MediaDecoder::openInternal(AVCodecParameters *para, bool tryHwAccel)
{
	lastError_.clear();
	const AVCodec *vcodec = avcodec_find_decoder(para->codec_id);
	if (!vcodec)
	{
		lastError_ = "codec not found";
		cout << "[Decoder] " << lastError_ << ", id=" << para->codec_id << endl;
		avcodec_parameters_free(&para);
		return false;
	}
	cout << "[Decoder] codec: " << vcodec->long_name << endl;

	std::lock_guard<std::mutex> lk(mux);
	codec_ = avcodec_alloc_context3(vcodec);
	if (!codec_)
	{
		avcodec_parameters_free(&para);
		lastError_ = "context allocation failed";
		return false;
	}
	int copyRe = avcodec_parameters_to_context(codec_, para);
	avcodec_parameters_free(&para);
	if (copyRe < 0)
	{
		avcodec_free_context(&codec_);
		lastError_ = "failed to copy codec parameters";
		return false;
	}
	codec_->thread_count = (int)std::thread::hardware_concurrency();

	if (tryHwAccel && vcodec->type == AVMEDIA_TYPE_VIDEO)
	{
		static const AVHWDeviceType kTypes[] = {
#if defined(__APPLE__)
			AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
#if defined(_WIN32)
			AV_HWDEVICE_TYPE_D3D11VA,
			AV_HWDEVICE_TYPE_DXVA2,
#endif
			AV_HWDEVICE_TYPE_VAAPI,
			AV_HWDEVICE_TYPE_CUDA,
			AV_HWDEVICE_TYPE_NONE
		};
		for (int ti = 0; kTypes[ti] != AV_HWDEVICE_TYPE_NONE; ++ti)
		{
			AVHWDeviceType type = kTypes[ti];
			for (int i = 0;; ++i)
			{
				const AVCodecHWConfig *config = avcodec_get_hw_config(vcodec, i);
				if (!config) break;
				if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
					continue;
				if (config->device_type != type) continue;
				if (av_hwdevice_ctx_create(&hwDeviceCtx_, type, nullptr, nullptr, 0) < 0)
				{
					hwDeviceCtx_ = nullptr;
					continue;
				}
				s_hwPixFmt = config->pix_fmt;
				hwPixFmt_ = (int)config->pix_fmt;
				codec_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
				codec_->get_format = getHwFormat;
				cout << "[Decoder] trying HW accel: " << av_hwdevice_get_type_name(type) << endl;
				break;
			}
			if (hwDeviceCtx_) break;
		}
	}

	int re = avcodec_open2(codec_, nullptr, nullptr);
	if (re != 0)
	{
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		lastError_ = std::string("open failed: ") + buf;
		if (hwDeviceCtx_)
		{
			cout << "[Decoder] HW open failed, falling back to software" << endl;
			if (codec_->hw_device_ctx) av_buffer_unref(&codec_->hw_device_ctx);
			av_buffer_unref(&hwDeviceCtx_);
			hwDeviceCtx_ = nullptr;
			hwPixFmt_ = -1;
			codec_->get_format = nullptr;
			re = avcodec_open2(codec_, nullptr, nullptr);
			if (re == 0)
			{
				lastError_.clear();
				cout << "[Decoder] software open OK" << endl;
				return true;
			}
			av_strerror(re, buf, sizeof(buf) - 1);
			lastError_ = std::string("open failed: ") + buf;
		}
		avcodec_free_context(&codec_);
		cout << "[Decoder] " << lastError_ << endl;
		return false;
	}
	cout << "[Decoder] open OK" << (hwDeviceCtx_ ? " (HW)" : " (SW)") << endl;
	return true;
}

void MediaDecoder::SetPacketRecycler(std::function<void(AVPacket*)> recycler)
{
	packetRecycler_ = std::move(recycler);
}

bool MediaDecoder::Send(AVPacket *pkt)
{
	if (!pkt || pkt->size <= 0 || !pkt->data)
	{
		if (pkt)
		{
			if (packetRecycler_) packetRecycler_(pkt);
			else av_packet_free(&pkt);
		}
		return false;
	}
	std::lock_guard<std::mutex> lk(mux);
	if (!codec_)
	{
		if (packetRecycler_) packetRecycler_(pkt);
		else av_packet_free(&pkt);
		return false;
	}
	int re = avcodec_send_packet(codec_, pkt);
	if (packetRecycler_) packetRecycler_(pkt);
	else av_packet_free(&pkt);
	return re == 0 || re == AVERROR(EAGAIN);
}

AVFrame* MediaDecoder::Recv()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!codec_) return nullptr;
	AVFrame *frame = av_frame_alloc();
	if (!frame) return nullptr;
	int re = avcodec_receive_frame(codec_, frame);
	if (re != 0)
	{
		av_frame_free(&frame);
		return nullptr;
	}

	if (hwDeviceCtx_ && frame->format == hwPixFmt_)
	{
		AVFrame *sw = av_frame_alloc();
		if (!sw || av_hwframe_transfer_data(sw, frame, 0) < 0)
		{
			av_frame_free(&sw);
			av_frame_free(&frame);
			return nullptr;
		}
		sw->pts = frame->pts;
		av_frame_free(&frame);
		frame = sw;
	}

	int64_t framePts = frame->pts;
	if (framePts == AV_NOPTS_VALUE)
		framePts = frame->best_effort_timestamp;
	if (framePts == AV_NOPTS_VALUE)
		framePts = 0;
	frame->pts = framePts;
	pts = framePts;
	return frame;
}

SubtitleDecodeResult MediaDecoder::DecodeSubtitle(AVPacket *pkt, long long *startMs, long long *endMs)
{
	SubtitleDecodeResult out;
	if (!pkt) return out;

	AVSubtitle sub{};
	int got = 0;
	{
		std::lock_guard<std::mutex> lk(mux);
		if (!codec_)
		{
			if (packetRecycler_) packetRecycler_(pkt);
			else av_packet_free(&pkt);
			return out;
		}

		long long pktPts = pkt->pts > 0 ? pkt->pts : 0;
		long long pktDur = pkt->duration > 0 ? pkt->duration : 3000;
		int re = avcodec_decode_subtitle2(codec_, &sub, &got, pkt);
		if (packetRecycler_) packetRecycler_(pkt);
		else av_packet_free(&pkt);
		if (re < 0 || !got)
			return out;

		long long s = pktPts;
		long long e = pktPts + pktDur;
		if (sub.end_display_time > sub.start_display_time)
			e = s + (long long)(sub.end_display_time - sub.start_display_time);
		else if (sub.end_display_time > 0)
			e = s + (long long)sub.end_display_time;

		for (unsigned i = 0; i < sub.num_rects; ++i)
		{
			AVSubtitleRect *r = sub.rects[i];
			if (!r) continue;
			if (r->type == SUBTITLE_ASS && r->ass)
			{
				if (!out.text.empty()) out.text.push_back('\n');
				out.text += r->ass;
			}
			else if (r->type == SUBTITLE_TEXT && r->text)
			{
				if (!out.text.empty()) out.text.push_back('\n');
				out.text += r->text;
			}
			else if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0 && r->data[0])
			{
				out.bmpW = r->w;
				out.bmpH = r->h;
				out.rgba.resize((size_t)r->w * (size_t)r->h * 4);
				const uint8_t *pal = r->data[1];
				for (int y = 0; y < r->h; ++y)
				{
					const uint8_t *src = r->data[0] + y * r->linesize[0];
					uint8_t *dst = out.rgba.data() + (size_t)y * (size_t)r->w * 4;
					for (int x = 0; x < r->w; ++x)
					{
						uint8_t idx = src[x];
						if (pal)
						{
							const uint8_t *c = pal + idx * 4;
							dst[x * 4 + 0] = c[0];
							dst[x * 4 + 1] = c[1];
							dst[x * 4 + 2] = c[2];
							dst[x * 4 + 3] = c[3];
						}
						else
						{
							dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = idx;
							dst[x * 4 + 3] = 255;
						}
					}
				}
			}
		}
		avsubtitle_free(&sub);

		if (startMs) *startMs = s;
		if (endMs) *endMs = e > s ? e : (s + 3000);
		pts = s;
	}
	return out;
}
