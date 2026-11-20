#!/bin/sh
# ci.sh - build and run the tests on Ubuntu, as root or with sudo.
# Used by .github/workflows/ci.yml; runs the same in a plain ubuntu container.
set -eu
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends build-essential meson pkgconf gettext \
  libgtk-3-dev libjson-glib-dev libsoup-3.0-dev libspeechd-dev libxfce4ui-2-dev \
  libxfce4util-dev >/dev/null
meson setup _ci --werror -Dwarning_level=2
meson compile -C _ci
meson test -C _ci --print-errorlogs
