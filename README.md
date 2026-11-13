# xfce4-speech-settings

A settings dialog for [speech-dispatcher](https://github.com/brailcom/speechd), the
text-to-speech service of the Linux desktop. Two builds of the same code:
`xfce4-speech-settings` appears in the Xfce settings manager; `speech-settings` is
the same dialog in plain GTK 3, for other desktops.

- language, synthesizer and voice (the real voices of the synthesizer for the
  language);
- rate, pitch and volume in percent;
- punctuation, capital letters, audio output;
- a test sentence in the chosen language.

It edits `~/.config/speech-dispatcher/speechd.conf` in place, keeping comments, and
makes the running speech-dispatcher reload it.

Modern neural voices (Supertonic, Vosk TTS, Russian Kokoro …) for speech-dispatcher
are in [linux-tts](https://github.com/gtubolcev/linux-tts).

## Install

Ubuntu 26.04 and 24.04, from the PPA:

```sh
sudo add-apt-repository ppa:gtubolcev/linux-tts
sudo apt install xfce4-speech-settings        # Xfce
sudo apt install speech-settings-standalone   # other desktops
```

## Build

```sh
meson setup build --prefix=/usr
ninja -C build
meson test -C build
```

Needs GTK 3 and libspeechd; libxfce4ui-2 and libxfce4util for the Xfce build.
`-Dxfce=disabled` builds `speech-settings` without them (by default the Xfce
build is made when the libraries are there). `build-deb.sh` builds a .deb in a
docker image (`speech-build`).

Status: early, version 0.1. Plans are in [PLAN.md](PLAN.md).

## License

GPL-2.0-or-later, see [COPYING](COPYING).
