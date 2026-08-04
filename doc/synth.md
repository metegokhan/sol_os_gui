# Synth Service

`service.synth` is SolarOS's reusable real-time sample-generation layer. It
depends on `service.audio`: the audio service continues to own the board codec,
global volume, and exclusive PCM output, while the synth service owns the
render worker, block timing, and telemetry.

## Client contract

A client supplies `solar_os_synth_config_t` with a stable owner name, render
callback, callback context, and a block size from 32 through 512 frames. The
callback receives signed 16-bit interleaved stereo storage and the active board
sample rate. It must fill exactly the requested frame count without blocking or
performing filesystem or network I/O.

Only one client can run at a time. `solar_os_synth_start()` returns
`ESP_ERR_INVALID_STATE` when audio output or another synth client is busy.
The same owner stops the worker with `solar_os_synth_stop()`. Status reports
the owner, format, rendered frames and blocks, render deadline misses, write
errors, maximum render time, and the last service error.

The worker uses an internal-memory stack and a bounded internal PCM block. It
opens an exclusive `solar_os_audio_stream_t`, renders and writes blocks until
stopped, submits one silent tail block, and then releases the stream. The
stream must be opened, written, and closed by the same task because the audio
service serializes complete operations with a FreeRTOS mutex.
