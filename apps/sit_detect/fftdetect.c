#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "sit_detect.h"

int main() {
	struct sit_detector *detector = sit_detector_new();
	int ms = 0, r, t;
	int16_t frame[160];
	for(;;) {
		r = read(0, frame, 160*2);
		if(r > 0) {
			ms += r/16;
			sit_detector_process_frame(detector, frame, r/2);
			if((t = sit_detector_detected_tones(detector))) {
				fprintf(stderr, "Detected SIT at %dms: %s\n", ms, sit_detector_get_cause(t));
				break;
			}
		} else if(r < 0 && errno != EINTR) {
			perror("Read error");
			break;
		} else {
			break;
		}
	}
	sit_detector_free(detector);
	return 0;
}
