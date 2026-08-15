# SolarOS 8-Bit Melody Player & Sound Visualizer
# Compatible with SolarOS MicroPython

import solaros

gfx = solaros.gfx
audio = solaros.audio
time = solaros.time

# Musical Notes Frequencies (Hz)
NOTE_C4 = 262
NOTE_D4 = 294
NOTE_E4 = 330
NOTE_F4 = 349
NOTE_G4 = 392
NOTE_A4 = 440
NOTE_B4 = 494
NOTE_C5 = 523
NOTE_D5 = 587
NOTE_E5 = 659
NOTE_F5 = 698
NOTE_G5 = 784
NOTE_REST = 0

# Mario Theme Melody
MELODY = [
    (NOTE_E5, 120), (NOTE_E5, 120), (NOTE_REST, 120), (NOTE_E5, 120),
    (NOTE_REST, 120), (NOTE_C5, 120), (NOTE_E5, 120), (NOTE_REST, 120),
    (NOTE_G5, 240), (NOTE_REST, 240), (NOTE_G4, 240), (NOTE_REST, 240),
    (NOTE_C5, 180), (NOTE_REST, 120), (NOTE_G4, 180), (NOTE_REST, 120),
    (NOTE_E4, 180), (NOTE_REST, 120), (NOTE_A4, 150), (NOTE_B4, 150),
    (NOTE_A4, 120), (NOTE_G4, 150), (NOTE_E5, 150), (NOTE_G5, 150),
    (NOTE_A5, 200), (NOTE_F5, 120), (NOTE_G5, 120), (NOTE_E5, 150),
    (NOTE_C5, 120), (NOTE_D5, 120), (NOTE_B4, 180)
]


def main():
    gfx.begin()
    w = gfx.width()
    h = gfx.height()

    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    gfx.font(gfx.FONT_BOLD)
    gfx.text(20, 30, "SOLAR-OS 8-BIT MELODY PLAYER")
    gfx.line(20, 38, w - 20, 38)
    gfx.font(gfx.FONT_SMALL)
    gfx.text(20, 60, "Playing: Super Mario Retro Theme")
    gfx.text(20, h - 15, "Press [ESC] to Stop & Exit")
    gfx.present()

    total_notes = len(MELODY)
    note_idx = 0

    for note in MELODY:
        freq = note[0]
        dur = note[1]

        # Draw visual equalizer bar
        gfx.clear(gfx.WHITE)
        gfx.color(gfx.BLACK)
        gfx.font(gfx.FONT_BOLD)
        gfx.text(20, 30, "SOLAR-OS 8-BIT MELODY PLAYER")
        gfx.line(20, 38, w - 20, 38)
        gfx.font(gfx.FONT_SMALL)
        gfx.text(20, 60, "Playing: Super Mario Retro Theme")

        # Note Info
        note_str = "Frequency: " + str(freq) + " Hz" if freq > 0 else "Note: Rest (Pause)"
        gfx.text(20, 90, note_str)
        gfx.text(20, 110, "Progress: " + str(note_idx + 1) + " / " + str(total_notes))

        # Graphic Visualizer
        bar_height = (freq - 200) * 100 // 600 if freq > 0 else 0
        if bar_height > 120:
            bar_height = 120
        if bar_height < 0:
            bar_height = 0

        gfx.rect(60, 140, 280, 100)
        if bar_height > 0:
            gfx.fill_rect(80 + (note_idx % 10) * 22, 238 - bar_height, 18, bar_height)

        gfx.text(20, h - 15, "Press [ESC] to Stop & Exit")
        gfx.present()

        # Play Tone
        if freq > 0:
            audio.tone(freq, dur)
        time.sleep_ms(dur + 30)

        note_idx = note_idx + 1

        # Check for ESC
        key = gfx.getch(10)
        if key == 27 or key == 113 or key == 81:
            break

    gfx.clear(gfx.WHITE)
    gfx.font(gfx.FONT_BOLD)
    gfx.text(40, 120, "Melody Finished!")
    gfx.font(gfx.FONT_SMALL)
    gfx.text(40, 150, "Press [ESC] to return")
    gfx.present()

    while True:
        k = gfx.getch(100)
        if k == 27 or k == 113 or k == 81 or k == 13 or k == 10:
            break

    gfx.end()


main()
