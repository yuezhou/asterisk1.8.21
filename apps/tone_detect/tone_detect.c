#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "tone_detect.h"

#ifndef M_PI
#define  M_PI  3.14159265358979323846
#endif

void fft(float *fftBuffer, long fftFrameSize, long sign);

struct tone_detector {
	float *history[2];
	int16_t *tmp_buffer;
	int pos;
	int freq, len;
	int peak_lvl;
	float *buffer;
	int is_tone;
	int is_esc_signal;
};

static float window[256];
static int windowSet = 0;

struct tone_detector *tone_detector_new() {
	int i;
	struct tone_detector *rv = malloc(sizeof(struct tone_detector));
	rv->history[0] = calloc(1, sizeof(float)*128);
	rv->history[1] = calloc(1, sizeof(float)*128);
	rv->tmp_buffer = calloc(1, sizeof(int16_t)*256);
	rv->buffer = malloc(sizeof(float)*512);
	rv->pos = 0;
	rv->freq = 0;
	rv->len = 0;
	rv->peak_lvl = 0;
	rv->is_tone = 0;
	rv->is_esc_signal = 0;
	if(!windowSet) {
		windowSet = 1;
		/* Initialize window function */
		for(i=0;i<256;i++) {
			window[i] = 0.53836 - 0.46164*cos((2.*M_PI*(float)i)/255);
		}
	}
	return rv;
}

void tone_detector_process_full_frame(struct tone_detector *t, int16_t *frame) {
	int i, pos = 0, peak_lvl, moved_peak;
	float re, im;
	float peak = 0., avg = 0.;
	memcpy(t->history[0], t->history[1], sizeof(float)*128);
	memset(t->buffer, 0, sizeof(float)*512);
	do {
		peak_lvl = t->peak_lvl;
		moved_peak = 0;
		for(i=0;i<256;i++) {
			t->buffer[i*2] = ((float)frame[i]/(float)peak_lvl)*window[i];
			if(frame[i] > t->peak_lvl) {
				t->peak_lvl = frame[i];
				moved_peak = 1;
			}
		}
	} while(moved_peak);
	fft(t->buffer, 256, -1);
	for(i=0;i<128;i++) {
		re = t->buffer[i*2]/256.;
		im = t->buffer[i*2+1]/256.;
		/* Store magnitude of tone (rect -> polar transform) */
		t->history[1][i] = sqrt(re*re+im*im)/2.;
		avg += t->history[1][i];
		if(t->history[1][i] > peak) { /* If found louder peak */
			/* Store new peak */
			peak = t->history[1][i];
			pos = i;
		}
	}
	avg /= 128.;
	/* If frequency > ~156Hz, peak > 0.005, and peak is at least
	 * 45 times the average volume level, see as tone */
	if(peak > 0.005 && peak > 45.*avg) {
		if(pos == t->freq) {
			t->len += 32;
		} else {
			t->freq = pos;
			t->len = 32;
		}
	} else {
		t->freq = 0;
		t->len = 0;
	}
	/*
	 * t->len >= 64 detects more samples than t->len >= 96, but triggers
	 * falsely on some music. In practice, 96 rarely misses a tone anyway
	 */
	/* If peak > 0.09, peak at least 15 times average, peak less than 3% different in volume from last frame, and peak is at higher than 156Hz, or tone length > 96ms */
	if((peak > 0.09 && peak > 15.*avg && fabs(peak - t->history[0][pos]) < (peak*0.03)) || t->len >= 96) {
		if(pos > 5)
			t->is_tone = 1;
		else if(pos == 3 && t->len >= 300)
			t->is_esc_signal = 1;
	}
}

static void tone_detector_process_partial_frame(struct tone_detector *t, int16_t *frame, int samples) {
	int s = 256 - t->pos;
	s = s > samples ? samples : s;
	memcpy(t->tmp_buffer+t->pos, frame, sizeof(int16_t)*s);
	t->pos += s;
	if(t->pos == 256) {
		tone_detector_process_full_frame(t, t->tmp_buffer);
		samples -= s;
		memcpy(t->tmp_buffer, frame+s, sizeof(int16_t)*samples);
		t->pos = samples;
	}
}

void tone_detector_process_frame(struct tone_detector *t, int16_t *frame, int samples) {
	while(samples > 0) {
		int s = samples > 256 ? 256 : samples;
		tone_detector_process_partial_frame(t, frame, s);
		samples -= s;
		frame += s;
	}
}

int tone_detector_detected_tone(struct tone_detector *t) {
	return t->is_tone;
}

int tone_detector_detected_escape_signal(struct tone_detector *t) {
	return t->is_esc_signal;
}

void tone_detector_free(struct tone_detector *t) {
	if(t) {
		free(t->history[0]);
		free(t->history[1]);
		free(t->tmp_buffer);
		free(t->buffer);
		free(t);
	}
}
