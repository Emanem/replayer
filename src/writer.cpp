/*
    This file is part of replayer.

    replayer is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    replayer is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with replayer.  If not, see <https://www.gnu.org/licenses/>.
 * */

#include "writer.h"
#include <thread>
#include <iostream>

namespace {
	class impl : public writer::iface {
		writer::params		params_;
		writer::frame_queue&	fq_;
		std::atomic<bool>	run_;
		std::thread		*th_;

		void run(void) {
			using namespace utils;

			const char	*outfile = "output.mkv";
			AVOutputFormat  *ofmt = av_guess_format(0, outfile, 0);
			if(!ofmt)
				throw std::runtime_error("av_guess_format");
			AVFormatContext	*octx_ = 0;
			averror(avformat_alloc_output_context2(&octx_, ofmt, 0, outfile));
			std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>	octx(octx_, [](AVFormatContext* p){ if(p) avformat_free_context(p); });
			auto		*penc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
			if(!penc)
				throw std::runtime_error("avcodec_find_encoder");
			AVStream	*strm = avformat_new_stream(octx.get(), penc);
			if(!strm)
				throw std::runtime_error("avformat_new_stream");
			strm->time_base = (AVRational){1, params_.fps};
			strm->avg_frame_rate = (AVRational){params_.fps, 1};
			auto		*pc = avcodec_alloc_context3(penc);
			std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>	ocodec(pc, [](AVCodecContext* p){ if(p) avcodec_free_context(&p); });
			// setup additinal info about codec
			ocodec->pix_fmt  = AV_PIX_FMT_YUV420P;
			ocodec->bit_rate = 40*1000*1000;
			ocodec->width = params_.ccodec->width;
			ocodec->height = params_.ccodec->height;
			ocodec->time_base = (AVRational){1, params_.fps};
			ocodec->framerate = (AVRational){params_.fps, 1};
			ocodec->gop_size = 12;
			ocodec->max_b_frames = 1;
			// fix about global headers
			if(octx->oformat->flags & AVFMT_GLOBALHEADER)
				octx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			// bind context codec
			averror(avcodec_open2(ocodec.get(), penc, 0));
			// fill in the context parameters
			averror(avcodec_parameters_from_context(strm->codecpar, ocodec.get()));
			// in case we have to create a file, do it...
			if(!(octx->flags & AVFMT_NOFILE)) {
				averror(avio_open2(&octx->pb , outfile , AVIO_FLAG_WRITE, 0, 0));
			}
			// check we have at least 1 stream...
			if(!octx->nb_streams)
				throw std::runtime_error("We have no output streams");
			// write the header
			averror(avformat_write_header(octx.get(), 0));
			// add the context to convert frames...
			SwsContext	*swsctx = sws_getContext(params_.ccodec->width,
	                	params_.ccodec->height,
	                	params_.ccodec->pix_fmt,
	                	ocodec->width,
				ocodec->height,
	                	ocodec->pix_fmt,
	                	SWS_BICUBIC, NULL, NULL, NULL);
			// output frame
			std::unique_ptr<AVFrame, void(*)(AVFrame*)>	oframe(av_frame_alloc(), [](AVFrame* p){ if(p) av_frame_free(&p); });
			oframe->width = ocodec->width;
			oframe->height = ocodec->height;
			oframe->format = AV_PIX_FMT_YUV420P;
			const int	nbytes = av_image_get_buffer_size(ocodec->pix_fmt, ocodec->width, ocodec->height, 32);
			uint8_t		video_buffer[nbytes];
			averror(av_image_fill_arrays(oframe->data, oframe->linesize, video_buffer, AV_PIX_FMT_YUV420P, ocodec->width, ocodec->height, 1));
			// packet, reference
			std::unique_ptr<AVPacket, void(*)(AVPacket*)>	opkt(av_packet_alloc(), [](AVPacket* p){ if(p) av_packet_free(&p); });
			// main loop
			// write all frames
			int	written_frames = 0;
			int64_t	iter = 1;
			while(true) {
				frame_holder*	fh = 0;
				if(!fq_.pop(fh)) {
					if(!run_)
						break;
					continue;
				}
				// TODO Use newer API
				sws_scale(swsctx, fh->frame->data, fh->frame->linesize, 0, ocodec->height, oframe->data, oframe->linesize);
				oframe->pts = iter++;
				averror(avcodec_send_frame(ocodec.get(), oframe.get()));
				const int	rv = avcodec_receive_packet(ocodec.get(), opkt.get());
				if(rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
					av_frame_unref(fh->frame.get());
					fh->release();
           					continue;
				} else if(rv < 0) {
					averror(rv);
				} else {
					if(opkt->pts != AV_NOPTS_VALUE)
						opkt->pts = av_rescale_q(opkt->pts, ocodec->pkt_timebase, strm->time_base);
					if(opkt->dts != AV_NOPTS_VALUE)
						opkt->dts = av_rescale_q(opkt->dts, ocodec->pkt_timebase, strm->time_base);
					averror(av_write_frame(octx.get(), opkt.get()));
					av_packet_unref(opkt.get());
					++written_frames;
				}
				av_frame_unref(fh->frame.get());
				fh->release();
			}
			// one last step to close the file
			if(written_frames) {
				averror(avcodec_send_frame(ocodec.get(), 0));
				avcodec_receive_packet(ocodec.get(), opkt.get());
				averror(av_write_frame(octx.get(), opkt.get()));
				av_packet_unref(opkt.get());
			}
			// close off all the streams
			averror(av_write_trailer(octx.get()));
			std::cout << "Written " << written_frames << " frames" << std::endl;
		}
	public:
		impl(const writer::params& p, writer::frame_queue& fq) : params_(p), fq_(fq), run_(true), th_(0) {
		}

		void start(void) {
			if(th_)
				throw std::runtime_error("already running");
			th_ = new std::thread(
				[this]() -> void {
					try {
						run();
					} catch(const std::exception& e) {
						std::cerr << "[f_writer] Exception: " <<  e.what() << std::endl;
						std::exit(-1);
					} catch(...) {
						std::cerr << "[f_writer] Unknown exception" << std::endl;
						std::exit(-1);
					}
				}
			);
		}

		void stop(void) {
			if(!th_)
				return;
			run_ = false;
			th_->join();
			delete th_;
			th_ = 0;
			run_ = true;
		}

		~impl() {
			stop();
		}
	};
}

writer::iface* writer::init(const writer::params& p, writer::frame_queue& fq) {
	return new impl(p, fq);
}

