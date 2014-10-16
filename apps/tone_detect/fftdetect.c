#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "tone_detect.h"

#define SAMPLES_PER_FRAME 160

int main() {
	struct tone_detector *detector = tone_detector_new();
	int ms = 0, r;
	int16_t frame[SAMPLES_PER_FRAME];
	for(;;) {
		r = read(0, frame, SAMPLES_PER_FRAME*2);
		if(r > 0) {
			ms += r/16;
			tone_detector_process_frame(detector, frame, r/2);
			if(tone_detector_detected_tone(detector)) {
				fprintf(stderr, "Detected tone at %dms\n", ms);
				break;
			}
			if(tone_detector_detected_escape_signal(detector)) {
				fprintf(stderr, "Detected escape tone at %dms\n", ms);
				break;
			}
		} else if(r < 0 && errno != EINTR) {
			perror("Read error");
			break;
		} else {
			break;
		}
	}
	tone_detector_free(detector);
	return 0;
}
