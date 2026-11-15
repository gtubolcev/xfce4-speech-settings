executable(
  app_name,
  'main.c',
  'speech-page.c',
  'speech-page.h',
  'speechd-config.c',
  'speechd-config.h',
  'voice-installer.c',
  'voice-installer.h',
  include_directories: include_directories('..'),
  dependencies: [gtk, gtk_x11, speechd, soup, json_glib] + xfce_deps,
  install: true,
)
