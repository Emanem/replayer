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

#ifndef _WRITER_H_
#define _WRITER_H_

#include "utils.h"

namespace writer {
	typedef utils::concurrent_deque<utils::frame_holder*>	frame_queue;

	struct params {
		int		fps;
		AVCodecContext	*ccodec;
	};

	class iface {
	public:
		virtual void start(void) = 0;
		virtual void stop(void) = 0;
		virtual ~iface() {}
	};

	extern iface* init(const params& p, frame_queue& fq);
}

#endif //_WRITER_H_

