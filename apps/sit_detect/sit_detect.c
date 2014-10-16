#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "sit_detect.h"

#define  M_PI  3.14159265358979323846

void fft(float *fftBuffer, long fftFrameSize, long sign);

#define IS_VALID_T1(x) (abs(31-x) < 2 ? 31 : abs(29-x) < 2 ? 29 : 0)
#define IS_VALID_T2(x) (abs(43-x) < 2 ? 43 : abs(45-x) < 2 ? 45 : 0)
#define IS_VALID_T3(x) (abs(56-x) < 3 ? 56 : 0)

static inline float peak_strength(float *x, int f) {
	float avg = (x[f-1]+x[f+1])/2.;
	return avg > 0 ? 10.*log10(x[f]/avg) : -96;
}

struct sit_detector {
	float *history[2];
	int16_t *tmp_buffer;
	int pos;
	int freq, len;
	int peak_lvl;
	float *buffer;
	int t1, t2, t3, nt;
	int busy_len;
};

static float window[256];
static int windowSet = 0;

struct sit_detector *sit_detector_new() {
	int i;
	struct sit_detector *rv = malloc(sizeof(struct sit_detector));
	rv->history[0] = calloc(1, sizeof(float)*128);
	rv->history[1] = calloc(1, sizeof(float)*128);
	rv->tmp_buffer = calloc(1, sizeof(int16_t)*256);
	rv->buffer = malloc(sizeof(float)*512);
	rv->pos = 0;
	rv->freq = 0;
	rv->len = 0;
	rv->peak_lvl = 0;
	rv->t1 = 0;
	rv->t2 = 0;
	rv->t3 = 0;
	rv->nt = 0;
	rv->busy_len = 0;
	if(!windowSet) {
		windowSet = 1;
		for(i=0;i<256;i++) {
			window[i] = 0.53836 - 0.46164*cos((2.*M_PI*(float)i)/255);
		}
	}
	return rv;
}

void sit_detector_process_full_frame(struct sit_detector *t, int16_t *frame) {
	int i, pos = 0, peak_lvl, moved_peak, tmp;
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
		t->history[1][i] = sqrt(re*re+im*im)/2.;
		avg += t->history[1][i];
		if(t->history[1][i] > peak) {
			peak = t->history[1][i];
			pos = i;
		}
	}
	avg /= 128.;
	if(pos > 5 && peak > 0.005 && peak > 45.*avg) {
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
	if(peak_strength(t->history[1], 15) > 1 && peak_strength(t->history[1], 20) > 1)
		t->busy_len += 32;
	else
		t->busy_len = 0;
	if((peak > 0.09 && peak > 15.*avg && fabs(peak - t->history[0][pos]) < (peak*0.03) && pos > 5) || t->len >= 96) {
		if(!t->t1) {
			if((tmp = IS_VALID_T1(t->freq))) {
				t->t1 = tmp;
				t->nt = 0;
			}
		} else if(!t->t2) {
			if((tmp = IS_VALID_T2(t->freq))) {
				t->t2 = tmp;
				t->nt = 0;
			}
		} else if(!t->t3) {
			if((tmp = IS_VALID_T3(t->freq))) {
				t->t3 = tmp;
				t->nt = 0;
			}
		} else {
			t->nt += 32;
		}
	} else {
		t->nt += 32;
	}
	if(t->t1 && t->nt > 200) {
		t->t1 = 0;
		t->t2 = 0;
		t->t3 = 0;
	}
}

void sit_detector_process_frame(struct sit_detector *t, int16_t *frame, int samples) {
	int s = 256 - t->pos;
	s = s > samples ? samples : s;
	memcpy(t->tmp_buffer+t->pos, frame, sizeof(int16_t)*s);
	t->pos += s;
	if(t->pos == 256) {
		sit_detector_process_full_frame(t, t->tmp_buffer);
		samples -= s;
		memcpy(t->tmp_buffer, frame+s, sizeof(int16_t)*samples);
		t->pos = samples;
	}
}

int sit_detector_detected_tones(struct sit_detector *t) {
	if(t->busy_len >= 192) {
		return SIT_BUSY;
	} else if(t->t3) {
		switch(t->t1) {
		case 31:
			switch(t->t2) {
			case 45:
				return SIT_NC;
				break;
			case 43:
				return SIT_VC;
				break;
			}
			break;
		case 29:
			switch(t->t2) {
				case 45:
					return SIT_RO;
					break;
				case 43:
					return SIT_IC;
					break;
			}
			break;
		}
	}
	return 0;
}

const char *sit_detector_get_cause(int sit) {
	switch(sit) {
		case SIT_NONE:
			return "No SIT detected";
		case SIT_NC:
			return "No circuit found";
		case SIT_IC:
			return "Operator intercept";
		case SIT_VC:
			return "Vacant circuit";
		case SIT_RO:
			return "Reorder (system busy)";
		case SIT_BUSY:
			return "Busy signal";
		default:
			return NULL;
	}
}

void sit_detector_free(struct sit_detector *t) {
	if(t) {
		free(t->history[0]);
		free(t->history[1]);
		free(t->tmp_buffer);
		free(t->buffer);
		free(t);
	}
}
