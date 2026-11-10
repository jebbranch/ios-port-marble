/*
 * Host-side contract test for MarbleAudio.driver.
 *
 * The production source is included unchanged; POSIX device calls are renamed
 * to mocks at preprocessing time. This exercises the factory, device list,
 * timestamp, PCM write and NaN-safe volume path without an iOS device.
 */
#define open marble_test_open
#define close marble_test_close
#define read marble_test_read
#define write marble_test_write
#define ioctl marble_test_ioctl
#include "marble_audio_plugin.c"
#undef open
#undef close
#undef read
#undef write
#undef ioctl

#include <assert.h>
#include <stdarg.h>

static size_t captured_bytes;
static int captured_peak;
static struct marble_audio_volume captured_volume;
static unsigned volume_calls;

int
marble_test_open(const char *path, int flags, ...)
{
	(void)flags;
	return strcmp(path, MARBLE_AUDIO_DEVICE) == 0 ? 42 : -1;
}

int
marble_test_close(int fd)
{
	(void)fd;
	return 0;
}

ssize_t
marble_test_read(int fd, void *data, size_t size)
{
	(void)fd;
	(void)data;
	(void)size;
	return -1;
}

ssize_t
marble_test_write(int fd, const void *data, size_t size)
{
	if (fd == 2) {
		return (ssize_t)size;
	}
	assert(fd == 42);
	assert(size % MARBLE_AUDIO_FRAME == 0);
	const int16_t *samples = data;

	for (size_t i = 0; i < size / sizeof(*samples); i++) {
		int value = samples[i] < 0 ? -samples[i] : samples[i];

		if (value > captured_peak) {
			captured_peak = value;
		}
	}
	captured_bytes += size;
	return (ssize_t)size;
}

int
marble_test_ioctl(int fd, unsigned long command, ...)
{
	va_list args;
	const struct marble_audio_volume *volume;

	assert(fd == 42);
	assert(command == MARBLE_AUDIO_SET_VOLUME);
	va_start(args, command);
	volume = va_arg(args, const struct marble_audio_volume *);
	va_end(args);
	captured_volume = *volume;
	volume_calls++;
	return 0;
}

int
main(void)
{
	AudioObjectPropertyAddress list = {
		kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0
	};
	UInt32 ids[8] = { 0 }, out_size = 0;
	UInt32 data_size = sizeof(ids);
	int16_t samples[256 * 2];
	Float64 sample_time;
	UInt64 host_time, seed;
	Float32 nan_volume = NAN;
	AudioObjectPropertyAddress scalar = {
		kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0
	};

	assert(MarbleAudio_Create(NULL, kAudioServerPlugInTypeUUID) == &driver);
	assert(initialize(&driver, NULL) == 0);
	assert(sink == 42);
	assert(get_property_data(&driver, OBJECT_PLUGIN, 0, &list, 0, NULL,
	    data_size, &out_size, ids) == 0);
	assert(out_size == 4 * sizeof(UInt32));
	assert(ids[0] == OBJECT_SPEAKER && ids[1] == OBJECT_HAWKING &&
	    ids[2] == OBJECT_ACTUATOR && ids[3] == OBJECT_CODEC);

	assert(start_io(&driver, OBJECT_SPEAKER, 1) == 0);
	for (unsigned i = 0; i < 256; i++) {
		samples[2 * i] = (int16_t)(i * 32);
		samples[2 * i + 1] = (int16_t)-(int)(i * 32);
	}
	assert(do_io_operation(&driver, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT,
	    1, kAudioServerPlugInIOOperationWriteMix, 256, NULL, samples, NULL) == 0);
	assert(captured_bytes == sizeof(samples));
	assert(captured_peak == 255 * 32);
	assert(written == 256 && refused == 0);

	assert(set_property_data(&driver, OBJECT_SPEAKER_VOLUME, 0, &scalar,
	    0, NULL, sizeof(nan_volume), &nan_volume) == 0);
	assert(volume_calls == 1);
	assert(captured_volume.muted == 1);
	assert(captured_volume.below == 48 * 8);

	assert(get_zero_time_stamp(&driver, OBJECT_SPEAKER, 1, &sample_time,
	    &host_time, &seed) == 0);
	assert(sample_time >= 0 && host_time != 0 && seed == 1);
	assert(stop_io(&driver, OBJECT_SPEAKER, 1) == 0);
	return 0;
}
