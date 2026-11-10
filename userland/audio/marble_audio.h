/*
 * MarbleAudio userspace ABI.
 *
 * Derived from HakiOS/Topaz RedwoodAudio under the MIT license. This header is
 * deliberately limited to the HAL-to-kernel contract; hardware/DSP interfaces
 * stay private to the marble kernel.
 */
#ifndef _SYS_MARBLE_AUDIO_H_
#define _SYS_MARBLE_AUDIO_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define MARBLE_AUDIO_DEVICE    "/dev/marble-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define MARBLE_AUDIO_RATE      48000
#define MARBLE_AUDIO_CHANNELS  2
#define MARBLE_AUDIO_FRAME     4       /* bytes */

struct marble_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};

#define MARBLE_AUDIO_SET_VOLUME _IOW('m', 1, struct marble_audio_volume)

#endif /* _SYS_MARBLE_AUDIO_H_ */
