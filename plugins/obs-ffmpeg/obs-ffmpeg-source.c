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

#define _GNU_SOURCE

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <ctype.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <regex.h>

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

#include <sys/wait.h>


#include <media-playback/media.h>

#define FF_LOG(level, format, ...) \
	blog(level, "[Media Source]: " format, ##__VA_ARGS__)
#define FF_LOG_S(source, level, format, ...) \
	blog(level, "[Media Source '%s']: " format, \
			obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)


int preprocess_stream = 1;

static bool video_frame(struct ff_frame *frame, void *opaque);
static bool video_format(AVCodecContext *codec_context, void *opaque);

struct ffmpeg_source {
	mp_media_t media;
	bool media_valid;
	bool destroy_media;

	// rescue
	double source_frames;
	unsigned long last_frame_at;
	unsigned long real_last_frame_at;
	unsigned long last_25_fps;
	unsigned long started_at;
	bool restarted;

	// preprocess
	int restart_status;
	pthread_t *tid;
	pid_t pid;
	FILE *fp;
	char **rescale;
	char **codecs;
	char *scene_name;
	char *ffinput;
	char *ffoutput;
	int i;
	int port;
	float speed;
	int gave_up;

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
	struct tm * time_info = NULL;
	char timeString[9];
	bool attempting_restart;


	//printf("speed: %f\n", c->speed);

	if ( c->source_frames != 0 ) {

		diff = (double) source_frames - c->source_frames;

		if ( diff > 0 && c->last_frame_at != current_time ) {
			if (c->restarted == true) {
				printf("rescue: resuming to normal state after restart\n");
				c->started_at = time(NULL);
				c->last_25_fps = time(NULL);
			}
			c->real_last_frame_at = current_time;
			c->last_frame_at = current_time;
			c->restarted = false;
			c->restart_status = -1;
			if (c->source->video_fps >= 24.0) {
				c->last_25_fps = time(NULL);
			} else {
				printf("%s: %f fps\n", obs_source_get_name(c->source), c->source->video_fps);
			}

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


		// more than 10 secs without a frame; or fps below 25 for more than 2 secs
		if ( ( c->last_frame_at != NULL &&
					((current_time - c->last_frame_at) > 10 ) ) ||
				(c->last_25_fps != NULL && (c->source->video_fps < 10.0) && (current_time - c->last_25_fps) > 30 && (current_time - c->started_at) > 60 ) ||
				( c->speed < 0.95 && (current_time - c->started_at) > 120 )
		   ) {
			printf("%s 10 seconds without new frames OR fps below 10 for more than 30 secs OR ffmpeg speed < 0.7! fps: %f, last 25 fps before %d secs, speed %f\n", obs_source_get_name(c->source), c->source->video_fps, (current_time - c->last_25_fps), c->speed);

			source_restart(c, debug);

			// restart the counter so that the log message will appear again in the next 10 seconds
			c->last_frame_at = current_time;
			//c->last_25_fps = time(NULL);
			c->started_at = time(NULL);
		}
	} else {
		if (c->started_at == 0)
			c->started_at = time(NULL);
		if (current_time - c->started_at > 5) {
			printf("%s starting... \n",  obs_source_get_name(c->source));
			c->started_at = time(NULL);
		}

		if (c->gave_up == 1) {
			source_restart(c, debug);

		}
	}

	if (time_info != NULL)
		free(time_info);

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
	c->restart_status++;

	bool opt_preprocess = get_opt_preprocess();
	// at start and each 60 secs
	if (opt_preprocess) {
		if (c->restart_status == 0 || c->restart_status%5 == 0) {
			if ( debug )
				printf("%s restarting...\n", obs_source_get_name(c->source));

			c->restarted = true;
			preprocess(&c);
			return true;
		} else {
			if ( debug )
				printf("%s already restarted, nothing to do\n", obs_source_get_name(c->source));

			return false;
		}
	}

	// 30 secs
	if ( (!opt_preprocess && c->restart_status == 0) ) {
		if ( debug )
			printf("%s restarting...\n", obs_source_get_name(c->source));
		c->restarted = true;

		obs_source_output_video(c->source, NULL);
		obs_source_update(c->source, NULL);

		return true;
	} else {
		if ( debug )
			printf("%s already restarted, nothing to do\n", obs_source_get_name(c->source));

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
	printf("START!!\n");

	if (!s->media_valid)
		ffmpeg_source_open(s);

	if (s->media_valid) {
		mp_media_play(&s->media, s->is_looping);
		if (s->is_local_file)
			obs_source_show_preloaded_video(s->source);
	}
}

static void ffmpeg_source_update2(void *data, obs_data_t *settings)
{

	printf("UPDATE2\n");
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

	} else {
		input = (char *)obs_data_get_string(settings, "input");
		input_format = (char *)obs_data_get_string(settings,
				"input_format");
		s->is_looping = false;

		s->close_when_inactive = true;

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


}




static void ffmpeg_source_update(void *data, obs_data_t *settings)
{

	printf("UPDATE\n");
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



int split (const char *str, char c, char ***arr)
{
	int count = 1;
	int token_len = 1;
	int i = 0;
	char *p;
	char *t;

	p = str;
	while (*p != '\0')
	{
		if (*p == c)
			count++;
		p++;
	}

	*arr = (char**) calloc(count, sizeof(char*));
	if (*arr == NULL)
		exit(1);

	p = str;
	while (*p != '\0')
	{
		if (*p == c)
		{
			(*arr)[i] = (char*) calloc(token_len, sizeof(char) );
			if ((*arr)[i] == NULL)
				exit(1);

			token_len = 0;
			i++;
		}
		p++;
		token_len++;
	}
	(*arr)[i] = (char*) calloc(token_len, sizeof(char) );
	if ((*arr)[i] == NULL)
		exit(1);

	i = 0;
	p = str;
	t = ((*arr)[i]);
	while (*p != '\0')
	{
		if (*p != c && *p != '\0')
		{
			*t = *p;
			t++;
		}
		else
		{
			*t = '\0';
			i++;
			t = ((*arr)[i]);
		}
		p++;
	}

	return count;
}


struct arg_struct {
	struct ffmpeg_source *s;
	int p_stderr;

};


char * trim(char * s) {
	char * p = s;
	int l = strlen(p);

	while(isspace(p[l - 1])) p[--l] = 0;
	while(* p && isspace(* p)) ++p, --l;

	memmove(s, p, l + 1);
	return s;
}



void str_replace(char *target, const char *needle, const char *replacement)
{
	char buffer[1024] = { 0 };
	char *insert_point = &buffer[0];
	const char *tmp = target;
	size_t needle_len = strlen(needle);
	size_t repl_len = strlen(replacement);

	while (1) {
		const char *p = strstr(tmp, needle);

		// walked past last occurrence of needle; copy remaining part
		if (p == NULL) {
			strcpy(insert_point, tmp);
			break;
		}

		// copy part before needle
		memcpy(insert_point, tmp, p - tmp);
		insert_point += p - tmp;

		// copy replacement string
		memcpy(insert_point, replacement, repl_len);
		insert_point += repl_len;

		// adjust pointers, move on
		tmp = p + needle_len;
	}

	// write altered string back to target
	strcpy(target, buffer);
}



void parse_stderr_thread(void *arguments) {

	struct arg_struct *args = (struct arg_struct *)arguments;

	struct ffmpeg_source *s = args->s;
	int p_stderr = args->p_stderr;

	char buffer[140];

	// wait for ffmpeg to start
	sleep_ms(1*1000);

	while (read(p_stderr, buffer, sizeof(buffer)) != 0)
	{


		regex_t regex;
		int reti;
		char msgbuf[100];

		/* Compile regular expression */
		reti = regcomp(&regex, "speed=\w*([0-9]+\\.[0-9]+)?", REG_EXTENDED);
		if (reti) {
			printf("Could not compile regex\n");
			continue;
		}

		regmatch_t pmatch[10];

		int len;
		char result[100];

		/* Execute regular expression */
		reti = regexec(&regex, buffer, 10, pmatch, REG_NOTBOL);
		if (!reti) {

			for (int i = 0; pmatch[i].rm_so != -1; i++)
			{
				len = pmatch[i].rm_eo - pmatch[i].rm_so;
				memcpy(result, buffer + pmatch[i].rm_so, len);
				result[len] = '\0';

				if (i==1) {
					str_replace(result, ".", ",");
					s->speed = (float) atof(result);
					if (s->speed < 0.95) {
						printf("%s: warning speed %f\n", obs_source_get_name(s->source), s->speed);

					}
				}
			}
		}
		else if (reti == REG_NOMATCH) {
		}
		else {
			regerror(reti, &regex, msgbuf, sizeof(msgbuf));
			printf("Regex match failed: %s\n", msgbuf);
		}

		/* Free memory allocated to the pattern buffer by regcomp() */
		regfree(&regex);
	}

	s->speed  = 0.0;
	free(args);


	return NULL;

}

#define READ 0
#define WRITE 1

	pid_t
popen2(struct ffmpeg_source *s, const char *command, int *infp, int *outfp, char *fifo)
{
	int p_stdin[2], p_stdout[2], p_stderr[2];
	pid_t pid;

	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0 || pipe(p_stderr))
		return -1;


	//fcntl(p_stderr[1], F_SETPIPE_SZ, sizeof(size_t));
	//fcntl(p_stdin[1], F_SETPIPE_SZ, sizeof(size_t));
	//fcntl(p_stdout[1], F_SETPIPE_SZ, sizeof(size_t));

	printf("forking %s\n", fifo);

	pid = fork();

	if (pid < 0)
		return pid;
	else if (pid == 0)
	{

		//int fd = open(fifo, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

		//fcntl(fd, F_SETPIPE_SZ, sizeof(size_t));

		//dup2(fd, 1);   // make stdout go to file

		close(p_stderr[0]);
		dup2(p_stderr[1], 2);

		//dup2(p, 2);   // make stderr go to file - you may choose to not do this
		// or perhaps send stderr to another file

		//close(fd);     // fd no longer needed - the dup'ed handles are sufficient

		int pid = getpid();
		FILE *f = fopen("/tmp/obs/pid.txt", "a");
		if (f == NULL)
		{
			printf("Error opening file!\n");
		} else {
			if (pid)
				fprintf(f, "%d ", pid);
			fclose(f);
		}



		char **brk = NULL;
		int size = split(command, ' ', &brk);

		brk[size] = (char *) NULL;

		//can change to any exec* function family.
		//execl(brk, NULL);
		execv("/usr/local/bin/ffmpeg", brk);
		perror("execl");
		exit(1);
	}


	close(p_stderr[1]);  // close the write end of the pipe in the parent


	struct arg_struct *args = malloc(sizeof(struct arg_struct));
	args->s =  s;
	args->p_stderr =  p_stderr[0];

	pthread_t tid;
	int err = pthread_create(&tid, NULL, &parse_stderr_thread,(void *)args);


	// close unused descriptors on parent process.
	close(p_stdin[READ]);
	close(p_stdout[WRITE]);

	if (infp == NULL)
		close(p_stdin[WRITE]);
	else
		*infp = p_stdin[WRITE];

	if (outfp == NULL)
		close(p_stdout[READ]);
	else
		*outfp = p_stdout[READ];

	return pid;
}

static pid_t run_sync_forever2(struct ffmpeg_source *s, char *cmd, char *fifo) {
	int *infp = malloc(sizeof(int));
	int *outfp = malloc(sizeof(int));
	pid_t pid = popen2(s, cmd, infp, outfp, fifo);
	free(infp);
	free(outfp);
	return pid;
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
int file_exists (char *filename)
{
	struct stat   buffer;
	return (stat (filename, &buffer) == 0);
}


static char ** probe(char *input, int reprobe, char timeout[]) {
    reprobe = 1;
	char *cmdres;
	char *input_hash = (char *) malloc(1000*sizeof(char));
	strcpy(input_hash, input);

	str_replace(input_hash, "/", "-");
	str_replace(input_hash, "?", "-");
	str_replace(input_hash, "=", "-");
	str_replace(input_hash, ":", "-");
	str_replace(input_hash, "&", "-");
	str_replace(input_hash, " ", "-");

	char *cache_path = (char *) malloc(1000*sizeof(char));
	strcpy(cache_path, "/tmp/obs/");
	strcat(cache_path, input_hash);


	char *cache_data = (char *) malloc(1002*sizeof(char));

	if( file_exists( cache_path ) ) {
		// if exists

		printf("cache found! %s\n", cache_path);

		FILE *file;
		file = fopen(cache_path, "r");
		if (file) {
			fread (cache_data, 1, 1000, file);
			fclose(file);
		}
		cmdres = cache_data;
	} else {

		cache_data = NULL;
	}



	char *cmd = (char*) malloc(1000*sizeof(char));
	char **codecs = (char **) malloc(6*sizeof(char *));
	strcpy(cmd, "timeout ");
	strcat(cmd, timeout);
	strcat(cmd, "s ffprobe -v quiet -select_streams v:0 -show_entries stream=codec_name,width,height,index -of default=noprint_wrappers=1:nokey=1 \"");
	strcat(cmd, input);
	strcat(cmd, "\" | head -4 2>/dev/null");


	if (reprobe == 1 || cache_data == NULL) {
		cmdres = trim(run_sync(cmd));
	}


	char **data = NULL;
	int size = split(cmdres, '\n', &data);

	if (size == 4) {

		FILE *fp = fopen(cache_path, "w");
		if (fp != NULL)
		{

			fprintf(fp, "%s", cmdres);
			fclose(fp);
		}

	
		printf("probe: got codec %s, %sx%s, index %s\n", data[1], data[2], data[3], data[0]);
	} else {
		printf("probe: failed to probe '%s'\n", input);
		data = NULL;


		return NULL;
	}


	//free(size);
	//free(cmdres);
	free(cache_data);
	free(cache_path);
	free(input_hash);
	free(cmd);

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
		free(rescalecmd);
		return NULL;
	}
	printf("%s\n", out);


	char **resize_to = NULL;
	int size = split(out, ':', &resize_to);


	if (size != 2) {
		printf("get rescale size: incorrect format '%s'\n", out);
		free(rescalecmd);
		return NULL;
	}

	int r_width = atoi(resize_to[0]);
	int r_height = atoi(resize_to[1]);

	free(rescalecmd);
	//free(size);
	free(resize_to);

	if (r_width > s_width) {
		printf("get rescale size: upscaling detected, disabling rescaling\n");
		return NULL;
	}

	if (s_width <= 0) {
		printf("get rescale size: incorrect stream width '%d'\n", s_width);
		return NULL;
	}


	printf("%d*%d / %d\n", r_width, s_height, s_width);

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

char **can_preprocess(char *input, int reprobe, char timeout[]) {

	char **codecs = probe(input, reprobe, timeout);

	if (codecs == NULL) {
		printf("preprocessing failed: codecs could not be retrieved\n");
		return NULL;
	}

	if (strcmp(codecs[1],"h264") == 0 || strcmp(codecs[1],"mpeg2video") == 0 || strcmp(codecs[1],"hevc") == 0) {
		return codecs;
	} else {
		printf("preprocessing failed: codec %s not supported\n", codecs[1]);
		return NULL;
	}
}


static void preprocess_exec(char *cmd) {

	printf("preprocess_exec: '%s'\n", cmd);

	FILE *fp = popen(cmd, "r");
	if (fp == NULL) {
		printf("sync: failed to run command\n" );
		return;
	}

	char temp[1035];
	while (fgets(temp, sizeof(temp)-1, fp) != NULL) {
		printf("%s\n", temp);
	}

	pclose(fp);
}



static void recreateFIFOAndsyncFS(char *fifo) {

	char *cmdrm = (char*) malloc(100*sizeof(char));
	strcpy(cmdrm, "rm -f ");
	strcat(cmdrm, fifo);
	preprocess_exec(cmdrm);


	char *cmdmk = (char*) malloc(100*sizeof(char));
	strcpy(cmdmk, "mkfifo ");
	strcat(cmdmk, fifo);
	preprocess_exec(cmdmk);

	// alternative
	//mkfifo(fifo, 0666);

	char *cmdsync = (char*) malloc(100*sizeof(char));
	strcpy(cmdsync, "sync");
	preprocess_exec(cmdsync);

	free(cmdrm);
	free(cmdmk);
	free(cmdsync);

}

static void ffmpeg_source_destroy(void *data);

void *preprocess_thread(struct ffmpeg_source *s) {

	//printf("preprocess: waiting 3 secs for prev processes to end...\n");
	//sleep_ms(3*1000);


	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	if (!get_opt_copy()) {
        s->codecs = NULL;
		while (s->codecs == NULL) {
			printf("preprocessing: trying to probe...\n");
			s->codecs = can_preprocess(s->ffinput, 0, "20");

			if (s->codecs != NULL) break;
			printf("preprocessing: cannot get codecs retrying in two secs\n");
			sleep_ms(2*1000);
		}
	}

	if (s->codecs == NULL) {
		printf("preprocess: cannot preprocess sending the input source as is to '%s'\n", s->ffoutput);
	}
	if (get_opt_copy()) {
		printf("preprocess: sending the input source as is to '%s'\n", s->ffoutput);
	}

	char *ffcmd = (char *) malloc(2000*sizeof(char));

	strcpy(ffcmd, "");
	if (get_opt_copy() || s->codecs == NULL) {

		strcpy(ffcmd, "/usr/local/bin/ffmpeg -i ");
		strcat(ffcmd, s->ffinput);
		strcat(ffcmd, " -c:v copy -c:a copy -reset_timestamps 1 -f mpegts pipe:1");//> \"");


	} else {
		strcpy(ffcmd, "/usr/local/bin/ffmpeg -rtbufsize 100M -err_detect ignore_err -fflags +genpts -c:v ");
		if (strcmp(s->codecs[1],"mpeg2video") == 0) {
			strcat(ffcmd, "mpeg2_cuvid");
		}

		if (strcmp(s->codecs[1],"h264") == 0) {
			strcat(ffcmd, "h264_cuvid");

		}
		strcat(ffcmd, " -surfaces 8 -drop_second_field 1 -deint 2 ");

		s->rescale = get_rescale_size(s->scene_name, s->input, atoi(s->codecs[2]), atoi(s->codecs[3]));
		if (s->rescale != NULL) {
			strcat(ffcmd, "-resize ");
			strcat(ffcmd, s->rescale[0]);
			strcat(ffcmd, "x");
			strcat(ffcmd, s->rescale[1]);
			strcat(ffcmd, " ");
		} else {
			printf("preprocess: no hw rescaling\n");
		}


		strcat(ffcmd, "-i ");

		strcat(ffcmd, s->ffinput);
		//strcat(ffcmd, " -pix_fmt yuv420p ");

		// omit audio select first video stream
		strcat(ffcmd, " -map 0:");
		strcat(ffcmd, s->codecs[0]);
		//strcat(ffcmd, " -c:v rawvideo -vf format=yuv444p");
		if (0 && s->rescale != NULL) {
			strcat(ffcmd, ",scale=");
			strcat(ffcmd, s->rescale[0]);
			strcat(ffcmd, ":");
			strcat(ffcmd, s->rescale[1]);
		}
		//strcat(ffcmd, " -buffer_size 0 -force_key_frames expr:gte(t,n_forced*0.2) -g 12 -f rawvideo ");
		//strcat(ffcmd, " -c:v hevc_nvenc -bf 0 -force_key_frames expr:gte(t,n_forced*0.2) -g 12 ");
		strcat(ffcmd, " -flags:v +global_header -c:v h264_nvenc -zerolatency 1 -force_key_frames expr:gte(t,n_forced*0.2) -g 5 -bf 0 -qp 19 -profile:v main -level 3.1 -preset ll -movflags frag_keyframe+empty_moov+faststart -flags global_header -bsf:v dump_extra -f mpegts ");// -map 0:1 -acodec mp3 -b:a 128k -ar 44100 -reset_timestamps 1 ");
		// -b:v 2500k -minrate 2500k -maxrate 3500k
		//strcat(ffcmd, "-f ts pipe:1");

		strcat(ffcmd, s->ffoutput);

	}


	//recreateFIFOAndsyncFS(s->ffoutput);

	printf("preprocess: executing \"%s\"...\n", ffcmd);
	s->pid = run_sync_forever2(s, ffcmd, s->ffoutput);
	s->fp = NULL;// fp->pipe_pid;

	free(ffcmd);

	//sleep_ms(10*1000);

	time_t current_time = time(NULL);
	char **ffcodecs = NULL;
	int retries = 0;

	//int fd, nread;
	//fd = open(s->input, O_RDONLY);

	//int flags = fcntl(fd, F_GETFL, 0);
	//fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	printf("preprocessing: is %d alive?\n", s->port);
	//sleep_ms(15*1000);
	int no_packet = 0;

	int gave_up = 0;

	if (1) {

        if (0) {
			int MSGBUFSIZE = 2;

			struct sockaddr_in addr;
			int fd, nbytes,addrlen;
			struct ip_mreq mreq;
			char msgbuf[MSGBUFSIZE];

			u_int yes=1;            /*** MODIFICATION TO ORIGINAL */

			/* create what looks like an ordinary UDP socket */
			if ((fd=socket(AF_INET,SOCK_DGRAM,0)) < 0) {
				perror("socket");
				exit(1);
			}


			/**** MODIFICATION TO ORIGINAL */
			/* allow multiple sockets to use the same PORT number */
			if (setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)) < 0) {
				perror("Reusing ADDR failed");
				exit(1);
			}
			/*** END OF MODIFICATION TO ORIGINAL */

			/* set up destination address */
			memset(&addr,0,sizeof(addr));
			addr.sin_family=AF_INET;
			addr.sin_addr.s_addr=htonl(INADDR_ANY); /* N.B.: differs from sender */
			addr.sin_port=htons(s->port);

			/* bind to receive address */
			if (bind(fd,(struct sockaddr *) &addr,sizeof(addr)) < 0) {
				perror("bind");
				exit(1);
			}

			/* use setsockopt() to request that the kernel join a multicast group */
			mreq.imr_multiaddr.s_addr=inet_addr("225.0.0.37");
			mreq.imr_interface.s_addr=htonl(INADDR_ANY);

			if (setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) < 0) {
				perror("setsockopt");
				exit(1);
			}

			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 100000;
			if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
				perror("Error");
			}
        }

		current_time = time(NULL);
		//if (0)
		while (1)
		{


            struct sockaddr_in si_me, si_other;
            int so, i, blen;
            socklen_t slen;
            char buf[100];
            so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            u_int yes = 1;
           
            setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
            if (setsockopt(so,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)) < 0) {
                printf("Reusing ADDR failed\n");
            }

            memset((char *) &si_me, 0, sizeof(si_me));
            si_me.sin_family = AF_INET;

            int prt = s->port;
            si_me.sin_port = htons(prt);
            
            if (bind(so, (struct sockaddr*) &si_me, sizeof(si_me))==-1)
                printf("bind");

			int res = recvfrom(so, buf, sizeof(buf), 0, (struct sockaddr*) &si_other, &slen);

		    close(so);

            printf("res port %d : %d\n", prt, res);
			if (res > 0) {
				break;
			}


			if (0) {
				//addrlen=sizeof(addr);
				//if ((nbytes=recvfrom(fd,msgbuf,MSGBUFSIZE,0,
				//				(struct sockaddr *) &addr,&addrlen)) < 0) {
				//}
				//if (strlen(msgbuf) != 0) {
				//	break;
				//}
			}
            sleep_ms(1000);

			if ((time(NULL) - current_time) > 20) {
                break;
				s->gave_up = 1;
				printf("preprocessing: %s any packet on the output for 20 secs...\n", s->ffoutput);
				return;
			}

		}

		//close(fd);
	}
	// give it some time
	//sleep_ms(3*1000);


	//sleep_ms(250);

	// must be ffprobable
	if (0)
		while (ffcodecs == NULL) {
			if (retries >=3) {

				printf("preprocessing: %s is not alive, giving up...\n", s->ffoutput);
				s->gave_up = 1;
				return;
			}

			retries++;
			printf("preprocessing: is %s alive? retry no. %d\n", s->ffoutput, retries);

			ffcodecs = can_preprocess(s->ffoutput, 1, "10");

			if (ffcodecs != NULL) break;
			sleep_ms(1*1000);
		}

	free(ffcodecs);

	printf("%s is alive!\n", s->ffoutput);

	current_time = time(NULL);

	mp_media_t *m = &s->media;

	// this will be handled at runtime
	if (0)
		while(s->speed < 0.8) {
			printf("%s waiting for %f > 0.8...\n", obs_source_get_name(s->source), s->speed);

			sleep_ms(1*1000);
			if ((time(NULL) - current_time) > 180) {
				// if speed will not raise quicky kill
				s->gave_up = 1;
				printf("preprocessing: %s < 0.8 for 10 secs , this is a candidate for restart...\n", s->ffoutput);
				return;
			}

		}

	if ( s->last_frame_at == NULL ) {

		// speed stabilisation
		sleep_ms(3*1000);


		if (preprocess_stream < s->i)
			while  (preprocess_stream != s->i) {
				sleep_ms(10);
			}


		// wait 3 secs for obs stabilization
		sleep_ms(3*1000);
		preprocess_stream++;
		//return;
	}

	//if (m->log != 1) {
	if ( s->last_frame_at == NULL ) {

		printf("%s starting thread...\n", obs_source_get_name(s->source));

		obs_source_output_video(s->source, NULL);
		obs_source_update(s->source, NULL);
		while (m == NULL || m->initialized == false) {
			m = &s->media;
			sleep_ms(10);
		}


		m->log = 1;
		return;
	} else {
		// no need in case of UDP
		if ( 0 && ( s->real_last_frame_at != NULL && (current_time - s->real_last_frame_at) > 60) ) {


			printf("%s no frames for the last 5 secs! restarting...\n", obs_source_get_name(s->source));

			m->initialized = false;
			m = NULL;
			obs_source_output_video(s->source, NULL);
			obs_source_update(s->source, NULL);
			while (m == NULL || m->initialized == false) {
				m = &s->media;
				sleep_ms(10);
			}

			m->log = 1;
			return;

		} else {
			printf("%s last frame before %d secs, nothing to do\n", obs_source_get_name(s->source), (current_time - s->real_last_frame_at));

		}
	}

}

static void preprocess(struct ffmpeg_source **s_p) {

	struct ffmpeg_source *s = *s_p;
	s->gave_up = 0;

	if (s->fp) {
		printf("FP close\n");
		fclose(s->fp);
	}
	if (s->pid) {
		printf("preprocess: PID kill %ld!\n", s->pid);
		kill(s->pid, SIGKILL);

		if (waitpid(s->pid, NULL, 0) < 0) {
			printf("Failed to collect child process\n");
		}
	}


	if (s->tid != NULL) {

		printf("preprocess: cancelling thread\n");
		pthread_cancel(s->tid);

		//wait till it is cancelled
		void *r;
		pthread_join(s->tid, &r);

		if (r == PTHREAD_CANCELED)
			printf("preprocess: thread was canceled\n");
		else
			printf("preprocess: thread wasn't canceled\n");

	}


	pthread_t tid;
	int err = pthread_create(&tid, NULL, &preprocess_thread, s);
	//int err = 0;
	//preprocess_thread(s);

	if (err != 0)
		printf("preprocess: can't create thread :[%s]\n", strerror(err));
	else {
		s->tid = tid;
		printf("preprocess: thread created successfully\n");
	}
}

static void *ffmpeg_source_create(obs_data_t *settings, obs_source_t *source)
{

	printf("CREATE\n");
	UNUSED_PARAMETER(settings);
	struct ffmpeg_source *s = bzalloc(sizeof(struct ffmpeg_source));
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
	ffmpeg_source_update2(s, settings);


	/**
	 * preprocess: start
	 */
	bool opt_preprocess = get_opt_preprocess();
	if (opt_preprocess) {
		s->i = (int) obs_data_get_int(settings, "i");
		s->port = (int) obs_data_get_int(settings, "port");

		s->gave_up = 0;
		s->scene_name =  (char *)obs_data_get_string(settings, "scene_name");
		char *ffinput = malloc(sizeof(char)*(strlen((char *)obs_data_get_string(settings, "ffinput"))+100));
		strcpy(ffinput, (char *)obs_data_get_string(settings, "ffinput"));


		char **data = NULL;
		int size = split(ffinput, '?', &data);


		if (size == 2) {
			strcpy(ffinput, data[0]);
		}

		strcat(ffinput, "?buffer_size=1000000&fifo_size=1000000&overrun_nonfatal=1&timeout=300000000&reuse=1");
		s->ffinput = ffinput;

		s->ffoutput =  (char *)obs_data_get_string(settings, "ffoutput");
		preprocess(&s);
	}
	// preprocess: end



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

	printf("ACTIVATE\n");
	return;
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
