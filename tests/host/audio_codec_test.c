#include <assert.h>
#include <stdint.h>

#include "solar_os_audio_codec.h"

int main(void)
{
    const uint8_t mp3_header[] = {0xffU, 0xfbU, 0x90U, 0x64U};
    solar_os_stream_audio_format_t format;
    assert(solar_os_audio_mp3_probe(mp3_header, sizeof(mp3_header), &format) == ESP_OK);
    assert(format.sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE);
    assert(format.sample_rate == 44100U);
    assert(format.channels == 2U);
    assert(format.bits_per_sample == 16U);

    const uint8_t invalid[] = {0x00U, 0x11U, 0x22U, 0x33U};
    assert(solar_os_audio_mp3_probe(invalid, sizeof(invalid), &format) ==
           ESP_ERR_INVALID_RESPONSE);

    solar_os_audio_mp3_decoder_t *decoder = NULL;
    assert(solar_os_audio_mp3_decoder_create(&decoder) == ESP_OK);
    assert(decoder != NULL);
    int16_t pcm[SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES];
    size_t consumed = 0U;
    solar_os_audio_decoded_frame_t frame;
    assert(solar_os_audio_mp3_decode(decoder,
                                     invalid,
                                     sizeof(invalid),
                                     &consumed,
                                     pcm,
                                     SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES,
                                     &frame) == ESP_OK);
    assert(frame.frames == 0U);
    solar_os_audio_mp3_decoder_destroy(decoder);
    return 0;
}
