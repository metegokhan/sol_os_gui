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

## Native voice engine

`solar_os_synth_voice.h` builds a bounded musical voice engine on the callback
layer. It provides eight voices, automatic release-first voice stealing,
per-note velocity, square, triangle, saw, sine, and noise waveforms, and a
custom wavetable. Each voice also has a resonant low-pass filter with cutoff,
resonance, envelope amount, and an independent ADSR envelope. Configuration
changes update active voices immediately and also set the defaults for new
notes.
The render path uses fixed-point oscillators and envelopes; scripting runtimes
only submit control changes and never run inside the audio callback. The mixed
voice signal retains the same PCM headroom as the system tone generator before
the codec applies global speaker volume. Periodic oscillators are evaluated at
eight evenly spaced sub-samples per output frame and averaged before mixing.
The custom oscillator reads a service-owned 256-sample signed wavetable with
linear interpolation; complete table updates are copied under the voice lock so
the render callback never reads mutable client memory.
The filter uses a two-pole state-variable topology. Its coefficients update at
a bounded control rate, transitions between dry and filtered output are ramped,
and resonant peaks use soft limiting before voice mixing.
The service latches a consecutive 64-sample trace and fingerprint of a complete
final mono PCM block so status reports describe the samples submitted to audio
rather than inferring them from the selected waveform.

`solar_os_synth_voice_note_on()` lazily claims output for its owner. Matching
`note_off()` calls enter the release stage, `all_notes_off()` releases every
voice, and `stop()` immediately stops the worker and gives up audio ownership.
The global audio service remains responsible for speaker volume.

Python and Lua expose the engine as `solaros.synth`. Their runtime owners are
released automatically on normal exit, error, cancellation, or foreground-app
shutdown, so a script cannot leave an audio stream or sustained note behind.

The native foreground `synth` app turns the voice engine into a playable
instrument. Its Play tab pairs the waveform selector and live PCM oscilloscope
with an envelope graph, global speaker volume, editable ADSR knobs, and the
physical-key piano. Its Wave tab draws the custom wavetable at full width and
supports selectable 16, 32, 64, 128, and 256-point resolution; square,
triangle, saw, Supersaw, sine, and flat starting shapes; cursor and brush
editing; smoothing; normalization; reset; and undo. `Enter` cycles the resolution and
resamples the current shape into the new point count. The piano remains active
while editing, and table changes reshape held notes immediately.
The Filter tab pairs a live low-pass response graph with the independent filter
envelope. Cutoff, resonance, envelope amount, and filter ADSR are editable while
the piano remains active.

The app also shows current octave and velocity, active voices, sample rate, and
audio errors. Keyboard press and release events sustain held notes and support
chords. Waveform and envelope edits keep oscillator phase and pitch continuous.
After a note renders, the waveform panel shows the captured PCM trace with
automatic vertical scaling and the low 16 bits of its block fingerprint.
Python and Lua synth status return the same fingerprint, range, mean absolute
level, and trace samples.
