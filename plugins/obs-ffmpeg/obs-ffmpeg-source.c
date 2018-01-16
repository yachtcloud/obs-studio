/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
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


#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <ctype.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h> 
#include <signal.h>


#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"
#include "obs-scene.h"
#include "obs-ui.h"


#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <media-playback/media.h>

#define FF_LOG(level, format, ...) \
	blog(level, "[Media Source]: " format, ##__VA_ARGS__)
#define FF_LOG_S(source, level, format, ...) \
	blog(level, "[Media Source '%s']: " format, \
			obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

static bool video_frame(struct ff_frame *frame, void *opaque);
static bool video_format(AVCodecContext *codec_context, void *opaque);

struct ffmpeg_source {
	mp_media_t media;
	bool media_valid;
	bool destroy_media;

  // rescue
  double source_frames;
  unsigned long last_frame_at;
  bool restarted;

  // preprocess
  int restart_status;
  pthread_t *tid;
  char **rescale;
  char **codecs;
  char *scene_name;
  char *ffinput;
  char *ffoutput;
  int i;

	struct SwsContext *sws_ctx;
	int sws_width;
	int sws_height;
	enum AVPixelFormat sws_format;
	uint8_t *sws_data;
	int sws_linesize;
	enum video_range_type range;
	obs_source_t *source;
	obs_hotkey_id hotkey;

	char *input;
	char *input_format;
	int buffering_mb;
	bool is_looping;
	bool is_local_file;
	bool is_hw_decoding;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool seekable;
};


void sleep_ms(int milliseconds) {
	struct timespec ts;
	ts.tv_sec = milliseconds / 1000;	
	ts.tv_nsec = (milliseconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}




static bool is_local_file_modified(obs_properties_t *props,
		obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *input = obs_properties_get(props, "input");
	obs_property_t *input_format =obs_properties_get(props,
			"input_format");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_t *looping = obs_properties_get(props, "looping");
	obs_property_t *buffering = obs_properties_get(props, "buffering_mb");
	obs_property_t *close = obs_properties_get(props, "close_when_inactive");
	obs_property_t *seekable = obs_properties_get(props, "seekable");
	obs_property_set_visible(input, !enabled);
	obs_property_set_visible(input_format, !enabled);
	obs_property_set_visible(buffering, !enabled);
	obs_property_set_visible(close, enabled);
	obs_property_set_visible(local_file, enabled);
	obs_property_set_visible(looping, enabled);
	obs_property_set_visible(seekable, !enabled);

	return true;
}

static void ffmpeg_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "is_local_file", true);
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
#if defined(_WIN32)
	obs_data_set_default_bool(settings, "hw_decode", true);
#endif
	obs_data_set_default_int(settings, "buffering_mb", 2);
}

static const char *media_filter =
	" (*.mp4 *.ts *.mov *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter =
	" (*.mp4 *.ts *.mov *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter =
	" (*.mp3 *.aac *.ogg *.wav);;";

static obs_properties_t *ffmpeg_source_getproperties(void *data)
{
	struct ffmpeg_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop;
	// use this when obs allows non-readonly paths
	prop = obs_properties_add_bool(props, "is_local_file",
			obs_module_text("LocalFile"));

	obs_property_set_modified_callback(prop, is_local_file_modified);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	if (s && s->input && *s->input) {
		const char *slash;

		dstr_copy(&path, s->input);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "local_file",
			obs_module_text("LocalFile"), OBS_PATH_FILE,
			filter.array, path.array);
	dstr_free(&filter);
	dstr_free(&path);

	prop = obs_properties_add_bool(props, "looping",
			obs_module_text("Looping"));

	obs_properties_add_bool(props, "restart_on_activate",
			obs_module_text("RestartWhenActivated"));

	obs_properties_add_text(props, "input",
			obs_module_text("Input"), OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format",
			obs_module_text("InputFormat"), OBS_TEXT_DEFAULT);

#ifndef __APPLE__
	obs_properties_add_bool(props, "hw_decode",
			obs_module_text("HardwareDecode"));
#endif

	obs_properties_add_bool(props, "clear_on_media_end",
			obs_module_text("ClearOnMediaEnd"));

	prop = obs_properties_add_bool(props, "close_when_inactive",
			obs_module_text("CloseFileWhenInactive"));

	obs_property_set_long_description(prop,
			obs_module_text("CloseFileWhenInactive.ToolTip"));

	prop = obs_properties_add_list(props, "color_range",
			obs_module_text("ColorRange"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Auto"),
			VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Partial"),
			VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Full"),
			VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "seekable", obs_module_text("Seekable"));

	return props;
}

static void dump_source_info(struct ffmpeg_source *s, const char *input,
		const char *input_format)
{
	FF_BLOG(LOG_INFO,
			"settings:\n"
			"\tinput:                   %s\n"
			"\tinput_format:            %s\n"
			"\tis_looping:              %s\n"
			"\tis_hw_decoding:          %s\n"
			"\tis_clear_on_media_end:   %s\n"
			"\trestart_on_activate:     %s\n"
			"\tclose_when_inactive:     %s",
			input ? input : "(null)",
			input_format ? input_format : "(null)",
			s->is_looping ? "yes" : "no",
			s->is_hw_decoding ? "yes" : "no",
			s->is_clear_on_media_end ? "yes" : "no",
			s->restart_on_activate ? "yes" : "no",
			s->close_when_inactive ? "yes" : "no");
}

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	obs_source_output_video(s->source, f);
}

static void preload_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	if (s->close_when_inactive)
		return;

	if (s->is_clear_on_media_end || s->is_looping)
		obs_source_preload_video(s->source, f);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct ffmpeg_source *s = opaque;
	obs_source_output_audio(s->source, a);
}

static void source_restart (void *data, bool debug);

static void media_stopped(void *opaque)
{
	struct ffmpeg_source *s = opaque;
  source_restart(s, false);
    return;
	if (s->is_clear_on_media_end) {
		obs_source_output_video(s->source, NULL);
		if (s->close_when_inactive && s->media_valid)
			s->destroy_media = true;
	}
}

static void ffmpeg_source_open(struct ffmpeg_source *s)
{
	if (s->input && *s->input)
		s->media_valid = mp_media_init(&s->media,
				s->input, s->input_format,
				s->buffering_mb * 1024 * 1024,
				s, get_frame, get_audio, media_stopped,
				preload_frame,
				s->is_hw_decoding,
				s->is_local_file || s->seekable,
				s->range);
}


static bool rescue (void *data, bool debug)
{
  struct ffmpeg_source *c = data;
  double source_frames = obs_source_get_total_frames(c->source);
  double diff;
  time_t current_time = time(NULL);
  struct tm * time_info;
  char timeString[9];
  bool attempting_restart;

  if ( c->source_frames != 0 ) {

    diff = (double) source_frames - c->source_frames;

    if ( diff > 0 && c->last_frame_at != current_time ) {
      if (c->restarted == true)
	    	printf("rescue: resuming to normal state after restart\n");
      c->last_frame_at = current_time;
      c->restarted = false;
      c->restart_status = 0;

      if ( debug ) {
        time_info = localtime((time_t) &current_time);
        strftime(timeString, sizeof(timeString), "%H:%M:%S", time_info);
        printf("last_frame_at: %s, id: %s\n", timeString, obs_source_get_name(c->source));
      }

    }
    
    if ( diff < 0 ) {
      printf("%s skipped to the past!\n", obs_source_get_name(c->source));

      source_restart(c, debug);
    }

    if ( c->last_frame_at != NULL && (current_time - c->last_frame_at) > 10 ) {
      printf("%s 10 seconds without new frames!\n", obs_source_get_name(c->source));

      source_restart(c, debug);

      // restart the counter so that the log message will appear again in the next 10 seconds
      c->last_frame_at = current_time;
    }
  }

  // update source_frames
  c->source_frames = source_frames;

  switch ( attempting_restart ) {
    case true:
      // stop 
      return false;
    
    default:
    case false:
      // proceed
      return true;
  }

}


static void preprocess(struct ffmpeg_source **s_p);

static void source_restart (void *data, bool debug)
{

  struct ffmpeg_source *c = data;

  bool opt_preprocess = get_opt_preprocess();

  // at start and each 30 secs
  if (opt_preprocess)
    if (c->restart_status == 0 || (c->restart_status != 3 && c->restart_status%3 == 0)) {
      preprocess(&c);
    }

  // 30 secs
  if ( (!opt_preprocess && c->restart_status == 0) || (opt_preprocess && c->restart_status == 3 ) ) {
    if ( debug )
      printf("%s restarting...\n", obs_source_get_name(c->source));
    c->restarted = true;
    c->restart_status++;
    
    obs_source_output_video(c->source, NULL);
    obs_source_update(c->source, NULL);

    return true;
  } else {
    if ( debug )
      printf("%s already restarted, nothing to do\n", obs_source_get_name(c->source));
    
    c->restart_status++;
    return false;
  }
}


static void ffmpeg_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct ffmpeg_source *s = data;
  if ( !rescue(data, false) ) 
    return;

	if (s->destroy_media) {
		if (s->media_valid) {
			mp_media_free(&s->media);
			s->media_valid = false;
		}
		s->destroy_media = false;
	}
}

static void ffmpeg_source_start(struct ffmpeg_source *s)
{
	if (!s->media_valid)
		ffmpeg_source_open(s);

	if (s->media_valid) {
		mp_media_play(&s->media, s->is_looping);
		if (s->is_local_file)
			obs_source_show_preloaded_video(s->source);
	}
}

static void ffmpeg_source_update(void *data, obs_data_t *settings)
{
	struct ffmpeg_source *s = data;

	bool is_local_file = obs_data_get_bool(settings, "is_local_file");

	char *input;
	char *input_format;

	bfree(s->input);
	bfree(s->input_format);

	if (is_local_file) {
		input = (char *)obs_data_get_string(settings, "local_file");
		input_format = NULL;
		s->is_looping = obs_data_get_bool(settings, "looping");
		s->close_when_inactive = obs_data_get_bool(settings,
				"close_when_inactive");

		obs_source_set_async_unbuffered(s->source, true);
	} else {
		input = (char *)obs_data_get_string(settings, "input");
		input_format = (char *)obs_data_get_string(settings,
				"input_format");
		s->is_looping = false;
		s->close_when_inactive = true;

		obs_source_set_async_unbuffered(s->source, false);
	}

	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
#ifndef __APPLE__
	s->is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
#endif
	s->is_clear_on_media_end = obs_data_get_bool(settings,
			"clear_on_media_end");
	s->restart_on_activate = obs_data_get_bool(settings,
			"restart_on_activate");
	s->range = (enum video_range_type)obs_data_get_int(settings,
			"color_range");
	s->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	s->is_local_file = is_local_file;
	s->seekable = obs_data_get_bool(settings, "seekable");

	if (s->media_valid) {
		//mp_media_free(&s->media);
		s->media_valid = false;
	}

	bool active = obs_source_active(s->source);
	if (!s->close_when_inactive || active)
		ffmpeg_source_open(s);

	dump_source_info(s, input, input_format);
	if (!s->restart_on_activate || active)
		ffmpeg_source_start(s);



}

static const char *ffmpeg_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFMpegSource");
}

static void restart_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	UNUSED_PARAMETER(pressed);

	struct ffmpeg_source *s = data;
	if (obs_source_active(s->source))
		ffmpeg_source_start(s);
}

static void restart_proc(void *data, calldata_t *cd)
{
	restart_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t dur = 0;
	if (s->media.fmt)
		dur = s->media.fmt->duration;

	calldata_set_int(cd, "duration", dur * 1000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t frames = 0;

	if (!s->media.fmt) {
		calldata_set_int(cd, "num_frames", frames);
		return;
	}

	int video_stream_index = av_find_best_stream(s->media.fmt,
			AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

	if (video_stream_index < 0) {
		FF_BLOG(LOG_WARNING, "Getting number of frames failed: No "
				"video stream in media file!");
		calldata_set_int(cd, "num_frames", frames);
		return;
	}

	AVStream *stream = s->media.fmt->streams[video_stream_index];

	if (stream->nb_frames > 0) {
		frames = stream->nb_frames;
	} else {
		FF_BLOG(LOG_DEBUG, "nb_frames not set, estimating using frame "
				"rate and duration");
		AVRational avg_frame_rate = stream->avg_frame_rate;
		frames = (int64_t)ceil((double)s->media.fmt->duration /
				(double)AV_TIME_BASE *
				(double)avg_frame_rate.num /
				(double)avg_frame_rate.den);
	}

	calldata_set_int(cd, "num_frames", frames);
}

static char *run_sync_forever(char *cmd) {
	
	printf("run sync forever: executing '%s'\n", cmd);

	FILE *fp = popen(cmd, "r");
	if (fp == NULL) {
		printf("run sync forever: failed to run command\n" );
		return NULL;
	}

	char path[1035];
	while (fgets(path, sizeof(path)-1, fp) != NULL) {
		printf("run sync forever: %s\n", path);
	}

	pclose(fp);
	return NULL;
}


static char *run_sync(char *cmd) {

	printf("run sync: executing '%s'\n", cmd);

	FILE *fp = popen(cmd, "r");
	if (fp == NULL) {
		printf("run sync: failed to run command\n" );
		return NULL;
	}

	int max = 1000;
	char *out = (char*) malloc(max*sizeof(char));
	strcpy(out, "");
	
	char path[1035];
	int i = 0;
	while (fgets(path, sizeof(path)-1, fp) != NULL) {
		i += strlen(path);
		if ((i+1) >= max) break;
		strcat(out, (char *) path);
	}

	pclose(fp);
	return out;
}

char * trim(char * s) {
	char * p = s;
	int l = strlen(p);

	while(isspace(p[l - 1])) p[--l] = 0;
	while(* p && isspace(* p)) ++p, --l;

	memmove(s, p, l + 1);
	return s;
}   

char** explode(char delimiter, char* str, int **size) {
	int l = strlen(str), i=0, j=0, k=0;
	char x = NULL;
	char** r = (char**)realloc(r, sizeof(char**));
	r[0] = (char*)malloc(l*sizeof(char));
	while (i<l+1) {
		x = str[i++];
		if (x==delimiter || x=='\0') {
			r[j][k] = '\0';
			r[j] = (char*)realloc(r[j], k*sizeof(char));
			k = 0;
			r = (char**)realloc(r, (++j+1)*sizeof(char**));
			r[j] = (char*)malloc(l*sizeof(char));
		} else {
			r[j][k++] = x;
		}
	}
	*size = j;
	return r;
}

static char ** probe(char *input) {
	char *cmd = (char*) malloc(1000*sizeof(char));
	char **codecs = (char **) malloc(6*sizeof(char *));
	strcpy(cmd, "timeout 20s ffprobe -v quiet -select_streams v:0 -show_entries stream=codec_name,width,height,index -of default=noprint_wrappers=1:nokey=1 \"");
	strcat(cmd, input);
	strcat(cmd, "\" | head -4");

	int *size = (int*) malloc(sizeof(int));
	char **data = explode('\n', trim(run_sync(cmd)), &size);
	if (size == 4) {
		printf("probe: got codec %s, %sx%s, index %s\n", data[1], data[2], data[3], data[0]);
	} else {
		printf("probe: failed to probe '%s'\n", input);
		return NULL;
	}
	return data;
}

char** get_rescale_size(char *scene_name, char *ffinput, int s_width, int s_height) {

	char *rescalecmd = malloc(500*sizeof(char));

	char *rescalescript = get_opt_rescale_script();

	if (rescalescript == NULL) {
		rescalescript = malloc(15*sizeof(char));
		strcpy(rescalescript, "rescale.py");
	}

	strcpy(rescalecmd, "python ");
	strcat(rescalecmd, rescalescript);
	strcat(rescalecmd, " ");
	strcat(rescalecmd, scene_name);
	strcat(rescalecmd, " \"");
	strcat(rescalecmd, ffinput);
	strcat(rescalecmd, "\"");

	char *out = trim(run_sync(rescalecmd));

	if (strcmp(out, "") == 0) {
		printf("get rescale size: no output '%s'\n", rescalecmd);
		return NULL;
	}

	int *size = (int*)malloc(sizeof(int));
	char **resize_to = explode(':', out, &size);

	if (size != 2) {
		printf("get rescale size: incorrect format '%s'\n", out);
		return NULL;
	}

	int r_width = atoi(resize_to[0]);
	int r_height = atoi(resize_to[1]);

	if (r_width > s_width) {
		printf("get rescale size: upscaling detected, disabling rescaling\n");
		return NULL;
	}


	if (s_width <= 0) {
		printf("get rescale size: incorrect stream width '%d'\n", s_width);
		return NULL;
	}

	// proportional resize
	int p_width = r_width;
	int p_height = (int) ((r_width*s_height)/s_width);

	char** r = (char**)malloc(sizeof(char**));
	r[0] = (char*)malloc(50*sizeof(char));
	r[1] = (char*)malloc(50*sizeof(char));

	sprintf(r[0], "%d", p_width);
	sprintf(r[1], "%d", p_height);

	printf("got rescale size: %sx%s\n", r[0], r[1]);

	return r;
}

char **can_preprocess(struct ffmpeg_source *s) {
	
	char **codecs = probe(s->ffinput);

	if (codecs == NULL) {
		printf("preprocessing failed: codecs could not be retrieved\n");
		return NULL;
	}

	if (strcmp(codecs[1],"h264") == 0 || strcmp(codecs[1],"mpeg2video") == 0) {
		return codecs;
	} else {
		printf("preprocessing failed: codec %s not supported\n", codecs[1]);
		return NULL;
	}
}
	

void *preprocess_thread(struct ffmpeg_source *s) {

	printf("preprocessing: sleeping to start stream %d secs...\n", s->i);
	sleep_ms(s->i * 1000);

	if (!get_opt_copy())
		while (s->codecs == NULL) {
			printf("preprocessing: trying to probe...\n");
			s->codecs = can_preprocess(s);

			if (s->codecs != NULL) break;
			printf("preprocessing: cannot get codecs retrying in two secs\n");
			sleep_ms(2*1000);
		}

	if (s->codecs == NULL) {
		printf("preprocess: cannot preprocess sending the input source as is to '%s'\n", s->ffoutput);
	}
	if (get_opt_copy()) {
		printf("preprocess: sending the input source as is to '%s'\n", s->ffoutput);
	}

	if (get_opt_copy() || s->codecs == NULL) {

		char *ffcmd = (char *) malloc(2000*sizeof(char));
		strcpy(ffcmd, "ffmpeg -i \"");
		strcat(ffcmd, s->ffinput);
		strcat(ffcmd, "\" -c:v copy -c:a copy -reset_timestamps 1 -f mpegts pipe:1 > \"");
		strcat(ffcmd, s->ffoutput);
		strcat(ffcmd, "\"");		

		run_sync_forever(ffcmd);

	} else {
		char *ffcmd = (char *) malloc(2000*sizeof(char));
		strcpy(ffcmd, "ffmpeg -hwaccel_device 0 -hwaccel cuvid -c:v ");
		if (strcmp(s->codecs[1],"mpeg2video") == 0) {
			strcat(ffcmd, "mpeg2_cuvid");
		}
		
		if (strcmp(s->codecs[1],"h264") == 0) {
			strcat(ffcmd, "h264_cuvid");

		}
		strcat(ffcmd, "  -surfaces 10 -drop_second_field 1 -deint 2 -i  \"");
		strcat(ffcmd, s->ffinput);
		strcat(ffcmd, "\" ");
		
		s->rescale = get_rescale_size(s->scene_name, s->ffinput, atoi(s->codecs[2]), atoi(s->codecs[3]));
		if (s->rescale != NULL) {
			//strcat(ffcmd, " -vf scale_npp=");
			//strcat(ffcmd, s->rescale[0]);
			//strcat(ffcmd, ":");
			//strcat(ffcmd, s->rescale[1]);
			//hwupload_cuda,scale... interp_algo=lanczos
			strcat(ffcmd, " -filter:v \"scale_npp=w=");
			strcat(ffcmd, s->rescale[0]);
			strcat(ffcmd, ":h=");
			strcat(ffcmd, s->rescale[1]);
			strcat(ffcmd, ":format=nv12:interp_algo=super,hwdownload,format=nv12\" ");
		} else {
			printf("preprocess: no hw rescaling\n");
		}

		// omit audio select first video stream
		strcat(ffcmd, "-map 0:");
		strcat(ffcmd, s->codecs[0]);
		strcat(ffcmd, " -c:v h264_nvenc -force_key_frames 'expr:gte(t,n_forced*2)' -g 50 -r 25 -bf 2 -qp 19 -profile:v main -level 4.0 -preset llhq -b:v 2500k -minrate 2500k -maxrate 3500k -movflags frag_keyframe+empty_moov+faststart ");// -acodec mp3 -b:a 128k -ar 44100 -reset_timestamps 1 
		strcat(ffcmd, " -f mpegts pipe:1 > \"");
		strcat(ffcmd, s->ffoutput);
		strcat(ffcmd, "\"");		

		run_sync_forever(ffcmd);
	}
}

static void preprocess(struct ffmpeg_source **s_p) {

	struct ffmpeg_source *s = *s_p;

	if (s->tid != NULL) {
		printf("preprocess: cancelling thread\n");
		pthread_cancel(s->tid);
	}

	pthread_t tid;
	int err = pthread_create(&tid, NULL, &preprocess_thread, s);

        if (err != 0)
            printf("preprocess: can't create thread :[%s]\n", strerror(err));
        else {
	    s->tid = tid;
            printf("preprocess: thread created successfully\n");
	}
}

static void *ffmpeg_source_create(obs_data_t *settings, obs_source_t *source)
{

	UNUSED_PARAMETER(settings);
	struct ffmpeg_source *s = bzalloc(sizeof(struct ffmpeg_source));

	/**
	 * preprocess: start
	 */
	bool opt_preprocess = get_opt_preprocess();
	if (opt_preprocess) {
		s->i = (int) obs_data_get_int(settings, "i");
		s->scene_name =  (char *)obs_data_get_string(settings, "scene_name");
		s->ffinput =  (char *)obs_data_get_string(settings, "ffinput");
		s->ffoutput =  (char *)obs_data_get_string(settings, "ffoutput");
		preprocess(&s);
	}
	// preprocess: end

	s->source = source;

  s->source_frames = (double) 0;

	s->hotkey = obs_hotkey_register_source(source,
			"MediaSource.Restart",
			obs_module_text("RestartMedia"),
			restart_hotkey, s);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void restart()", restart_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)",
			get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)",
			get_nb_frames, s);
	ffmpeg_source_update(s, settings);

	return s;
}

static void ffmpeg_source_destroy(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);
	if (s->media_valid)
		mp_media_free(&s->media);

	if (s->sws_ctx != NULL)
		sws_freeContext(s->sws_ctx);
	bfree(s->sws_data);
	bfree(s->input);
	bfree(s->input_format);
	bfree(s);
}

static void ffmpeg_source_activate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate)
		ffmpeg_source_start(s);
}

static void ffmpeg_source_deactivate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate) {
		if (s->media_valid) {
			mp_media_stop(&s->media);

			if (s->is_clear_on_media_end)
				obs_source_output_video(s->source, NULL);
		}
	}
}

struct obs_source_info ffmpeg_source = {
	.id             = "ffmpeg_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
	                  OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = ffmpeg_source_getname,
	.create         = ffmpeg_source_create,
	.destroy        = ffmpeg_source_destroy,
	.get_defaults   = ffmpeg_source_defaults,
	.get_properties = ffmpeg_source_getproperties,
	.activate       = ffmpeg_source_activate,
	.deactivate     = ffmpeg_source_deactivate,
	.video_tick     = ffmpeg_source_tick,
	.update         = ffmpeg_source_update
};
