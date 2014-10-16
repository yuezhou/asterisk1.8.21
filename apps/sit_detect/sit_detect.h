#include <inttypes.h>

#define SIT_NONE	0
#define SIT_NC		1
#define SIT_IC		2
#define SIT_VC		3
#define	SIT_RO		4
#define SIT_BUSY	5

struct sit_detector;

struct sit_detector *sit_detector_new(void);
void sit_detector_process_full_frame(struct sit_detector *t, int16_t *frame);
void sit_detector_process_frame(struct sit_detector *t, int16_t *frame, int samples);
int sit_detector_detected_tones(struct sit_detector *t);
const char *sit_detector_get_cause(int sit);
void sit_detector_free(struct sit_detector *t);
