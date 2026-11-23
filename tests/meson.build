test_config = executable(
  'test-config',
  'test-config.c',
  '../src/speechd-config.c',
  include_directories: include_directories('..'),
  dependencies: [dependency('gio-2.0')],
)
test('config', test_config)

test_voice_installer = executable(
  'test-voice-installer',
  'test-voice-installer.c',
  '../src/voice-installer.c',
  '../src/voice-packagekit.c',
  '../src/voice-source-audiocpp.c',
  '../src/voice-source-piper.c',
  '../src/voice-source-rhvoice.c',
  '../src/voice-unzip.c',
  include_directories: include_directories('..'),
  dependencies: [gtk, gtk_x11, x11, soup, json_glib] + xfce_deps,
)
test('voice-installer', test_voice_installer)
