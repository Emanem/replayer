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

#ifndef _UTILS_H_
#define _UTILS_H_

#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
// needed because of C libraries
extern "C" {
	#include <libavformat/avformat.h> // libavcodec-dev libavformat-dev libavutil-dev
	#include <libavdevice/avdevice.h> // libavdevice-dev
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
}

namespace utils {
	template<typename T>
	class concurrent_deque {
		std::mutex		mtx_;
		std::condition_variable	cv_;
		std::deque<T>		d_;
	public:
		inline void push(const T& in) {
			std::unique_lock<std::mutex>	ul(mtx_);
			d_.push_back(in);
			cv_.notify_all();
		}

		inline bool pop(T& out, size_t tmout_ms = 100) {
			std::unique_lock<std::mutex>	ul(mtx_);
			if(!cv_.wait_for(ul, std::chrono::milliseconds(tmout_ms), [this](){ return !d_.empty(); }))
				return false;
			out = *d_.begin();
			d_.pop_front();
			return true;
		}
	};

	struct frame_holder {
		std::unique_ptr<AVFrame, void(*)(AVFrame*)>	frame;
		std::atomic<bool>				used;
		uint8_t						padding[40];

		frame_holder() : frame(av_frame_alloc(), [](AVFrame* p){ if(p) av_frame_free(&p); }), used(false) {
		}

		inline bool try_lock(void) {
			bool	v = false;
			if(used.compare_exchange_strong(v, true))
				return true;
			return false;
		}

		inline void release(void) {
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

		inline frame_holder* get_one(void) {
			for(size_t i = 0; i < n_; ++i) {
				if(fh_[i].try_lock()) {
					return &fh_[i];
				}
			}
			return 0;
		}
	};

	inline void averror(const int err) {
		if(err < 0) {
			char	buf[512];
			if(av_strerror(err, buf, 512))
				std::sprintf(buf, "[libav] unknown error code %d", err);
			buf[511] = '\0';
			throw std::runtime_error((std::string("[libav] ") + buf).c_str());
		}
	}
}

#endif //_UTILS_H_

