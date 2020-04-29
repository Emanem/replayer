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

// reference https://github.com/abdullahfarwees/screen-recorder-ffmpeg-cpp/blob/master/src/ScreenRecorder.cpp

#include <iostream>
#include <memory>
// needed because of C libraries
extern "C" {
	#include <libavformat/avformat.h> // libavcodec-dev libavformat-dev libavutil-dev
	#include <libavdevice/avdevice.h> // libavdevice-dev
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
}
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

namespace {
	template<typename T>
	class concurrent_deque {
		std::mutex		mtx_;
		std::condition_variable	cv_;
		std::deque<T>		d_;
	public:
		void push(const T& in) {
			std::unique_lock<std::mutex>	ul(mtx_);
			d_.push_back(in);
			cv_.notify_all();
		}

		bool pop(T& out, size_t tmout_ms = 100) {
			std::unique_lock<std::mutex>	ul(mtx_);
			if(!cv_.wait_for(ul, std::chrono::milliseconds(tmout_ms), [this](){ return !d_.empty(); }))
				return false;
			out = *d_.begin();
			d_.pop_front();
			return true;
		}
	};
}

namespace {
	struct frame_holder {
		std::unique_ptr<AVFrame, void(*)(AVFrame*)>	frame;
		std::atomic<bool>				used;
		uint8_t						padding[40];

		frame_holder() : frame(av_frame_alloc(), [](AVFrame* p){ if(p) av_frame_free(&p); }), used(false) {
		}

		bool try_lock(void) {
			bool	v = false;
			if(used.compare_exchange_strong(v, true))
				return true;
			return false;
		}

		void release(void) {
			bool	v = true;
			if(!used.compare_exchange_strong(v, false))
				throw std::runtime_error("This is not possible!");
		}
	};

	static_assert(sizeof(frame_holder) == 64, "frame_holder must be size of cacheline");

	class frame_buffers {
		const size_t		n_;
	public:
		frame_holder		*fh_;
	public:
		frame_buffers(const size_t n) : n_(n), fh_(new frame_holder[n]) {
		}

		~frame_buffers() {
			delete [] fh_;
		}

		frame_holder* get_one(void) {
			for(size_t i = 0; i < n_; ++i) {
				if(fh_[i].try_lock()) {
					return &fh_[i];
				}
			}
			return 0;
		}
	};
}

namespace {
	void averror(const int err) {
		if(err < 0) {
			char	buf[512];
			if(av_strerror(err, buf, 512))
				std::sprintf(buf, "[libav] unknown error code %d", err);
			buf[511] = '\0';
			throw std::runtime_error((std::string("[libav] ") + buf).c_str());
		}
	}
}

int main(int argc, char *argv[]) {
	try {
		// Initial setup
		av_register_all();
		avdevice_register_all();
		// HW decode sample https://ffmpeg.org/doxygen/3.4/hw__decode_8c_source.html
		// get X11
		auto*	x11format = av_find_input_format("x11grab");
		if(!x11format)
			throw std::runtime_error("av_find_input_format - can't find 'x11grab'");
		// open x11grab
		AVFormatContext	*fctx_ = 0;
		AVDictionary	*opt = 0;
		av_dict_set(&opt, "framerate", "30", 0);
		av_dict_set(&opt, "video_size", "3440x1440", 0);
		averror(avformat_open_input(&fctx_, ":0.0", x11format, &opt));
		// this is not great... but still
		av_dict_free(&opt);
		// embed in a unique_ptr to leverage RAII
		std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>	fctx(fctx_, [](AVFormatContext* p){ if(p) {avformat_close_input(&p);} });
		// need to allocate a decoder for the
		// video stream
		// 1. find the video stream
		int	vstream = -1;
		for(size_t i = 0; i < fctx->nb_streams; ++i) {
			auto*	cctx = fctx->streams[i];
			// find first video
			if(cctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				vstream = i;
				break;
			}
		}
		if(-1 == vstream)
			throw std::runtime_error("Can't find video stream");
		// find and initialize the decoder
		auto*	dec = avcodec_find_decoder(fctx->streams[vstream]->codecpar->codec_id);
		if(!dec)
			throw std::runtime_error("Can't find decoder");
		// embed in a unique_ptr to leverage RAII
		std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>	ccodec(avcodec_alloc_context3(dec), [](AVCodecContext* p){ if(p) {avcodec_free_context(&p);} });
		if(!ccodec.get())
			throw std::runtime_error("avcodec_alloc_context3");
		averror(avcodec_parameters_to_context(ccodec.get(), fctx->streams[vstream]->codecpar));
		// initialize the decoder
		averror(avcodec_open2(ccodec.get(), dec, 0));
		// TODO need to understand why one should
		// allocate a new AVCodecContext and not
		// use the existing one...
		// Even the example at https://ffmpeg.org/doxygen/trunk/doc_2examples_2filtering_video_8c-example.html
		// still use the deprecated member...
		//averror(avcodec_open2(fctx->streams[vstream]->codec, dec, 0));
		// try to read n frames
		const int	MAX_FRAMES = 1000;
		int		cur_frame = 0;
		AVPacket	packet = {0};
		// structures to share data between threads
		// the 'screen-reader' (main) and output 'writer'
		concurrent_deque<frame_holder*>	c_deq;
		frame_buffers			frame_bufs(16);
		std::atomic<bool>		run(true);
		auto				fn_write = [&c_deq, &run, &ccodec]() -> void {
			const char	*outfile = "output.mp4";
			AVFormatContext	*octx_ = 0;
			averror(avformat_alloc_output_context2(&octx_, 0, 0, outfile));
			std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>	octx(octx_, [](AVFormatContext* p){ if(p) {avformat_close_input(&p);} });
			AVOutputFormat	*fmt = av_guess_format(0, outfile, 0);
			if(!fmt)
				throw std::runtime_error("av_guess_format");
			AVStream	*strm = avformat_new_stream(octx.get(), 0);
			if(!strm)
				throw std::runtime_error("avformat_new_stream");
			auto		*penc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
			if(!penc)
				throw std::runtime_error("avcodec_find_encoder");
			auto		*pc = avcodec_alloc_context3(penc);
			std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>	ocodec(pc, [](AVCodecContext* p){ if(p) avcodec_free_context(&p); });
			// setup additinal info about codec
			ocodec->pix_fmt  = AV_PIX_FMT_YUV420P;
			//ocodec->bit_rate = 400000;
			ocodec->width = 3440;
			ocodec->height = 1440;
			ocodec->time_base = (AVRational){1, 60};
			ocodec->framerate = (AVRational){60, 1};
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
			SwsContext	*swsctx = sws_getContext(ccodec->width,
		                ccodec->height,
		                ccodec->pix_fmt,
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
			// main loop
			// write all frames
			while(true) {
				frame_holder*	fh = 0;
				if(!c_deq.pop(fh)) {
					if(!run)
						break;
					continue;
				}
				// TODO Use newer API
				AVPacket	outp = {0};
				// sws_scale(swsCtx_, pAVFrame->data, pAVFrame->linesize,0, pAVCodecContext->height, outFrame->data,outFrame->linesize);
				sws_scale(swsctx, fh->frame->data, fh->frame->linesize, 0, ocodec->height, oframe->data, oframe->linesize);
				av_init_packet(&outp);
				outp.data = 0;
				outp.size = 0;
				int	got_pic = 0;
				averror(avcodec_encode_video2(ocodec.get(), &outp, oframe.get(), &got_pic));
				if(got_pic) {
					averror(av_write_frame(octx.get(), &outp));
				}
				av_packet_unref(&outp);
				//
				av_frame_unref(fh->frame.get());
				fh->release();
			}
			// close off all the streams
			averror(av_write_trailer(octx.get()));
		};
		std::thread			f_writer(
			[&fn_write]() -> void {
				try {
					fn_write();
				} catch(const std::exception& e) {
					std::cerr << "[f_writer] Exception: " <<  e.what() << std::endl;
					std::exit(-1);
				} catch(...) {
					std::cerr << "[f_writer] Unknown exception" << std::endl;
					std::exit(-1);
				}
			}
		);
		// embed in a unique_ptr to leverage RAII
		while(av_read_frame(fctx.get(), &packet) >= 0) {
			if(vstream == packet.stream_index) {
				// deprecated
				/*int	got_picture = 0;
				averror(avcodec_decode_video2(ccodec.get(), frame.get(), &got_picture, &packet));
				if(got_picture) {
					if(cur_frame++ >= MAX_FRAMES)
						break;
				}*/
				averror(avcodec_send_packet(ccodec.get(), &packet));
				while(1) {
					// get a frame
					auto*		cur_fh = frame_bufs.get_one();
					int		iter = 0;
					while(!cur_fh) {
						++iter;
						std::this_thread::yield();
						cur_fh = frame_bufs.get_one();
					}
					if(iter) std::cout << "Had to wait: " << iter << " iterations..." << std::endl;
					const int	rv = avcodec_receive_frame(ccodec.get(), cur_fh->frame.get());
					if(!rv) {
						cur_frame++;
						std::printf("Frame %d\r", cur_frame);
						std::fflush(stdout);
						c_deq.push(cur_fh);
					}
					if(AVERROR(EAGAIN) == rv) {
						cur_fh->release();
						break;
					}
					averror(rv);
				}
			}
			av_packet_unref(&packet);
			if(cur_frame >= MAX_FRAMES)
				break;
		}
		// join the writer
		run = false;
		f_writer.join();
	} catch(const std::exception& e) {
		std::cerr << "Exception: " << e.what() << std::endl;
	} catch(...) {
		std::cerr << "Unknown exception" << std::endl;
	}
}

