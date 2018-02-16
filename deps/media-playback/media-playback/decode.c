/*
 * Copyright (c) 2017 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "decode.h"
#include "media.h"
#include <obs-ui.h>

static AVCodec *find_hardware_decoder(enum AVCodecID id)
{
	AVHWAccel *hwa = av_hwaccel_next(NULL);
	AVCodec *c = NULL;

  while (hwa) {
		if (hwa->id == id) {
			if ( (get_opt_cuda_decoding() && hwa->pix_fmt == AV_PIX_FMT_CUDA) ||
          hwa->pix_fmt == AV_PIX_FMT_VDA_VLD ||
			    hwa->pix_fmt == AV_PIX_FMT_DXVA2_VLD ||
			    hwa->pix_fmt == AV_PIX_FMT_VAAPI_VLD) {
				c = avcodec_find_decoder_by_name(hwa->name);
				if (c)
					break;
			}
		}

		hwa = av_hwaccel_next(hwa);
	}

  if (c != NULL)
    printf("hw decoding: codec id %d found %s!\n", id, c->name);
  else 
    printf("hw decoding: codec id %d NOT found!\n", id);

	return c;
}

static int mp_open_codec(struct mp_decode *d)
{
	AVCodecContext *c;
	int ret;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	c = avcodec_alloc_context3(d->codec);
	if (!c) {
		blog(LOG_WARNING, "MP: Failed to allocate context");
		return -1;
	}

	ret = avcodec_parameters_to_context(c, d->stream->codecpar);
	if (ret < 0)
		goto fail;
#else
	c = d->stream->codec;
#endif

	if (c->thread_count == 1 &&
	    c->codec_id != AV_CODEC_ID_PNG &&
	    c->codec_id != AV_CODEC_ID_TIFF &&
	    c->codec_id != AV_CODEC_ID_JPEG2000 &&
	    c->codec_id != AV_CODEC_ID_MPEG4 &&
	    c->codec_id != AV_CODEC_ID_WEBP)
		c->thread_count = 0;


	//c->pix_fmt = AV_PIX_FMT_NV12;
	//c->pix_fmt = AV_PIX_FMT_NV20;
	//c->pix_fmt = AV_PIX_FMT_RGB24;
	//c->pix_fmt = AV_PIX_FMT_YUV444P;
	//c->codec_id = AV_CODEC_ID_RAWVIDEO;

	//c->codec_type = 1;
	//c->time_base.num = 1;
	//c->time_base.den = 25;
	//c->pix_fmt = AV_PIX_FMT_YUV420P;
	//c->width = 275;
	//c->height = 114;

	printf("codec: %s, max_b_frames: %d, pix_fmt: %d, width: %d, height: %d, gop_size: %d, bit_rate: %d, time_base.num: %d, time_base.den: %d, codec_type: %d\n", d->codec->name, c->max_b_frames, c->pix_fmt, c->width, c->height, c->gop_size, c->bit_rate, c->time_base.num, c->time_base.den, c->codec_type);

	AVDictionary * av_dict_opts = NULL;


	//av_dict_set( &av_dict_opts, "hwaccel_device", "0", 0);
	//av_dict_set( &av_dict_opts, "hwaccel", "cuvid", 0);
	//av_dict_set( &av_dict_opts, "gpu", "0", 0);

	//av_dict_set( &av_dict_opts, "deint", "2", 0);
	//av_dict_set( &av_dict_opts, "drop_second_field", "0", 0);
	//av_dict_set( &av_dict_opts, "surfaces", "8", 0 );
	//av_dict_set( &av_dict_opts, "preset", "ultrafast", 0 );
	//av_dict_set( &av_dict_opts, "tune", "zerolatency", 0 );
	//av_dict_set( &av_dict_opts, "delay", "50", 0 );

	ret = avcodec_open2(c, d->codec, NULL); //&av_dict_opts);
	if (ret < 0)
		goto fail;

	d->decoder = c;
	return ret;

fail:
	avcodec_close(c);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	av_free(d->decoder);
#endif
	return ret;
}

int init_filters(mp_media_t *m, const char *filters_descr, AVStream *s);

bool mp_decode_init(mp_media_t *m, enum AVMediaType type, bool hw)
{
	struct mp_decode *d = type == AVMEDIA_TYPE_VIDEO ? &m->v : &m->a;
	enum AVCodecID id;
	AVStream *stream;
	int ret;

	memset(d, 0, sizeof(*d));
	d->m = m;
	d->audio = type == AVMEDIA_TYPE_AUDIO;

	// disable audio
	if (type == AVMEDIA_TYPE_AUDIO)
		return false;

	// enable in case of rawvideo
	//AVCodec *my = avcodec_find_decoder_by_name("rawvideo");
	//ret = av_find_best_stream(m->fmt, type, 0, -1, &my, 0);
	
	ret = av_find_best_stream(m->fmt, type, -1, -1, NULL, 0);
	printf("find_best_stream: %d\n", ret);
	if (ret < 0)
		return false;
	stream = d->stream = m->fmt->streams[ret];

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	id = stream->codecpar->codec_id;
#else
	id = stream->codec->codec_id;
#endif

	if (hw) {
		d->codec = find_hardware_decoder(id);
		if (d->codec) {
			ret = mp_open_codec(d);
			if (ret < 0)
				d->codec = NULL;
		}
	}

	if (!d->codec) {
		if (id == AV_CODEC_ID_VP8)
			d->codec = avcodec_find_decoder_by_name("libvpx");
		else if (id == AV_CODEC_ID_VP9)
			d->codec = avcodec_find_decoder_by_name("libvpx-vp9");
		else if (id == AV_CODEC_ID_RAWVIDEO)
			d->codec = avcodec_find_decoder_by_name("rawvideo");

		if (!d->codec)
			d->codec = avcodec_find_decoder(id);
		if (!d->codec) {
			blog(LOG_WARNING, "MP: Failed to find %s codec",
					av_get_media_type_string(type));
			return false;
		}

		ret = mp_open_codec(d);
		if (ret < 0) {
			blog(LOG_WARNING, "MP: Failed to open %s decoder: %s",
					av_get_media_type_string(type),
					av_err2str(ret));
			return false;
		}
	}

	d->frame = av_frame_alloc();
	if (!d->frame) {
		blog(LOG_WARNING, "MP: Failed to allocate %s frame",
				av_get_media_type_string(type));
		return false;
	}

	if (d->codec->capabilities & CODEC_CAP_TRUNCATED)
		d->decoder->flags |= CODEC_FLAG_TRUNCATED;

  if (get_opt_filter() != NULL && type == AVMEDIA_TYPE_VIDEO) {
    const char *filters_descr = get_opt_filter();
    printf("init_filter %s, source: %s\n", filters_descr, m->path);
    init_filters(m, filters_descr, stream);
  }

	return true;
}

int init_filters(mp_media_t *m, const char *filters_descr, AVStream *s)
{

  struct mp_decode *d = &m->v;
  AVCodecContext *dec_ctx = d->decoder;
  avfilter_register_all();

  char args[512];
  int ret;
  AVFilter *buffersrc  = avfilter_get_by_name("buffer");
  AVFilter *buffersink = avfilter_get_by_name("buffersink");
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE, AV_PIX_FMT_CUDA, AV_PIX_FMT_NV12 };
  AVBufferSinkParams *buffersink_params;
  d->filter_graph = avfilter_graph_alloc();
    
  snprintf(args, sizeof(args), "%d:%d:%d:%d:%d:%d:%d",
              dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
              s->time_base.num, s->time_base.den,
              s->sample_aspect_ratio.num, s->sample_aspect_ratio.den);

  ret = avfilter_graph_create_filter(&d->buffersrc_ctx, buffersrc, "in",
                                     args, NULL, d->filter_graph);
  if (ret < 0) {
      printf("Cannot create buffer source %d, args=\"%s\"\n", ret, args);
      return ret;
  }


  /* buffer video sink: to terminate the filter chain. */
  buffersink_params = av_buffersink_params_alloc();
  buffersink_params->pixel_fmts = pix_fmts;
  ret = avfilter_graph_create_filter(&d->buffersink_ctx, buffersink, "out",
                                     NULL, buffersink_params, d->filter_graph);
  av_free(buffersink_params);
  if (ret < 0) {
      printf("Cannot create buffer sink\n");
      return ret;
  }
  /* Endpoints for the filter graph. */
  outputs->name       = av_strdup("in");
  outputs->filter_ctx = d->buffersrc_ctx;
  outputs->pad_idx    = 0;
  outputs->next       = NULL;

  inputs->name       = av_strdup("out");
  inputs->filter_ctx = d->buffersink_ctx;
  inputs->pad_idx    = 0;
  inputs->next       = NULL;

  if ((ret = avfilter_graph_parse_ptr(d->filter_graph, filters_descr,
                                  &inputs, &outputs, NULL)) < 0)
      return ret;
  
  if ((ret = avfilter_graph_config(d->filter_graph, NULL)) < 0)
      return ret;
  return 0;
}

void mp_decode_clear_packets(struct mp_decode *d)
{
	if (d->packet_pending) {
		av_packet_unref(&d->orig_pkt);
		d->packet_pending = false;
	}

	while (d->packets.size) {
		AVPacket pkt;
		circlebuf_pop_front(&d->packets, &pkt, sizeof(pkt));
		av_packet_unref(&pkt);
	}
}

void mp_decode_free(struct mp_decode *d)
{
	mp_decode_clear_packets(d);
	circlebuf_free(&d->packets);

	if (d->decoder) {
		avcodec_close(d->decoder);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
		av_free(d->decoder);
#endif
	}

	if (d->frame)
		av_free(d->frame);

	memset(d, 0, sizeof(*d));
}

void mp_decode_push_packet(struct mp_decode *decode, AVPacket *packet)
{
	circlebuf_push_back(&decode->packets, packet, sizeof(*packet));
}

static inline int64_t get_estimated_duration(struct mp_decode *d,
		int64_t last_pts)
{
	if (last_pts)
		return d->frame_pts - last_pts;

	if (d->audio) {
		return av_rescale_q(d->frame->nb_samples,
				(AVRational){1, d->frame->sample_rate},
				(AVRational){1, 1000000000});
	} else {
		if (d->last_duration)
			return d->last_duration;

		return av_rescale_q(d->decoder->time_base.num,
				d->decoder->time_base,
				(AVRational){1, 1000000000});
	}
}

static int decode_packet(struct mp_decode *d, int *got_frame)
{
	int ret;
	*got_frame = 0;

#ifdef USE_NEW_FFMPEG_DECODE_API
	ret = avcodec_receive_frame(d->decoder, d->frame);
	if (ret != 0 && ret != AVERROR(EAGAIN)) {
		if (ret == AVERROR_EOF)
			ret = 0;
		return ret;
	}

	if (ret != 0) {
		ret = avcodec_send_packet(d->decoder, &d->pkt);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		ret = avcodec_receive_frame(d->decoder, d->frame);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		*got_frame = (ret == 0);
		ret = d->pkt.size;
	} else {
		ret = 0;
		*got_frame = 1;
	}

#else
	if (d->audio) {
		ret = avcodec_decode_audio4(d->decoder,
				d->frame, got_frame, &d->pkt);
	} else {
		ret = avcodec_decode_video2(d->decoder,
				d->frame, got_frame, &d->pkt);
	}
#endif
	return ret;
}

bool mp_decode_next(struct mp_decode *d)
{
	bool eof = d->m->eof;
	int got_frame;
	int ret;

	d->frame_ready = false;

	if (!eof && !d->packets.size)
		return true;

	while (!d->frame_ready) {
		if (!d->packet_pending) {
			if (!d->packets.size) {
				if (eof) {
					d->pkt.data = NULL;
					d->pkt.size = 0;
				} else {
					return true;
				}
			} else {
				circlebuf_pop_front(&d->packets, &d->orig_pkt,
						sizeof(d->orig_pkt));
				d->pkt = d->orig_pkt;
				d->packet_pending = true;
			}
		}

		ret = decode_packet(d, &got_frame);

		if (!got_frame && ret == 0) {
			d->eof = true;
			return true;
		}
		if (ret < 0) {
#ifdef DETAILED_DEBUG_INFO
			blog(LOG_DEBUG, "MP: decode failed: %s",
					av_err2str(ret));
#endif

			if (d->packet_pending) {
				av_packet_unref(&d->orig_pkt);
				av_init_packet(&d->orig_pkt);
				av_init_packet(&d->pkt);
				d->packet_pending = false;
			}
			return true;
		}

		d->frame_ready = !!got_frame;

		if (d->packet_pending) {
			if (d->pkt.size) {
				d->pkt.data += ret;
				d->pkt.size -= ret;
			}

			if (d->pkt.size <= 0) {
				av_packet_unref(&d->orig_pkt);
				av_init_packet(&d->orig_pkt);
				av_init_packet(&d->pkt);
				d->packet_pending = false;
			}
		}
	}

	if (d->frame_ready) {
		int64_t last_pts = d->frame_pts;

		if (d->frame->best_effort_timestamp == AV_NOPTS_VALUE)
			d->frame_pts = d->next_pts;
		else
			d->frame_pts = av_rescale_q(
					d->frame->best_effort_timestamp,
					d->stream->time_base,
					(AVRational){1, 1000000000});

		int64_t duration = d->frame->pkt_duration;
		if (!duration)
			duration = get_estimated_duration(d, last_pts);
		else
			duration = av_rescale_q(duration,
					d->stream->time_base,
					(AVRational){1, 1000000000});

		d->last_duration = duration;
		d->next_pts = d->frame_pts + duration;

    // filter
    if (get_opt_filter() != NULL && d->buffersrc_ctx) {

      int r;
      d->frame->pts = av_frame_get_best_effort_timestamp(d->frame);

      /* push the decoded frame into the filtergraph */
      if ((r=av_buffersrc_add_frame_flags(d->buffersrc_ctx, d->frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
          av_log(NULL, AV_LOG_ERROR, "Error %d while feeding the filtergraph\n", r);
          return ret;
      } 
       
      AVFrame *frame = d->frame,
        *filt_frame = av_frame_alloc();

      r = av_buffersink_get_frame(d->buffersink_ctx, filt_frame);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
        av_frame_unref(filt_frame);
        av_frame_free(&filt_frame);
        return ret;
      }
      if (filt_frame) 
        d->frame = filt_frame;

      av_frame_unref(frame);      
      av_frame_free(&frame);
    }
    // filter end
    
	}

	return true;
}

void mp_decode_flush(struct mp_decode *d)
{
	avcodec_flush_buffers(d->decoder);
	mp_decode_clear_packets(d);
	d->eof = false;
	d->frame_pts = 0;
	d->frame_ready = false;
}
