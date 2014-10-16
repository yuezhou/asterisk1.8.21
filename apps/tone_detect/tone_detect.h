#include <inttypes.h>

struct tone_detector;

struct tone_detector *tone_detector_new(void);
void tone_detector_process_full_frame(struct tone_detector *t, int16_t *frame);
void tone_detector_process_frame(struct tone_detector *t, int16_t *frame, int samples);
int tone_detector_detected_tone(struct tone_detector *t);
int tone_detector_detected_escape_signal(struct tone_detector *t);
void tone_detector_free(struct tone_detector *t);
