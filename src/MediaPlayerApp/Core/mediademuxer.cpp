#include "mediademuxer.h"
#include <iostream>
using namespace std;
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

static double r2d(AVRational r)
{
	return r.den == 0 ? 0 : (double)r.num / (double)r.den;
}

static StreamTrackInfo makeTrackInfo(AVStream *st, int index)
{
	StreamTrackInfo info;
	info.streamIndex = index;
	if (!st) return info;
	AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", nullptr, 0);
	AVDictionaryEntry *title = av_dict_get(st->metadata, "title", nullptr, 0);
	if (lang && lang->value) info.language = lang->value;
	if (title && title->value) info.title = title->value;
	if (st->codecpar)
	{
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		info.codec = c && c->name ? c->name : "unknown";
	}
	return info;
}

void MediaDemuxer::setError(const std::string &msg)
{
	lastError_ = msg;
	cout << "[Demuxer] " << msg << endl;
}

std::string MediaDemuxer::LastError() const
{
	return lastError_;
}

bool MediaDemuxer::Open(const char *url)
{
	return Open(url, DemuxOpenOptions{});
}

bool MediaDemuxer::Open(const char *url, const DemuxOpenOptions &openOpts)
{
	Close();
	if (!url || !url[0])
	{
		setError("empty url");
		return false;
	}

	AVDictionary *opts = NULL;
	av_dict_set(&opts, "rtsp_transport", "tcp", 0);
	av_dict_set(&opts, "max_delay", "500", 0);
	av_dict_set(&opts, "reconnect", "1", 0);
	av_dict_set(&opts, "reconnect_streamed", "1", 0);
	av_dict_set(&opts, "rw_timeout", "15000000", 0);
	if (!openOpts.userAgent.empty())
		av_dict_set(&opts, "user_agent", openOpts.userAgent.c_str(), 0);
	else
		av_dict_set(&opts, "user_agent", "FFMediaPlayer/1.0", 0);
	if (!openOpts.headers.empty())
		av_dict_set(&opts, "headers", openOpts.headers.c_str(), 0);

	std::lock_guard<std::mutex> lk(mux);
	eof_ = false;
	lastError_.clear();
	subtitleStream = -1;
	subtitleTracks_.clear();
	audioTracks_.clear();

	int re = avformat_open_input(&ic, url, 0, &opts);
	av_dict_free(&opts);
	if (re != 0)
	{
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		setError(std::string("open failed: ") + url + " (" + buf + ")");
		return false;
	}
	cout << "[Demuxer] opened: " << url << endl;

	re = avformat_find_stream_info(ic, 0);
	if (re < 0)
	{
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		avformat_close_input(&ic);
		setError(std::string("stream info failed: ") + buf);
		return false;
	}

	totalMs = (ic->duration != AV_NOPTS_VALUE && ic->duration > 0)
		? (int)(ic->duration / (AV_TIME_BASE / 1000)) : 0;
	cout << "[Demuxer] duration: " << totalMs << " ms"
		<< (totalMs <= 0 ? " (live/unknown)" : "") << endl;
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
	else
	{
		videoStream = -1;
		cout << "[Demuxer] no video stream" << endl;
	}

	for (unsigned i = 0; i < ic->nb_streams; ++i)
	{
		AVStream *st = ic->streams[i];
		if (!st || !st->codecpar) continue;
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audioTracks_.push_back(makeTrackInfo(st, (int)i));
			cout << "[Demuxer] audio #" << i
				<< " lang=" << audioTracks_.back().language
				<< " codec=" << audioTracks_.back().codec << endl;
		}
		else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
		{
			subtitleTracks_.push_back(makeTrackInfo(st, (int)i));
			cout << "[Demuxer] subtitle #" << i
				<< " lang=" << subtitleTracks_.back().language
				<< " codec=" << subtitleTracks_.back().codec << endl;
		}
	}

	audioStream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audioStream >= 0)
	{
		AVStream *as = ic->streams[audioStream];
		sampleRate = as->codecpar->sample_rate;
		channels = as->codecpar->ch_layout.nb_channels;
		if (sampleRate <= 0 || channels <= 0)
		{
			sampleRate = 0;
			channels = 0;
			setError("audio stream has invalid parameters");
		}
		else
		{
			cout << "[Demuxer] active audio #" << audioStream
				<< " " << sampleRate << "Hz " << channels << "ch" << endl;
		}
	}
	else
	{
		audioStream = -1;
		cout << "[Demuxer] no audio stream" << endl;
	}

	subtitleStream = -1;
	(void)openOpts.tryHwAccel; // consumed by DemuxThread/MediaDecoder
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
	if (ic) avformat_close_input(&ic);
	totalMs = 0;
	width = height = sampleRate = channels = 0;
	videoStream = audioStream = subtitleStream = -1;
	subtitleTracks_.clear();
	audioTracks_.clear();
	eof_ = false;
}

bool MediaDemuxer::Seek(double pos)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return false;
	if (totalMs <= 0)
	{
		setError("seek not supported on live/unknown duration stream");
		return false;
	}
	if (pos < 0.0) pos = 0.0;
	if (pos > 1.0) pos = 1.0;
	int seekStream = videoStream >= 0 ? videoStream : audioStream;
	if (seekStream < 0) return false;

	const AVStream *stream = ic->streams[seekStream];
	int64_t target = 0;
	if (stream->duration != AV_NOPTS_VALUE && stream->duration >= 0)
	{
		int64_t offset = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
		target = offset + (int64_t)(stream->duration * pos);
	}
	else if (ic->duration != AV_NOPTS_VALUE && ic->duration >= 0)
	{
		target = av_rescale_q((int64_t)(ic->duration * pos),
			AV_TIME_BASE_Q, stream->time_base);
	}
	else
	{
		setError("seek failed: no duration");
		return false;
	}
	int re = av_seek_frame(ic, seekStream, target,
		AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME);
	if (re >= 0)
		eof_ = false;
	else
	{
		char buf[1024] = { 0 };
		av_strerror(re, buf, sizeof(buf) - 1);
		setError(std::string("seek failed: ") + buf);
	}
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

AVCodecParameters *MediaDemuxer::CopySPara()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic || subtitleStream < 0) return nullptr;
	if (subtitleStream >= (int)ic->nb_streams) return nullptr;
	AVCodecParameters *pa = avcodec_parameters_alloc();
	if (!pa) return nullptr;
	avcodec_parameters_copy(pa, ic->streams[subtitleStream]->codecpar);
	return pa;
}

bool MediaDemuxer::IsAudio(AVPacket *pkt)
{
	if (!pkt) return false;
	std::lock_guard<std::mutex> lk(mux);
	return audioStream >= 0 && pkt->stream_index == audioStream;
}

bool MediaDemuxer::IsVideo(AVPacket *pkt)
{
	if (!pkt) return false;
	std::lock_guard<std::mutex> lk(mux);
	return videoStream >= 0 && pkt->stream_index == videoStream;
}

bool MediaDemuxer::IsSubtitle(AVPacket *pkt)
{
	if (!pkt) return false;
	std::lock_guard<std::mutex> lk(mux);
	return subtitleStream >= 0 && pkt->stream_index == subtitleStream;
}

std::vector<SubtitleTrackInfo> MediaDemuxer::ListSubtitleTracks()
{
	std::lock_guard<std::mutex> lk(mux);
	return subtitleTracks_;
}

std::vector<AudioTrackInfo> MediaDemuxer::ListAudioTracks()
{
	std::lock_guard<std::mutex> lk(mux);
	return audioTracks_;
}

bool MediaDemuxer::SelectSubtitle(int streamIndex)
{
	std::lock_guard<std::mutex> lk(mux);
	if (streamIndex < 0)
	{
		subtitleStream = -1;
		return true;
	}
	for (const auto &t : subtitleTracks_)
	{
		if (t.streamIndex == streamIndex)
		{
			subtitleStream = streamIndex;
			return true;
		}
	}
	setError("subtitle track not found");
	return false;
}

bool MediaDemuxer::SelectAudio(int streamIndex)
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return false;
	for (const auto &t : audioTracks_)
	{
		if (t.streamIndex != streamIndex) continue;
		if (streamIndex < 0 || streamIndex >= (int)ic->nb_streams) return false;
		AVStream *as = ic->streams[streamIndex];
		if (!as || !as->codecpar) return false;
		audioStream = streamIndex;
		sampleRate = as->codecpar->sample_rate;
		channels = as->codecpar->ch_layout.nb_channels;
		if (sampleRate <= 0 || channels <= 0)
		{
			sampleRate = 0;
			channels = 0;
			setError("selected audio has invalid parameters");
			return false;
		}
		return true;
	}
	setError("audio track not found");
	return false;
}

void MediaDemuxer::SetPacketAllocator(std::function<AVPacket*()> alloc)
{
	packetAlloc_ = std::move(alloc);
}

AVPacket *MediaDemuxer::Read()
{
	std::lock_guard<std::mutex> lk(mux);
	if (!ic) return nullptr;
	AVPacket *pkt = packetAlloc_ ? packetAlloc_() : av_packet_alloc();
	if (!pkt) pkt = av_packet_alloc();
	if (!pkt) return nullptr;
	int re = av_read_frame(ic, pkt);
	if (re != 0)
	{
		av_packet_unref(pkt);
		av_packet_free(&pkt);
		eof_ = (re == AVERROR_EOF);
		if (!eof_)
		{
			char buf[256] = { 0 };
			av_strerror(re, buf, sizeof(buf) - 1);
			lastError_ = std::string("read error: ") + buf;
		}
		return nullptr;
	}
	if (pkt->stream_index < 0 || pkt->stream_index >= (int)ic->nb_streams)
	{
		av_packet_unref(pkt);
		av_packet_free(&pkt);
		return nullptr;
	}
	eof_ = false;
	AVRational tb = ic->streams[pkt->stream_index]->time_base;
	if (pkt->pts != AV_NOPTS_VALUE)
		pkt->pts = av_rescale_q(pkt->pts, tb, AVRational{1, 1000});
	else
		pkt->pts = 0;
	if (pkt->dts != AV_NOPTS_VALUE)
		pkt->dts = av_rescale_q(pkt->dts, tb, AVRational{1, 1000});
	else
		pkt->dts = 0;
	if (pkt->duration > 0)
		pkt->duration = av_rescale_q(pkt->duration, tb, AVRational{1, 1000});
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
