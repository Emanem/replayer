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
#include <stdatomic.h>

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

/* struct used for buffer allocation */
typedef struct XCompGrabSlice {
	int	used;
	uint8_t	*buf;
} XCompGrabSlice;

typedef struct XCompGrabBuffer {
	int		n_slices;
	XCompGrabSlice	*slices;
} XCompGrabBuffer;

/* struct used for the PBO buffer allocation */
typedef struct XCompGrabPBOSlice {
	GLuint	pbo;
	void*	ptr;
	int	used;
} XCompGrabPBOSlice;

typedef struct XCompGrabPBOBuffer {
	int			n_slices;
	XCompGrabPBOSlice	*slices;
} XCompGrabPBOBuffer;

/* Utility functions to allocate/free memory */
static uint8_t* pvt_alloc(int sz) {
	return (uint8_t*)av_malloc(sz);
}

static void pvt_free(void* opaque, uint8_t* data) {
	if(data)
		av_free(data);
}

static int pvt_init_membuffer(AVFormatContext *s, int n_slices, int n_bytes, XCompGrabBuffer* out) {
	if(n_slices <= 0) {
		av_log(s, AV_LOG_ERROR, "Invalid number of slices for internal memory buffer (%d)\n", n_slices);
		return AVERROR(ENOTSUP);
	}
	// allocate enough space for slices
	out->slices = (XCompGrabSlice*)av_malloc(n_slices*(sizeof(XCompGrabSlice)));
	if(!out->slices) {
		av_log(s, AV_LOG_ERROR, "Can't initialize internal memory buffer\n");
		return AVERROR(ENOMEM);
	}
	out->n_slices = n_slices;
	// initialize those
	for(int i = 0; i < out->n_slices; ++i) {
		out->slices[i].used = 0;
		out->slices[i].buf = (uint8_t*)av_malloc(n_bytes);
		if(!out->slices[i].buf) {
			for(int j = 0; j < i; ++j) {
				av_free(out->slices[j].buf);
			}
			av_free(out->slices);
			return AVERROR(ENOMEM);
		}
	}
	return 0;
}

static void pvt_cleanup_membuffer(XCompGrabBuffer* buf) {
	if(buf->slices) {
		for(int i = 0; i < buf->n_slices; ++i)
			av_free(buf->slices[i].buf);
		av_free(buf->slices);
	}
}

static uint8_t* pvt_alloc_membuffer(XCompGrabBuffer* buf) {
	for(int i = 0; i < buf->n_slices; ++i) {
		/* if we can atomically mark a slice as used, 
		 * we can return its buffer
		 */
		int	exp = 0;
		if(atomic_compare_exchange_strong(&buf->slices[i].used, &exp, 1)) {
			return buf->slices[i].buf;
		}
	}
	return 0;
}

static void pvt_free_membuffer(void* opaque, uint8_t* data) {
	XCompGrabBuffer* buf = (XCompGrabBuffer*)opaque;
	/* first find the slice */
	for(int i = 0; i < buf->n_slices; ++i) {
		if(data == buf->slices[i].buf) {
			/* then reset the buffer to used=0
			 *  this should never fail */
			int	exp = 1;
			if(!atomic_compare_exchange_strong(&buf->slices[i].used, &exp, 0)) {
				/* We should log fatal error */
			}
			return;
		}
	}
}

static int pvt_init_pbobuffer(AVFormatContext *s, int n_slices, XCompGrabPBOBuffer* out) {
	if(n_slices <= 0) {
		av_log(s, AV_LOG_ERROR, "Invalid number of slices for internal memory buffer (%d)\n", n_slices);
		return AVERROR(ENOTSUP);
	}
	out->slices = (XCompGrabPBOSlice*)av_malloc(sizeof(XCompGrabPBOSlice)*n_slices);
	if(!out->slices) {
		av_log(s, AV_LOG_ERROR, "Can't initialize internal memory buffer\n");
		return AVERROR(ENOMEM);
	}
	out->n_slices = n_slices;
	for(int i = 0; i < out->n_slices; ++i) {
		out->slices[i].pbo = 0;
		out->slices[i].ptr = 0;
		out->slices[i].used = 0;
	}
	return 0;
}

static void pvt_cleanup_pbobuffer(XCompGrabPBOBuffer* buf) {
	if(buf->slices)
		av_free(buf->slices);
}

static XCompGrabPBOSlice* pvt_alloc_pbobuffer(XCompGrabPBOBuffer *buf) {
	for(int i = 0; i < buf->n_slices; ++i) {
		XCompGrabPBOSlice	*cur_slice = &buf->slices[i];
		/* if we can atomically mark a slice as used, 
		 * we can return its buffer
		 */
		int	exp = 0;
		if(atomic_compare_exchange_strong(&cur_slice->used, &exp, 1)) {
			return cur_slice;
		}

	}
	return 0;
}

static void pvt_free_pbobuffer(void* opaque, uint8_t* data) {
	XCompGrabPBOSlice*	slice = (XCompGrabPBOSlice*)opaque;
	/* we should crash if data != slice->ptr */
	if(data == slice->ptr) {
		int	exp = 1;
		if(!atomic_compare_exchange_strong(&slice->used, &exp, 0)) {
			/* We should log fatal error */
		}
	}

}

/* Useful typedefs */
typedef void (*f_glXBindTexImageEXT)(Display *, GLXDrawable, int, int *);
typedef void (*f_glXReleaseTexImageEXT)(Display *, GLXDrawable, int);
typedef void (*f_glGenBuffers)(GLsizei, GLuint*);
typedef void (*f_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void (*f_glBindBuffer)(GLenum, GLuint);
typedef void (*f_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void* (*f_glMapBuffer)(GLenum, GLenum);
typedef GLboolean (*f_glUnmapBuffer)(GLenum);

/* this type has to have the first member
 * as a AVClass*, otherwise it will
 * lead to a crash
 */
typedef struct XCompGrabCtx {
	const AVClass 		*class;
	Display			*xdisplay;
	Window			win_capture;
	Pixmap			win_pixmap;
	XWindowAttributes	win_attr;
	GLXContext		gl_ctx;
	GLXPixmap		gl_pixmap;
	GLuint			gl_texmap;
	const char 		*framerate;
	const char		*window_name;
	int			framebuf_type;
	int64_t			time_frame;
	AVRational		time_base;
	int64_t			frame_duration;
	f_glXBindTexImageEXT	glXBindTexImageEXT;
	f_glXReleaseTexImageEXT	glXReleaseTexImageEXT;
	f_glGenBuffers		glGenBuffers;
	f_glDeleteBuffers	glDeleteBuffers;
	f_glBindBuffer		glBindBuffer;
	f_glBufferData		glBufferData;
	f_glMapBuffer		glMapBuffer;
	f_glUnmapBuffer		glUnmapBuffer;
	XCompGrabBuffer		pvt_framebuf;
	XCompGrabPBOBuffer	glpbo_framebuf;
} XCompGrabCtx;

#define OFFSET(x) offsetof(XCompGrabCtx, x)
#define D AV_OPT_FLAG_DECODING_PARAM

#define BUF_SYSTEM	(0)
#define BUF_INTERNAL	(1)
#define BUF_GLPBO	(2)

static const AVOption options[] = {
	{ "framerate", "", OFFSET(framerate), AV_OPT_TYPE_STRING, {.str = "ntsc" }, 0, 0, D },
	{ "window_name", "X window name/title", OFFSET(window_name), AV_OPT_TYPE_STRING, {.str = "Desktop" }, 0, 0, D },
	{ "framebuf_type", "0 to use system memory (slow), 1 for internal buffers, 2 for GL PBO managed buffers", OFFSET(framebuf_type), AV_OPT_TYPE_INT, { .i64 = BUF_INTERNAL }, BUF_SYSTEM, BUF_GLPBO, D },
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

static int pvt_check_comp_support(AVFormatContext *s, XCompGrabCtx *c) {
	int	eventBase,
		errorBase,
		major = 0,
		minor = 2;

	if (!XCompositeQueryExtension(c->xdisplay, &eventBase, &errorBase)) {
		av_log(s, AV_LOG_ERROR, "XComposite extension not supported\n");
		return AVERROR(ENOTSUP);
	}
	XCompositeQueryVersion(c->xdisplay, &major, &minor);
	if (major == 0 && minor < 2) {
		av_log(s, AV_LOG_ERROR, "XComposite extension is too old: %d.%d < 0.2\n", major, minor);
		return AVERROR(ENOTSUP);
	}
	return 0;
}

static int pvt_find_window(Display *xdisplay, const char *target, Window* out) {
	Window		rootWindow = RootWindow(xdisplay, DefaultScreen(xdisplay));
	Atom		atom = XInternAtom(xdisplay, "_NET_CLIENT_LIST", 1);
	Atom		actualType;
	int		format;
	unsigned long	numItems,
			bytesAfter;
	unsigned char	*data = '\0';
	Window 		*list;    
	char		*win_name = 0;
	int		status = XGetWindowProperty(xdisplay, rootWindow, atom, 0L, (~0L), 0,
				AnyPropertyType, &actualType, &format, &numItems, &bytesAfter, &data);
	list = (Window *)data;
	if (status >= Success && numItems) {
		for (int i = 0; i < numItems; ++i) {
			status = XFetchName(xdisplay, list[i], &win_name);
			if (status >= Success && win_name) {
				if (strstr(win_name, target)) {
					XFree(win_name);
					XFree(data);
					*out = list[i];
					return 0;
				} else XFree(win_name);
			}
		}
	}
	XFree(data);
	return -1;
}

static int pvt_init_stream(AVFormatContext *s) {
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

static int pvt_check_gl_error(AVFormatContext *s, const char* desc) {
	switch(glGetError()) {
		case GL_NO_ERROR:
			return 0;
		case GL_INVALID_ENUM:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_INVALID_ENUM!\n", desc);
			return -1;
		case GL_INVALID_VALUE:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_INVALID_VALUE!\n", desc);
			return -1;
		case GL_INVALID_OPERATION:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_INVALID_OPERATION!\n", desc);
			return -1;
		case GL_STACK_OVERFLOW:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_STACK_OVERFLOW!\n", desc);
			return -1;
		case GL_STACK_UNDERFLOW:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_STACK_UNDERFLOW!\n", desc);
			return -1;
		case GL_OUT_OF_MEMORY:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_OUT_OF_MEMORY!\n", desc);
			return -1;
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_INVALID_FRAMEBUFFER_OPERATION!\n", desc);
			return -1;
		case GL_CONTEXT_LOST:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_CONTEXT_LOST!\n", desc);
			return -1;
		case GL_TABLE_TOO_LARGE:
			av_log(s, AV_LOG_ERROR, "GL error: %s GL_TABLE_TOO_LARGE!\n", desc);
			return -1;
		default:
			break;
	}
	return -2;
}

static int pvt_init_gl_func(AVFormatContext *s, XCompGrabCtx *c) {
	if(!(c->glXBindTexImageEXT = (f_glXBindTexImageEXT) glXGetProcAddress((GLubyte*)"glXBindTexImageEXT"))) {
		av_log(s, AV_LOG_ERROR, "Can't lookup 'glXBindTexImageEXT'\n");
		return AVERROR(ENOTSUP);
	}
	if(!(c->glXReleaseTexImageEXT = (f_glXReleaseTexImageEXT) glXGetProcAddress((GLubyte*)"glXReleaseTexImageEXT"))) {
		av_log(s, AV_LOG_ERROR, "Can't lookup 'glXReleaseTexImageEXT'\n");
		return AVERROR(ENOTSUP);
	}
	if(c->framebuf_type == BUF_GLPBO) {
		if(!(c->glGenBuffers = (f_glGenBuffers) glXGetProcAddress((GLubyte*)"glGenBuffers"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glGenBuffers'\n");
			return AVERROR(ENOTSUP);
		}
		if(!(c->glDeleteBuffers = (f_glDeleteBuffers) glXGetProcAddress((GLubyte*)"glDeleteBuffers"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glDeleteBuffers'\n");
			return AVERROR(ENOTSUP);
		}
		if(!(c->glBindBuffer = (f_glBindBuffer) glXGetProcAddress((GLubyte*)"glBindBuffer"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glBindBuffer'\n");
			return AVERROR(ENOTSUP);
		}
		if(!(c->glBufferData = (f_glBufferData) glXGetProcAddress((GLubyte*)"glBufferData"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glBufferData'\n");
			return AVERROR(ENOTSUP);
		}
		if(!(c->glMapBuffer = (f_glMapBuffer) glXGetProcAddress((GLubyte*)"glMapBuffer"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glMapBuffer'\n");
			return AVERROR(ENOTSUP);
		}
		if(!(c->glUnmapBuffer = (f_glUnmapBuffer) glXGetProcAddress((GLubyte*)"glUnmapBuffer"))) {
			av_log(s, AV_LOG_ERROR, "Can't lookup 'glUnmapBuffer'\n");
			return AVERROR(ENOTSUP);
		}
	}
	return 0;
}

/* Functions and static variables to help out with error
 * handling of X errors.
 * A good reading is https://www.remlab.net/op/xlib.shtml
 */

static int 		is_x_error = 0;
static char		x_error_buf[512];

static int pvt_x_error_handler(Display *d, XErrorEvent* e) {
	is_x_error = 1;
	char	err_desc[128];
	XGetErrorText(d, e->error_code, err_desc, 128);
	err_desc[127] = '\0';
	snprintf(x_error_buf, 511, "X error %d [%d, %d] : %s", e->error_code, e->request_code, e->minor_code, err_desc);
	return 0;
}

static int pvt_check_x_error(AVFormatContext *s, Display *d) {
	XSync(d, 0);
	if(is_x_error) {
		av_log(s, AV_LOG_ERROR, "%s\n", x_error_buf);
		is_x_error = 0;
		return -1;
	}
	return 0;
}

static av_cold int xcompgrab_read_header(AVFormatContext *s) {
	int		rv = 0;
	GLXFBConfig	*configs = 0,
			*cur_cfg = 0;
	XCompGrabCtx	*c = s->priv_data;
	XErrorHandler	prev_x_error_handler = 0;

	/* reset data members used for destruction */
	c->xdisplay = 0;
	c->win_pixmap = 0;
	c->gl_ctx = 0;
	c->gl_texmap = 0;

	c->xdisplay = XOpenDisplay(NULL);
	if(!c->xdisplay)
		return AVERROR(ENODEV);
	prev_x_error_handler = XSetErrorHandler(pvt_x_error_handler);
	/* we should lock the display here */
	/* check composite extension is supported */
	if((rv = pvt_check_comp_support(s, c)) < 0) {
		goto err_exit;
	}
	/* find the window name */
	if(pvt_find_window(c->xdisplay, c->window_name, &c->win_capture) < 0) {
		av_log(s, AV_LOG_ERROR, "Can't find X window containing string '%s'\n", c->window_name);
		rv = AVERROR(EINVAL);
		goto err_exit;
	}
	XCompositeRedirectWindow(c->xdisplay, c->win_capture, CompositeRedirectAutomatic);
	if(pvt_check_x_error(s, c->xdisplay) < 0) {
		rv = AVERROR(EINVAL);
		goto err_exit;
	}
	/* Get windows attributes */
	if(!XGetWindowAttributes(c->xdisplay, c->win_capture, &c->win_attr)) {
		av_log(s, AV_LOG_ERROR, "Can't retrieve window attributes!\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	av_log(s, AV_LOG_INFO, "Captuing window id %ld, resolution %dx%d\n", c->win_capture, c->win_attr.width, c->win_attr.height);
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
	configs = glXChooseFBConfig(c->xdisplay, get_root_window_screen(c->xdisplay, c->win_attr.root), config_attrs, &nelem);
	if(!configs) {
		av_log(s, AV_LOG_ERROR, "glXChooseFBConfig failed\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
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
		av_log(s, AV_LOG_ERROR, "Couldn't find a valid FBConfig\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	/* Create the pixmap */
	c->win_pixmap = XCompositeNameWindowPixmap(c->xdisplay, c->win_capture);
	if(!c->win_pixmap || (pvt_check_x_error(s, c->xdisplay) < 0)) {
		av_log(s, AV_LOG_ERROR, "Can't create Window Pixmap!\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	const int pixmap_attrs[] = {GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
				    GLX_TEXTURE_FORMAT_EXT,
				    GLX_TEXTURE_FORMAT_RGBA_EXT, None};
	c->gl_pixmap = glXCreatePixmap(c->xdisplay, *cur_cfg, c->win_pixmap, pixmap_attrs);
	if(!c->gl_pixmap || (pvt_check_x_error(s, c->xdisplay) < 0)) {
		av_log(s, AV_LOG_ERROR, "Can't create GL Pixmap!\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	c->gl_ctx = glXCreateNewContext(c->xdisplay, *cur_cfg, GLX_RGBA_TYPE, 0, 1);
	if(!c->gl_ctx) {
		av_log(s, AV_LOG_ERROR, "Can't create new GLXContext with glXCreateNewContext!\n");
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	XFree(configs);
	configs = 0;
	glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
	if(pvt_check_x_error(s, c->xdisplay) < 0) {
		rv = AVERROR(ENOTSUP);
		goto err_exit;
	}
	/* At this stage all X commands should have
	 * been done, remove the error callback
	 */
	XSetErrorHandler(prev_x_error_handler);
	prev_x_error_handler = 0;
	/* create gl texture in memory */
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &c->gl_texmap);
	if(pvt_check_gl_error(s, "glGenTextures") < 0) {
		rv = AVERROR(EINVAL);
		goto err_exit;
	}
	glBindTexture(GL_TEXTURE_2D, c->gl_texmap);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->win_attr.width, c->win_attr.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	if(pvt_check_gl_error(s, "glTexImage2D") < 0) {
		rv = AVERROR(EINVAL);
		goto err_exit;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	/* init all nonstandard gl func */
	if((rv = pvt_init_gl_func(s, c)) < 0) {
		goto err_exit;
	}
	/* take care of different buffer types */
	switch(c->framebuf_type) {
	case BUF_INTERNAL:
		av_log(s, AV_LOG_INFO, "Using internal framebuffers\n");
		if((rv = pvt_init_membuffer(s, 8, c->win_attr.width*c->win_attr.height*4, &c->pvt_framebuf)) < 0) {
			goto err_exit;
		}
		break;
	case BUF_GLPBO:
		av_log(s, AV_LOG_INFO, "Using GL Pixel Buffer Object to manage framebuffers\n");
		if((rv = pvt_init_pbobuffer(s, 8, &c->glpbo_framebuf)) < 0) {
			goto err_exit;
		}
		/* init each single PBO */
		for(int i = 0; i < c->glpbo_framebuf.n_slices; ++i) {
			GLuint	*cur_pbo = &c->glpbo_framebuf.slices[i].pbo;
			c->glGenBuffers(1, cur_pbo);
			if(pvt_check_gl_error(s, "glGenBuffers") < 0) {
				rv = AVERROR(EINVAL);
				goto err_exit;
			}
			c->glBindBuffer(GL_PIXEL_PACK_BUFFER, *cur_pbo);
			c->glBufferData(GL_PIXEL_PACK_BUFFER, c->win_attr.width*c->win_attr.height* 4, NULL, GL_STREAM_READ);
			if(pvt_check_gl_error(s, "glBufferData") < 0) {
				rv = AVERROR(EINVAL);
				goto err_exit;
			}
		}
		break;
	case BUF_SYSTEM:
	default:
		av_log(s, AV_LOG_INFO, "Using system memory for framebuffers\n");
		break;
	}
	/* init public stream info */
	if((rv = pvt_init_stream(s)) < 0) {
		goto err_exit;
	}
	return 0;

err_exit:
	if(configs)
		XFree(configs);
	if(is_x_error)
		is_x_error = 0;
	if(prev_x_error_handler)
		XSetErrorHandler(prev_x_error_handler);
	xcompgrab_read_close(s);
	return rv;
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
	/* properly setup memory structures
	 * to allocate buffer from desired
	 * pool (malloc/pvt_membuffer)
	 */
	pkt->buf = 0;
	/* init memory based on framebuffer type */
	if(c->framebuf_type != BUF_GLPBO) {
		if(c->framebuf_type == BUF_SYSTEM) {
			data = pvt_alloc(length);
			if (data) pkt->buf = av_buffer_create(data, length, pvt_free, 0, 0);
		} else {
			data = pvt_alloc_membuffer(&c->pvt_framebuf);
			if (data) pkt->buf = av_buffer_create(data, length, pvt_free_membuffer, &c->pvt_framebuf, 0);
			else av_log(s, AV_LOG_WARNING, "Warning: consumer is too slow in processing AVPacket from av_read_frame (or equivalent call)\n");
		}
		if (!pkt->buf) {
			return AVERROR(ENOMEM);
		}
	}
	pkt->dts = pkt->pts = pts;
	pkt->duration = c->frame_duration;
	pkt->data = data;
	pkt->size = length;
	/* gl calls to capture the composite window */
	glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
	glBindTexture(GL_TEXTURE_2D, c->gl_texmap);
	c->glXBindTexImageEXT(c->xdisplay, c->gl_pixmap, GLX_FRONT_LEFT_EXT, NULL);
	if(c->framebuf_type == BUF_GLPBO) {
		XCompGrabPBOSlice	*slice = pvt_alloc_pbobuffer(&c->glpbo_framebuf);
		if(!slice) {
			av_log(s, AV_LOG_WARNING, "Warning: consumer is too slow in processing AVPacket from av_read_frame (or equivalent call)\n");
			return AVERROR(ENOMEM);
		}
		c->glBindBuffer(GL_PIXEL_PACK_BUFFER, slice->pbo);
		/* if we had data, unmap and set the pointer to 0 */
		if(slice->ptr) {
			c->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			slice->ptr = 0;
		}
		/* with PBOs, the below call is asynchronous
		 * and the buffer indicates an offset - which is 0
		 */
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		/* this call is synchrounous */
		slice->ptr = c->glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if(!slice->ptr) {
			av_log(s, AV_LOG_FATAL, "Fatal: the GL driver couldn't DMA the buffer using PBO\n");
			return AVERROR(ENOMEM);
		}
		/* Then, we initialize the packet */
		pkt->buf = av_buffer_create(slice->ptr, length, pvt_free_pbobuffer, slice, 0);
		pkt->data = slice->ptr;
	} else {
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	}
	c->glXReleaseTexImageEXT(c->xdisplay, c->gl_pixmap, GLX_FRONT_LEFT_EXT);
	return 0;
}

static av_cold int xcompgrab_read_close(AVFormatContext *s) {
	XCompGrabCtx	*c = s->priv_data;

	/* clear respective buffer type */
	switch(c->framebuf_type) {
	case BUF_INTERNAL:
		pvt_cleanup_membuffer(&c->pvt_framebuf);
		break;
	case BUF_GLPBO:
		if(c->xdisplay && c->gl_pixmap && c->gl_ctx) {
			glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
			c->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			for(int i = 0; i < c->glpbo_framebuf.n_slices; ++i)
				if(c->glpbo_framebuf.slices[i].pbo)
					c->glDeleteBuffers(1, &c->glpbo_framebuf.slices[i].pbo);
		}
		pvt_cleanup_pbobuffer(&c->glpbo_framebuf);
		break;
	case BUF_SYSTEM:
	default:
		break;

	}
	if(c->gl_texmap && c->xdisplay && c->gl_pixmap && c->gl_ctx) {
		glXMakeCurrent(c->xdisplay, c->gl_pixmap, c->gl_ctx);
		glDeleteTextures(1, &c->gl_texmap);
		c->gl_texmap = 0;
	}
	if(c->gl_ctx && c->xdisplay) {
		glXMakeCurrent(c->xdisplay, None, 0);
		glXDestroyContext(c->xdisplay, c->gl_ctx); 
		c->gl_ctx = 0;
	}
	if(c->win_pixmap && c->xdisplay) {
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

