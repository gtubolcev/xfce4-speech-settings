# xfce4-speech-settings

A settings dialog for [speech-dispatcher](https://github.com/brailcom/speechd), the
text-to-speech service of the Linux desktop, for Xfce. It appears in the Xfce
settings manager.

- language, synthesizer and voice (the real voices of the synthesizer for the
  language);
- rate, pitch and volume in percent;
- punctuation, capital letters, audio output;
- a test sentence in the chosen language.

It edits `~/.config/speech-dispatcher/speechd.conf` in place, keeping comments, and
makes the running speech-dispatcher reload it.

Modern neural voices (Supertonic, Vosk TTS, Russian Kokoro …) for speech-dispatcher
are in [linux-tts](https://github.com/gtubolcev/linux-tts).

## Build

```sh
meson setup build --prefix=/usr
ninja -C build
meson test -C build
```

Needs GTK 3, libxfce4ui-2, libxfce4util and libspeechd. `build-deb.sh` builds a
.deb in a docker image (`speech-build`).

Status: early, version 0.1. Plans are in [PLAN.md](PLAN.md).

## License

GPL-2.0-or-later, see [COPYING](COPYING).
