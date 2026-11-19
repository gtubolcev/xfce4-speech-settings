i18n.merge_file(
  input: app_name + '.desktop.in',
  output: app_name + '.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

# a speech bubble with a sound wave, not the speaker of the volume control
# (named after the build, so both packages can be installed)
install_data('icons/xfce4-speech-settings.svg',
  rename: app_name + '.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)
