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

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/parseutils.h>
#include <libavutil/time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <errno.h>

/* taking inspiration from both
 * https://github.com/FFmpeg/FFmpeg/blob/e931119a41d0c48d1c544af89768b119b13feb4d/libavdevice/xcbgrab.c
 * https://github.com/obsproject/obs-studio/blob/master/plugins/linux-capture/xcompcap-main.cpp
 * https://github.com/obsproject/obs-studio/blob/master/plugins/linux-capture/xcompcap-helper.cpp
 */

/* TODO: We should include
 * #include <libavformat/internal.h>
 * And then remove the below code, should use
 * properly defined 'avpriv_set_pts_info'
 */
static void avpriv_set_pts_info(AVStream *s, int pts_wrap_bits, unsigned int pts_num, unsigned int pts_den) {
	AVRational new_tb;
	if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
		if (new_tb.num != pts_num)
			av_log(NULL, AV_LOG_DEBUG, "st:%d removing common factor %d from timebase\n", s->index, pts_num / new_tb.num);
	} else av_log(NULL, AV_LOG_WARNING, "st:%d has too large timebase, reducing\n", s->index);

	if (new_tb.num <= 0 || new_tb.den <= 0) {
		av_log(NULL, AV_LOG_ERROR, "Ignoring attempt to set invalid timebase %d/%d for st:%d\n", new_tb.num, new_tb.den, s->index);
		return;
	}
	s->time_base     = new_tb;
	//s->internal->avctx->pkt_timebase = new_tb;
	s->pts_wrap_bits = pts_wrap_bits;
}

typedef struct XCompGrabCtx {
	Display			*xdisplay;
	Window			win_capture;
	Pixmap			win_pixmap;
	XWindowAttributes	win_attr;
	GLXContext		gl_ctx;
	GLXPixmap		gl_pixmap;
	GLuint			gl_texmap;
	const char 		*framerate;
	const char		*window_name;
	int64_t			time_frame;
	AVRational		time_base;
	int64_t			frame_duration;
} XCompGrabCtx;

#define OFFSET(x) offsetof(XCompGrabCtx, x)
#define D AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
	{ "framerate", "", OFFSET(framerate), AV_OPT_TYPE_STRING, {.str = "ntsc" }, 0, 0, D },
	{ "window_name", "", OFFSET(window_name), AV_OPT_TYPE_STRING, {.str = "Desktop" }, 0, 0, D },
	{ NULL },
};

static const AVClass xcompgrab_class = {
    .class_name = "xcompgrab indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

/* Fwd declaration */
static av_cold int xcompgrab_read_close(AVFormatContext *s);

static int get_root_window_screen(Display *xdisplay, Window root) {
	XWindowAttributes attr;
	if (!XGetWindowAttributes(xdisplay, root, &attr))
		return DefaultScreen(xdisplay);
	return XScreenNumberOfScreen(attr.screen);
}

static int init_pvt_stream(AVFormatContext *s) {
	int		rv = 0;
	XCompGrabCtx	*c = s->priv_data;

	AVStream	*st = avformat_new_stream(s, NULL);
	if (!st)
        	return AVERROR(ENOMEM);
	rv = av_parse_video_rate(&st->avg_frame_rate, c->framerate);
	if(rv < 0)
		return rv;
	avpriv_set_pts_info(st, 64, 1, 1000000);
	st->codecpar->format = AV_PIX_FMT_RGBA;
	st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
	st->codecpar->width = c->win_attr.width;
	st->codecpar->height = c->win_attr.height;
	st->codecpar->bit_rate = av_rescale(32*c->win_attr.width*c->win_attr.height, st->avg_frame_rate.num, st->avg_frame_rate.den);
	/* useful to determine the sleep interval */
	/* TODO: verify why we need time_base and
	 * frame_duration ... after all we already
	 * have st->avg_frame_rate, right?
	 */
	c->time_base  = (AVRational){ st->avg_frame_rate.den, st->avg_frame_rate.num};
	c->frame_duration = av_rescale_q(1, c->time_base, AV_TIME_BASE_Q);
	c->time_frame = av_gettime();
	return 0;
}

static av_cold int xcompgrab_read_header(AVFormatContext *s) {
	int		rv = 0;
	XCompGrabCtx	*c = s->priv_data;

	c->xdisplay = XOpenDisplay(NULL);
	if(!c->xdisplay)
		return AVERROR(ENOTSUP);
	/* check composite extension is supported */
	{
		int	eventBase,
			errorBase,
			major = 0,
			minor = 2;

		if (!XCompositeQueryExtension(c->xdisplay, &eventBase, &errorBase)) {
			av_log(s, AV_LOG_ERROR, "XComposite extension not supported");
			xcompgrab_read_close(s);
			return AVERROR(ENOTSUP);
		}

		XCompositeQueryVersion(c->xdisplay, &major, &minor);
		if (major == 0 && minor < 2) {
			av_log(s, AV_LOG_ERROR, "XComposite extension is too old: %d.%d < 0.2", major, minor);
			xcompgrab_read_close(s);
			return AVERROR(ENOTSUP);
		}
	}
	/* find the window name */
	{
		Window		rootWindow = RootWindow(c->xdisplay, DefaultScreen(c->xdisplay));
    		Atom		atom = XInternAtom(c->xdisplay, "_NET_CLIENT_LIST", 1);
    		Atom		actualType;
    		int		format;
    		unsigned long	numItems,
    				bytesAfter;
		unsigned char	*data = '\0';
    		Window 		*list;    
    		char		*win_name = 0;
		c->win_capture = 0;
		int		status = XGetWindowProperty(c->xdisplay, rootWindow, atom, 0L, (~0L), 0,
        				AnyPropertyType, &actualType, &format, &numItems, &bytesAfter, &data);
    		list = (Window *)data;
		if (status >= Success && numItems) {
        		for (int i = 0; i < numItems; ++i) {
            			status = XFetchName(c->xdisplay, list[i], &win_name);
            			if (status >= Success && win_name) {
                			if (strstr(win_name, c->window_name)) {
                    				c->win_capture = list[i];
						XFree(win_name);
                    				break;
                			}
					else XFree(win_name);
            			}
        		}
    		}
		XFree(data);
	}
	if(!c->win_capture) {
		av_log(s, AV_LOG_ERROR, "Can't find X window containing string '%s'", c->window_name);
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	/* TODO: we should manage errors! */
	XCompositeRedirectWindow(c->xdisplay, c->win_capture, CompositeRedirectAutomatic);
	/* TODO: need to understand why we're calling this API*/
	XSelectInput(c->xdisplay, c->win_capture, StructureNotifyMask|ExposureMask|VisibilityChangeMask);
	XSync(c->xdisplay, 0);
	/* Get windows attributes */
	if(!XGetWindowAttributes(c->xdisplay, c->win_capture, &c->win_attr)) {
		av_log(s, AV_LOG_ERROR, "Can't retrieve window attributes!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);	
	}
	/* get GLX FB configs and find the right to use */
	const int 	config_attrs[] = {GLX_BIND_TO_TEXTURE_RGBA_EXT,
				GL_TRUE,
				GLX_DRAWABLE_TYPE,
				GLX_PIXMAP_BIT,
				GLX_BIND_TO_TEXTURE_TARGETS_EXT,
				GLX_TEXTURE_2D_BIT_EXT,
				GLX_DOUBLEBUFFER,
				GL_FALSE,
				None};
	int		nelem = 0;
	GLXFBConfig	*configs = glXChooseFBConfig(c->xdisplay, get_root_window_screen(c->xdisplay, c->win_attr.root), config_attrs, &nelem),
			*cur_cfg = 0;
	for(int i = 0; i < nelem; ++i) {
		XVisualInfo *visual = glXGetVisualFromFBConfig(c->xdisplay, configs[i]);
		if (!visual)
			continue;

		if (c->win_attr.depth != visual->depth) {
			XFree(visual);
			continue;
		}
		XFree(visual);
		cur_cfg = &configs[i];
		break;
	}
	if(!cur_cfg) {
		av_log(s, AV_LOG_ERROR, "Couldn't find a valid FBConfig");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	/* Create the pixmap */
	c->win_pixmap = XCompositeNameWindowPixmap(c->xdisplay, c->win_capture);
	if(!c->win_pixmap) {
		av_log(s, AV_LOG_ERROR, "Can't create Window Pixmap!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	const int pixmap_attrs[] = {GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
				    GLX_TEXTURE_FORMAT_EXT,
				    GLX_TEXTURE_FORMAT_RGBA_EXT, None};
	c->gl_pixmap = glXCreatePixmap(c->xdisplay, *cur_cfg, c->win_pixmap, pixmap_attrs);
	if(!c->gl_pixmap) {
		av_log(s, AV_LOG_ERROR, "Can't create GL Pixmap!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	c->gl_ctx = glXCreateNewContext(c->xdisplay, *cur_cfg, GLX_RGBA_TYPE, 0, 1);
	if(!c->gl_ctx) {
		av_log(s, AV_LOG_ERROR, "Can't create new GLXContext with glXCreateNewContext!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
	/* create gl texture in memory */
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &c->gl_texmap);
	if(glGetError() != GL_NO_ERROR) {
		av_log(s, AV_LOG_ERROR, "Can't init GL texture glGenTextures!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	glBindTexture(GL_TEXTURE_2D, c->gl_texmap);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->win_attr.width, c->win_attr.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	if(glGetError() != GL_NO_ERROR) {
		av_log(s, AV_LOG_ERROR, "Can't init GL texture glTexImage2D!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	/* find ... */
	void (*glXBindTexImageEXT)(Display *, GLXDrawable, int, int *) = (void (*)(Display *, GLXDrawable, int, int *)) glXGetProcAddress((GLubyte*)"glXBindTexImageEXT");
	if(!glXBindTexImageEXT) {
		av_log(s, AV_LOG_ERROR, "Can't find 'glXBindTexImageEXT'");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	glXBindTexImageEXT(c->xdisplay, c->gl_pixmap, GLX_FRONT_LEFT_EXT, NULL);
	if(glGetError() != GL_NO_ERROR) {
		av_log(s, AV_LOG_ERROR, "Can't init glXBindTexImageEXT!");
		xcompgrab_read_close(s);
		return AVERROR(ENOTSUP);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	XFree(configs);
	rv = init_pvt_stream(s);
	if(rv < 0) {
		xcompgrab_read_close(s);
		return rv;
	}
	return 0;
}

static void my_free(void* opaque, uint8_t* data) {
	if(data)
		free(data);
}	

static int xcompgrab_read_packet(AVFormatContext *s, AVPacket *pkt) {
	XCompGrabCtx	*c = s->priv_data;
	int64_t 	pts = 0,
			delay = 0;
	int		length = c->win_attr.width * c->win_attr.height * sizeof(uint8_t) * 4;
	uint8_t		*data = 0;

	/* wait enough time */
	c->time_frame += c->frame_duration;
	while(1) {
		pts = av_gettime();
		delay = c->time_frame - pts;
		if (delay <= 0)
			break;
		av_usleep(delay);
	}
	av_init_packet(pkt);
	data = (uint8_t*)malloc(length);
	pkt->buf = av_buffer_create(data, length, my_free, 0, 0);
    	if (!pkt->buf) {
        	return AVERROR(ENOMEM);
    	}
	pkt->dts = pkt->pts = pts;
	pkt->duration = c->frame_duration;
	pkt->data = data;
	pkt->size = length;

	glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
	glBindTexture(GL_TEXTURE_2D, c->gl_texmap);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

	return 0;
}

static av_cold int xcompgrab_read_close(AVFormatContext *s) {
	XCompGrabCtx	*c = s->priv_data;

	if(c->gl_texmap) {
		glDeleteTextures(1, &c->gl_texmap);
		c->gl_texmap = 0;
	}
	if(c->gl_ctx) {
		glXDestroyContext(c->xdisplay, c->gl_ctx); 
		c->gl_ctx = 0;
	}
	if(c->win_pixmap) {
		XFreePixmap(c->xdisplay, c->win_pixmap);
		c->win_pixmap = 0;
	}
	if(c->xdisplay) {
		XCloseDisplay(c->xdisplay);
		c->xdisplay = 0;
	}
	return 0;
}

AVInputFormat ff_xcompgrab_demuxer = {
	.name           = "xcompgrab",
	.long_name      = "XComposite window capture, using X and OpenGL",
	.priv_data_size = sizeof(XCompGrabCtx),
	.read_header    = xcompgrab_read_header,
	.read_packet    = xcompgrab_read_packet,
	.read_close     = xcompgrab_read_close,
	.flags          = AVFMT_NOFILE,
	.priv_class     = &xcompgrab_class
};

