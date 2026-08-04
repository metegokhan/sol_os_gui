/* Compile the vendored MiniGB APU for SolarOS's native Waveshare format. */
#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "vendor/peanut_gb/minigb_apu.c"
