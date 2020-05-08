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
#include "utils.h"
#include "writer.h"
#include <thread>
#include <fstream>

extern "C" {
	extern AVInputFormat ff_xcompgrab_demuxer;
}

namespace {
	void ppm_write(AVStream *st, AVPacket& pkt, int seq) {
		std::ofstream ppm((std::string("out") + std::to_string(seq) + ".ppm").c_str());
		ppm << "P3\n";
		ppm << st->codecpar->width << " " << st->codecpar->height << '\n';
		ppm << "255\n";
		for(int i = 0; i < st->codecpar->width*st->codecpar->height; ++i) {
			const uint8_t	*data = &pkt.buf->data[i*4];
			ppm << (int)data[0] << " " << (int)data[1] << " " << (int)data[2] << '\n';
		}
	}
}

int main(int argc, char *argv[]) {
	try {
		using namespace utils;

		// Initial setup
		av_register_all();
		avdevice_register_all();
		bool		useX11grab = true,
				writeOutput = false;
		// fps value
		const int	FPS = 30;
		AVFormatContext	*fctx_ = 0;
		// HW decode sample https://ffmpeg.org/doxygen/3.4/hw__decode_8c_source.html
		if(useX11grab) {
			auto*	x11format = av_find_input_format("x11grab");
			if(!x11format)
				throw std::runtime_error("av_find_input_format - can't find 'x11grab'");
			// open x11grab
			AVDictionary	*opt = 0;
			av_dict_set(&opt, "framerate", std::to_string(FPS).c_str(), 0);
			av_dict_set(&opt, "video_size", "1720x1376" /*"3440x1440"*/, 0);
			averror(avformat_open_input(&fctx_, ":0.0", x11format, &opt));
			// this is not great... but still
			av_dict_free(&opt);
		} else {
			auto*	xcompformat = &ff_xcompgrab_demuxer;
			if(!xcompformat)
				throw std::runtime_error("av_find_input_format - can't find 'xcompgrab'");
			// open xcompgrab
			AVDictionary	*opt = 0;
			av_dict_set(&opt, "framerate", std::to_string(FPS).c_str(), 0);
			av_dict_set(&opt, "window_name", (argc > 1) ? argv[1] : "Firefox", 0);
			//av_dict_set_int(&opt, "use_framebuf", 1, 0);
			averror(avformat_open_input(&fctx_, "", xcompformat, &opt));
			// this is not great... but still
			av_dict_free(&opt);
		}
		// embed in a unique_ptr to leverage RAII
		std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)>	fctx(fctx_, [](AVFormatContext* p){ if(p) avformat_close_input(&p); });
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
		const int	MAX_FRAMES = 10*FPS;
		int		cur_frame = 0;
		AVPacket	packet = {0};
		// structures to share data between threads
		// the 'screen-reader' (main) and output 'writer'
		concurrent_deque<frame_holder*>	c_deq;
		frame_buffers			frame_bufs(16);
		std::unique_ptr<writer::iface>	cur_writer(writer::init(writer::params{FPS, ccodec.get()}, c_deq));
		cur_writer->start();
		// embed in a unique_ptr to leverage RAII
		while(av_read_frame(fctx.get(), &packet) >= 0) {
			if(vstream == packet.stream_index) {
				//ppm_write(fctx->streams[vstream], packet, cur_frame);
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
						if(writeOutput) c_deq.push(cur_fh);
						else {
							av_frame_unref(cur_fh->frame.get());
							cur_fh->release();
						}
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
		cur_writer->stop();
	} catch(const std::exception& e) {
		std::cerr << "Exception: " << e.what() << std::endl;
	} catch(...) {
		std::cerr << "Unknown exception" << std::endl;
	}
}

