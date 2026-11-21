# Original MP3 regression audio

All audio in this directory is generated specifically for these tests, with no
third-party music or speech. The fixtures are dedicated to the public domain
under CC0-1.0.

- `silence-stereo.mp3`, `silence-mono.mp3`: 40 valid MPEG-1 Layer III frames,
  44.1 kHz / 128 kbps, empty granules. Regenerate with
  `python tests/player/generate_fixtures.py`.
- `sine-source.wav`: generated PCM16 stereo, 44.1 kHz, one second, amplitude
  12000, left 440 Hz / right 880 Hz. The same Python generator produces it.
- `sine-stereo.mp3`: encode that original WAV with
  [mp3-enc-bsd](https://github.com/lieff/mp3-enc-bsd), commit
  `d5817086e598c71030a254899244a88d110826ee`:

  ```sh
  gcc -O2 -DUNIX -DBS_FORMAT=BINARY src/*.c -o mp3enc -lm
  ./mp3enc sine-source.wav sine-stereo.mp3
  ```

The temporary test encoder is BSD 2-Clause; it is not linked into the firmware
and is not required to run the checked-in regression fixtures. The firmware
decoder is minimp3, with its separate CC0 license under the component's vendor
directory. A CBR stream without a Xing/LAME gapless tag includes encoder delay
and padding in its displayed duration; this fixture intentionally covers that
case.
