#include "mediadecoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <iostream>
using namespace std;

void XFreePacket(AVPacket **pkt)
{
	if (!pkt || !(*pkt)) return;
	av_packet_free(pkt);
}

void XFreeFrame(AVFrame **frame)
{
	if (!frame || !(*frame)) return;
	av_frame_free(frame);
}

void MediaDecoder::Close()
{
	std::lock_guard<std::mutex> lk(mux);
	if (codec_) avcodec_free_context(&codec_);
	pts = 0;
}

void MediaDecoder::Clear()
{
	std::lock_guard<std::mutex> lk(mux);
	if (codec_) avcodec_flush_buffers(codec_);
}

bool MediaDecoder::Open(AVCodecParameters *para)
{
	if (!para) return false;
	Close();

	const AVCodec *vcodec = avcodec_find_decoder(para->codec_id);
	if (!vcodec)
	{
		cout << "[Decoder] codec not found, id=" << para->codec_id << endl;
		avcodec_parameters_free(&para);
		return false;
	}
	cout << "[Decoder] codec: " << vcodec->long_name << endl;

	std::lock_guard<std::mutex> lk(mux);
	codec_ = avcodec_alloc_context3(vcodec);
	avcodec_parameters_to_context(codec_, para);
	avcodec_parameters_free(&para);
	codec_->thread_count = 8;

	int re = avcodec_open2(codec_, 0, 0);
	if (re != 0)
	{
		avcodec_free_context(&codec_);
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		cout << "[Decoder] open failed: " << buf << endl;
		return false;
	}
	cout << "[Decoder] open OK" << endl;
	return true;
}

bool MediaDecoder::Send(AVPacket *pkt)
{
	if (!pkt || pkt->size <= 0 || !pkt->data)
	{
		if (pkt) av_packet_free(&pkt);
		return false;
	}
	std::lock_guard<std::mutex> lk(mux);
	if (!codec_) { av_packet_free(&pkt); return false; }
	int re = avcodec_send_packet(codec_, pkt);
	av_packet_free(&pkt);
	return re == 0;
}

AVFrame* MediaDecoder::Recv()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!codec_) return nullptr;
	AVFrame *frame = av_frame_alloc();
	int re = avcodec_receive_frame(codec_, frame);
	if (re != 0)
	{
		av_frame_free(&frame);
		return nullptr;
	}
	pts = frame->pts;
	return frame;
}

MediaDecoder::MediaDecoder() {}
MediaDecoder::~MediaDecoder() { Close(); }

AVRational MediaDecoder::getTimeBase() const
{
	std::lock_guard<std::mutex> lk(mux);
	return codec_ ? codec_->time_base : AVRational{1, 1000};
}
