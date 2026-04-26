#include "mediademuxer.h"
#include <iostream>
using namespace std;
extern "C" {
#include <libavformat/avformat.h>
}

static double r2d(AVRational r)
{
	return r.den == 0 ? 0 : (double)r.num / (double)r.den;
}

bool MediaDemuxer::Open(const char *url)
{
	Close();
	AVDictionary *opts = NULL;
	av_dict_set(&opts, "rtsp_transport", "tcp", 0);
	av_dict_set(&opts, "max_delay", "500", 0);

	std::lock_guard<std::mutex> lk(mux);
	int re = avformat_open_input(&ic, url, 0, &opts);
	av_dict_free(&opts);
	if (re != 0)
	{
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		cout << "[Demuxer] open failed: " << url << " reason: " << buf << endl;
		return false;
	}
	cout << "[Demuxer] opened: " << url << endl;

	re = avformat_find_stream_info(ic, 0);
	totalMs = ic->duration / (AV_TIME_BASE / 1000);
	cout << "[Demuxer] duration: " << totalMs << " ms" << endl;
	av_dump_format(ic, 0, url, 0);

	videoStream = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (videoStream >= 0)
	{
		AVStream *as = ic->streams[videoStream];
		width = as->codecpar->width;
		height = as->codecpar->height;
		cout << "[Demuxer] video #" << videoStream
			<< " " << width << "x" << height
			<< " codec=" << as->codecpar->codec_id
			<< " fps=" << r2d(as->avg_frame_rate) << endl;
	}
	else { videoStream = -1; cout << "[Demuxer] no video stream" << endl; }

	audioStream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audioStream >= 0)
	{
		AVStream *as = ic->streams[audioStream];
		sampleRate = as->codecpar->sample_rate;
		channels = as->codecpar->ch_layout.nb_channels;
		cout << "[Demuxer] audio #" << audioStream
			<< " " << sampleRate << "Hz " << channels << "ch"
			<< " codec=" << as->codecpar->codec_id << endl;
	}
	else { audioStream = -1; cout << "[Demuxer] no audio stream" << endl; }

	return true;
}

void MediaDemuxer::Clear()
{
	std::lock_guard<std::mutex> lk(mux);
	if (ic) avformat_flush(ic);
}

void MediaDemuxer::Close()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return;
	avformat_close_input(&ic);
	totalMs = 0;
}

bool MediaDemuxer::Seek(double pos)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return false;
	long long seekPos = 0;
	if (videoStream >= 0)
		seekPos = ic->streams[videoStream]->duration * pos;
	int re = av_seek_frame(ic, videoStream, seekPos, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME);
	return re >= 0;
}

AVCodecParameters *MediaDemuxer::CopyVPara()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic || videoStream < 0) return nullptr;
	AVCodecParameters *pa = avcodec_parameters_alloc();
	if (!pa) return nullptr;
	avcodec_parameters_copy(pa, ic->streams[videoStream]->codecpar);
	return pa;
}

AVCodecParameters *MediaDemuxer::CopyAPara()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic || audioStream < 0) return nullptr;
	AVCodecParameters *pa = avcodec_parameters_alloc();
	if (!pa) return nullptr;
	avcodec_parameters_copy(pa, ic->streams[audioStream]->codecpar);
	return pa;
}

bool MediaDemuxer::IsAudio(AVPacket *pkt)
{
	if (!pkt) return false;
	return pkt->stream_index == audioStream;
}

AVPacket *MediaDemuxer::ReadVideo()
{
	AVPacket *pkt = nullptr;
	for (int i = 0; i < 20; i++)
	{
		pkt = Read();
		if (!pkt) break;
		if (pkt->stream_index == videoStream) break;
		av_packet_free(&pkt);
	}
	return pkt;
}

AVPacket *MediaDemuxer::Read()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return nullptr;
	AVPacket *pkt = av_packet_alloc();
	int re = av_read_frame(ic, pkt);
	if (re != 0)
	{
		av_packet_free(&pkt);
		return nullptr;
	}
	AVRational tb = ic->streams[pkt->stream_index]->time_base;
	if (pkt->pts != AV_NOPTS_VALUE)
		pkt->pts = pkt->pts * 1000 * r2d(tb);
	else
		pkt->pts = 0;
	if (pkt->dts != AV_NOPTS_VALUE)
		pkt->dts = pkt->dts * 1000 * r2d(tb);
	else
		pkt->dts = 0;
	return pkt;
}

MediaDemuxer::MediaDemuxer()
{
	static bool isFirst = true;
	static std::mutex dmux;
	std::lock_guard<std::mutex> lk(dmux);
	if (isFirst) { avformat_network_init(); isFirst = false; }
}

MediaDemuxer::~MediaDemuxer() { Close(); }
