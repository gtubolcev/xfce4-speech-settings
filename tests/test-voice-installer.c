/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The voice keys from the network end up in file names and URLs.
 */

#include <gtk/gtk.h>

#include "src/voice-installer.h"

static void
test_valid_key (void)
{
  g_assert_true (voice_installer_valid_key ("ru_RU-dmitri-medium"));
  g_assert_true (voice_installer_valid_key ("uk_UA-ukrainian_tts-medium"));
  g_assert_true (voice_installer_valid_key ("en_US-libritts_r-x_low"));
  g_assert_false (voice_installer_valid_key (NULL));
  g_assert_false (voice_installer_valid_key (""));
  g_assert_false (voice_installer_valid_key ("ru_RU-../../etc-medium"));
  g_assert_false (voice_installer_valid_key ("ru_RU-a/b-medium"));
  g_assert_false (voice_installer_valid_key ("ru_RU-dmitri-medium\n"));
  g_assert_false (voice_installer_valid_key ("ru_RU-dmitri-huge"));
  g_assert_false (voice_installer_valid_key ("ru_RU-dmitri medium-low"));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/voice-installer/valid-key", test_valid_key);

  return g_test_run ();
}
