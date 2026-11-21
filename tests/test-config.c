/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* Tests for the speechd.conf editor. They run in temporary XDG
 * directories, never on the real configuration. */

#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "src/speechd-config.h"

static const gchar *sample =
  "# The DefaultRate controls how fast the synthesizer is going to speak.\n"
  "# DefaultRate  0\n"
  "\n"
  "DefaultVolume 100\n"
  "# DefaultLanguage \"en-US\"\n"
  "BeginClient \"emacs:*\"\n"
  "    DefaultPunctuationMode \"some\"\n"
  "EndClient\n"
  "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
  "DefaultModule \"piper\"\n"
  "Include \"clients/*.conf\"\n";

static const gchar *expected =
  "# The DefaultRate controls how fast the synthesizer is going to speak.\n"
  "# DefaultRate  0\n"
  "DefaultRate -20\n"
  "\n"
  "DefaultVolume 80\n"
  "# DefaultLanguage \"en-US\"\n"
  "DefaultLanguage \"ru\"\n"
  "BeginClient \"emacs:*\"\n"
  "    DefaultPunctuationMode \"some\"\n"
  "EndClient\n"
  "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
  "# DefaultModule \"piper\"\n"
  "Include \"clients/*.conf\"\n"
  "DefaultPunctuationMode \"all\"\n";



/* G_TEST_OPTION_ISOLATE_DIRS gives every test its own empty, temporary
 * XDG directories, which GLib removes afterwards */
static gchar *
setup_config_home (void)
{
  return g_strdup (g_get_user_config_dir ());
}



static void
cleanup_config_home (gchar *dir)
{
  g_free (dir);
}



static void
test_edit (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents, *value;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_assert_true (g_file_set_contents (path, sample, -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);
  g_assert_cmpstr (speechd_config_get_path (config), ==, path);

  /* keys are case-insensitive, like in Speech Dispatcher */
  value = speechd_config_get (config, "defaultmodule");
  g_assert_cmpstr (value, ==, "piper");
  g_free (value);
  g_assert_cmpint (speechd_config_get_int (config, "DefaultVolume", 0), ==, 100);
  g_assert_cmpint (speechd_config_get_int (config, "DefaultRate", 7), ==, 7);

  /* a client section is not a global default */
  g_assert_null (speechd_config_get (config, "DefaultPunctuationMode"));

  /* the same value changes nothing */
  speechd_config_set_int (config, "DefaultVolume", 100);
  g_assert_false (speechd_config_is_dirty (config));

  speechd_config_set_int (config, "DefaultRate", -20);
  speechd_config_set_int (config, "DefaultVolume", 80);
  speechd_config_set (config, "DefaultLanguage", "ru");
  speechd_config_set (config, "DefaultPunctuationMode", "all");
  speechd_config_unset (config, "DefaultModule");
  g_assert_true (speechd_config_is_dirty (config));
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_false (speechd_config_is_dirty (config));
  speechd_config_free (config);

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, expected);
  g_free (contents);

  config = speechd_config_load (NULL);
  value = speechd_config_get (config, "DefaultLanguage");
  g_assert_cmpstr (value, ==, "ru");
  g_free (value);
  g_assert_null (speechd_config_get (config, "DefaultModule"));
  speechd_config_free (config);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



/* Clearing and retyping the language must not pile up comment lines */
static void
test_unset_set (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents;
  SpeechdConfig *config;
  guint          i;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_assert_true (g_file_set_contents (path,
                                      "# DefaultLanguage \"en-US\"\n"
                                      "DefaultVolume 100\n", -1, NULL));

  config = speechd_config_load (NULL);
  for (i = 0; i < 3; i++)
    {
      speechd_config_set (config, "DefaultLanguage", "lt");
      speechd_config_unset (config, "DefaultLanguage");
    }
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "# DefaultLanguage \"en-US\"\n"
                                 "DefaultVolume 100\n");
  g_free (contents);

  speechd_config_set (config, "DefaultLanguage", "lv");
  g_assert_true (speechd_config_save (config, NULL));
  speechd_config_free (config);
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "# DefaultLanguage \"en-US\"\n"
                                 "DefaultLanguage \"lv\"\n"
                                 "DefaultVolume 100\n");
  g_free (contents);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



static void
test_no_user_file (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *path;
  SpeechdConfig *config;

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);

  /* nothing is written until something changes */
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  speechd_config_set_int (config, "DefaultRate", 10);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
  speechd_config_free (config);

  g_free (path);
  cleanup_config_home (dir);
}



static void
test_detect_modules (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents;
  GError        *error = NULL;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", "modules", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  path = g_build_filename (conf_dir, "piper.conf", NULL);
  g_assert_true (g_file_set_contents (path, "GenericExecuteSynth \"true\"\n",
                                      -1, NULL));
  g_free (path);
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (g_file_set_contents (path,
                                      "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
                                      "AddModule \"rhvoice\" \"sd_rhvoice\" \"rhvoice.conf\"\n"
                                      "DefaultModule \"piper\"\n"
                                      "LanguageDefaultModule \"ru\" \"piper\"\n",
                                      -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_true (speechd_config_has_module_list (config));
  g_assert_true (speechd_config_use_detected_modules (config, &error));
  g_assert_no_error (error);
  g_assert_false (speechd_config_has_module_list (config));
  g_assert_true (speechd_config_save (config, NULL));
  speechd_config_free (config);

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
                   "# AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
                   "# AddModule \"rhvoice\" \"sd_rhvoice\" \"rhvoice.conf\"\n"
                   "DefaultModule \"piper-generic\"\n"
                   "LanguageDefaultModule \"ru\" \"piper-generic\"\n");
  g_free (contents);
  g_free (path);

  /* the old file stays, the copy is what Speech Dispatcher finds */
  path = g_build_filename (conf_dir, "piper-generic.conf", NULL);
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "GenericExecuteSynth \"true\"\n");
  g_free (contents);
  g_free (path);

  /* a module under another name cannot be detected: nothing changes */
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (g_file_set_contents (path,
                                      "AddModule \"mbrola\" \"sd_espeak-ng\" \"mbrola.conf\"\n",
                                      -1, NULL));
  config = speechd_config_load (NULL);
  g_assert_false (speechd_config_use_detected_modules (config, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_false (speechd_config_is_dirty (config));
  speechd_config_free (config);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



static void
test_module_option (void)
{
  gchar  *dir = setup_config_home ();
  gchar  *conf_dir, *path, *contents, *value;
  const gchar *start =
    "# free the model after this many seconds\n"
    "#KokoroIdleTimeout 300\n"
    "#KokoroPreload 0\n"
    "KokoroThreads 2\n";

  conf_dir = g_build_filename (dir, "speech-dispatcher", "modules", NULL);
  g_assert_cmpint (g_mkdir_with_parents (conf_dir, 0700), ==, 0);
  path = g_build_filename (conf_dir, "foo.conf", NULL);
  g_assert_true (g_file_set_contents (path, start, -1, NULL));

  g_assert_true (speechd_module_has_option ("foo", "KokoroPreload"));
  g_assert_false (speechd_module_has_option ("foo", "KokoroBert"));
  g_assert_null (speechd_module_get_option ("foo", "KokoroPreload"));

  /* under the example; numbers unquoted, names quoted */
  g_assert_true (speechd_module_set_option ("foo", "KokoroPreload", "1", NULL));
  g_assert_true (speechd_module_set_option ("foo", "KokoroIdleTimeout", "0", NULL));
  g_assert_true (speechd_module_set_option ("foo", "KokoroVoice", "a b", NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
                   "# free the model after this many seconds\n"
                   "#KokoroIdleTimeout 300\n"
                   "KokoroIdleTimeout 0\n"
                   "#KokoroPreload 0\n"
                   "KokoroPreload 1\n"
                   "KokoroThreads 2\n"
                   "KokoroVoice \"a b\"\n");
  g_free (contents);
  value = speechd_module_get_option ("foo", "KokoroPreload");
  g_assert_cmpstr (value, ==, "1");
  g_free (value);

  /* a new value replaces the line in place */
  g_assert_true (speechd_module_set_option ("foo", "KokoroIdleTimeout", "60", NULL));
  value = speechd_module_get_option ("foo", "KokoroIdleTimeout");
  g_assert_cmpstr (value, ==, "60");
  g_free (value);

  /* also when the option ends the file (this read past the lines once) */
  g_assert_true (speechd_module_set_option ("foo", "KokoroVoice", "c d", NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_true (g_str_has_suffix (contents, "KokoroThreads 2\nKokoroVoice \"c d\"\n"));
  g_free (contents);
  value = speechd_module_get_option ("foo", "KokoroVoice");
  g_assert_cmpstr (value, ==, "c d");
  g_free (value);

  /* removing gives back the file as it was */
  g_assert_true (speechd_module_set_option ("foo", "KokoroPreload", NULL, NULL));
  g_assert_true (speechd_module_set_option ("foo", "KokoroIdleTimeout", NULL, NULL));
  g_assert_true (speechd_module_set_option ("foo", "KokoroVoice", NULL, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, start);
  g_free (contents);

  /* bad input is refused, nothing written */
  g_assert_false (speechd_module_set_option ("foo", "KokoroPreload", "x\"y", NULL));
  g_assert_false (speechd_module_set_option ("foo", "KokoroPreload", "1\nZ 2", NULL));
  g_assert_false (speechd_module_set_option ("../x", "KokoroPreload", "1", NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, start);
  g_free (contents);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



/* our modules read the user's file on top of the system one: the dialog
 * writes only what differs, and turns an old full copy into that */
/* migration happens once: hand-written files, a comment above our header
 * and families set on purpose stay; a second full copy gets its own backup */
static void
test_module_migrate_once (void)
{
  gchar *dir = setup_config_home ();
  gchar *sys = g_build_filename (dir, "sys", NULL);
  gchar *sys_modules = g_build_filename (sys, "modules", NULL);
  gchar *conf_dir = g_build_filename (dir, "speech-dispatcher", "modules", NULL);
  gchar *path = g_build_filename (conf_dir, "sherpa-onnx.conf", NULL);
  gchar *spath = g_build_filename (sys_modules, "sherpa-onnx.conf", NULL);
  gchar *backup = g_strconcat (path, ".before-delta", NULL);
  gchar *contents;
  const gchar *system =
    "# the system file\n"
    "# with comments\n"
    "SherpaFamilies \"supertonic piper vits\"\n"
    "SherpaProvider \"cpu\"\n"
    "SherpaVoice \"ru\" \"piper-ru-irina-medium\"\n";
  const gchar *hand[] = {
    "SherpaFamilies \"piper\"\nSherpaVoice \"ru\" \"piper-ru-dmitri-medium\"\n",
    "# mine\n# Changes to the system file of this synthesizer, which is read first\n"
    "SherpaProvider \"cpu\"\nSherpaFamilies \"piper\"\n",
  };
  GDir *d;
  const gchar *name;
  guint backups = 0, i;

  g_setenv ("SPEECH_SETTINGS_SYSCONFDIR", sys, TRUE);
  g_assert_cmpint (g_mkdir_with_parents (sys_modules, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (conf_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (spath, system, -1, NULL));
  g_assert_true (g_file_set_contents (backup, "older\n", -1, NULL));

  for (i = 0; i < G_N_ELEMENTS (hand); i++)
    {
      g_assert_true (g_file_set_contents (path, hand[i], -1, NULL));
      g_assert_false (speechd_modules_migrate ());
      g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
      g_assert_cmpstr (contents, ==, hand[i]);
      g_free (contents);
    }

  /* a full copy, though .before-delta is there from an earlier one */
  contents = g_strconcat (system, "SherpaProvider \"vulkan\"\n", NULL);
  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  g_free (contents);
  g_assert_true (speechd_modules_migrate ());
  g_assert_false (speechd_modules_migrate ());
  g_assert_true (g_file_get_contents (backup, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "older\n");
  g_free (contents);
  d = g_dir_open (conf_dir, 0, NULL);
  while ((name = g_dir_read_name (d)) != NULL)
    if (g_str_has_prefix (name, "sherpa-onnx.conf.before-delta-"))
      backups++;
  g_dir_close (d);
  g_assert_cmpuint (backups, ==, 1);

  g_unsetenv ("SPEECH_SETTINGS_SYSCONFDIR");
  g_free (backup);
  g_free (spath);
  g_free (path);
  g_free (conf_dir);
  g_free (sys_modules);
  g_free (sys);
  cleanup_config_home (dir);
}



static void
test_module_layered (void)
{
  gchar *dir = setup_config_home ();
  gchar *sys = g_build_filename (dir, "sys", NULL);
  gchar *sys_modules = g_build_filename (sys, "modules", NULL);
  gchar *conf_dir = g_build_filename (dir, "speech-dispatcher", "modules", NULL);
  gchar *path = g_build_filename (conf_dir, "sherpa-onnx-vulkan-rs.conf", NULL);
  gchar *spath = g_build_filename (sys_modules, "sherpa-onnx-vulkan-rs.conf", NULL);
  gchar *vpath = g_build_filename (conf_dir, "vosk.conf", NULL);
  gchar *backup, *contents, *value;
  const gchar *header =
    "# Changes to the system file of this synthesizer, which is read first\n"
    "# (xfce4-speech-settings writes only the options that differ from it)\n";

  g_setenv ("SPEECH_SETTINGS_SYSCONFDIR", sys, TRUE);
  g_assert_cmpint (g_mkdir_with_parents (sys_modules, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (conf_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (spath,
                                      "# the system file\n"
                                      "SherpaFamilies \"supertonic piper vits\"\n"
                                      "SherpaProvider \"vulkan\"\n"
                                      "#SherpaPreload \"x\"\n"
                                      "SherpaVoice \"ru\" \"piper-ru-irina-medium\"\n", -1, NULL));
  /* a copy made before Supertonic ran on Vulkan, then changed */
  g_assert_true (g_file_set_contents (path,
                                      "# the system file\n"
                                      "SherpaFamilies \"piper vits\"\n"
                                      "SherpaProvider \"vulkan\"\n"
                                      "#SherpaPreload \"x\"\n"
                                      "SherpaIdleTimeout 300\n"
                                      "SherpaIdleTimeout 0\n"
                                      "SherpaVoice \"ru\" \"piper-ru-irina-medium\"\n"
                                      "SherpaVoice \"de\" \"piper-de-thorsten-medium\"\n", -1, NULL));
  g_assert_true (g_file_set_contents (vpath, "VoskThreads 2\n", -1, NULL));
  {
    gchar *svpath = g_build_filename (sys_modules, "vosk.conf", NULL);

    g_assert_true (g_file_set_contents (svpath, "# vosk\nVoskThreads 2\n", -1, NULL));
    g_free (svpath);
  }

  /* only the changed options stay, the old file is kept */
  g_assert_true (speechd_modules_migrate ());
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  value = g_strconcat (header, "SherpaIdleTimeout 0\n"
                       "SherpaVoice \"de\" \"piper-de-thorsten-medium\"\n", NULL);
  g_assert_cmpstr (contents, ==, value);
  g_free (value);
  g_free (contents);
  backup = g_strconcat (path, ".before-delta", NULL);
  g_assert_true (g_file_test (backup, G_FILE_TEST_EXISTS));
  /* a short file the user wrote is not a copy: left alone */
  g_assert_true (g_file_get_contents (vpath, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "VoskThreads 2\n");
  g_free (contents);
  g_assert_false (speechd_modules_migrate ());

  /* reading sees both files */
  value = speechd_module_get_option ("sherpa-onnx-vulkan-rs", "SherpaFamilies");
  g_assert_cmpstr (value, ==, "supertonic piper vits");
  g_free (value);
  value = speechd_module_get_voice ("sherpa-onnx-vulkan-rs", "ru", NULL);
  g_assert_cmpstr (value, ==, "piper-ru-irina-medium");
  g_free (value);
  value = speechd_module_get_voice ("sherpa-onnx-vulkan-rs", "de", NULL);
  g_assert_cmpstr (value, ==, "piper-de-thorsten-medium");
  g_free (value);
  g_assert_true (speechd_module_has_option ("sherpa-onnx-vulkan-rs", "SherpaPreload"));

  /* another voice is a line; the system's voice again removes it */
  g_assert_true (speechd_module_set_voice ("sherpa-onnx-vulkan-rs", "ru", NULL, "supertonic-ru-female-1", NULL));
  value = speechd_module_get_voice ("sherpa-onnx-vulkan-rs", "ru", NULL);
  g_assert_cmpstr (value, ==, "supertonic-ru-female-1");
  g_free (value);
  g_assert_true (speechd_module_set_voice ("sherpa-onnx-vulkan-rs", "ru", NULL, "piper-ru-irina-medium", NULL));
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaPreload", "piper-en-john-medium", NULL));
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaIdleTimeout", "60", NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  value = g_strconcat (header, "SherpaVoice \"de\" \"piper-de-thorsten-medium\"\n"
                       "SherpaPreload \"piper-en-john-medium\"\n"
                       "SherpaIdleTimeout 60\n", NULL);
  g_assert_cmpstr (contents, ==, value);
  g_free (value);
  g_free (contents);

  /* clearing an option the system file sets writes it empty */
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaProvider", NULL, NULL));
  value = speechd_module_get_option ("sherpa-onnx-vulkan-rs", "SherpaProvider");
  g_assert_cmpstr (value, ==, "");
  g_free (value);
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaProvider", "vulkan", NULL));

  /* back to the system values: only the German voice is left */
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaPreload", NULL, NULL));
  g_assert_true (speechd_module_set_option ("sherpa-onnx-vulkan-rs", "SherpaIdleTimeout", NULL, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  value = g_strconcat (header, "SherpaVoice \"de\" \"piper-de-thorsten-medium\"\n", NULL);
  g_assert_cmpstr (contents, ==, value);
  g_free (value);
  g_free (contents);

  /* a voice goes on top of the user's own file, whose line stays */
  g_assert_true (speechd_module_set_voice ("vosk", "ru", NULL, "vosk-ru-female-2", NULL));
  g_assert_true (g_file_get_contents (vpath, &contents, NULL, NULL));
  value = g_strconcat (header, "VoskThreads 2\n"
                       "VoskVoice \"ru\" \"vosk-ru-female-2\"\n", NULL);
  g_assert_cmpstr (contents, ==, value);
  g_free (value);
  g_free (contents);

  g_unsetenv ("SPEECH_SETTINGS_SYSCONFDIR");
  g_free (backup);
  g_free (vpath);
  g_free (spath);
  g_free (path);
  g_free (conf_dir);
  g_free (sys_modules);
  g_free (sys);
  cleanup_config_home (dir);
}



/* sd_sherpa-onnx under four names reads the Sherpa or the Piper options */
static void
test_module_variant (void)
{
  gchar *key;

  g_assert_cmpstr (speechd_module_base ("sherpa-onnx"), ==, "sherpa");
  g_assert_cmpstr (speechd_module_base ("sherpa-onnx-vulkan-rs"), ==, "sherpa");
  g_assert_cmpstr (speechd_module_base ("piper-sherpa-onnx"), ==, "piper");
  g_assert_cmpstr (speechd_module_base ("piper-sherpa-onnx-vulkan-rs"), ==, "piper");
  g_assert_cmpstr (speechd_module_base ("kokoro-vulkan-rs"), ==, "kokoro");
  g_assert_true (speechd_module_reads_changes ("kokoro-vulkan-rs"));
  g_assert_cmpstr (speechd_module_base ("espeak-ng"), ==, "espeak-ng");
  {
    gchar *v;

    v = speechd_module_gpu_variant ("kokoro");
    g_assert_cmpstr (v, ==, "kokoro-vulkan-rs");
    g_free (v);
    g_assert_null (speechd_module_gpu_variant ("kokoro-vulkan-rs"));
    g_assert_null (speechd_module_gpu_variant (NULL));
    g_assert_null (speechd_module_gpu_variant (""));
    v = speechd_module_cpu_variant ("sherpa-onnx-vulkan-rs");
    g_assert_cmpstr (v, ==, "sherpa-onnx");
    g_free (v);
    g_assert_null (speechd_module_cpu_variant ("sherpa-onnx"));
    g_assert_null (speechd_module_cpu_variant ("-vulkan-rs"));
    g_assert_null (speechd_module_cpu_variant (NULL));
  }
  g_assert_null (speechd_module_base (NULL));
  key = speechd_module_option_key ("sherpa-onnx-vulkan-rs", "Preload");
  g_assert_cmpstr (key, ==, "SherpaPreload");
  g_free (key);
  key = speechd_module_option_key ("piper-sherpa-onnx", "Voice");
  g_assert_cmpstr (key, ==, "PiperVoice");
  g_free (key);
  key = speechd_module_option_key ("kokoro", "Voice");
  g_assert_cmpstr (key, ==, "KokoroVoice");
  g_free (key);
  g_assert_cmpint (speechd_module_voice_setting ("piper-sherpa-onnx-vulkan-rs"), ==,
                   SPEECHD_VOICE_SETTING_MODULE);
}



/* The file changes on disk while the dialog has it open: saving applies the
 * dialog's edits to the new contents and keeps the other writer's lines. */
static void
test_changed_on_disk (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents, *outside;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_assert_true (g_file_set_contents (path,
                                      "DefaultModule \"vosk\"\n"
                                      "DefaultLanguage \"ru\"\n"
                                      "DefaultRate 10\n", -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);

  /* someone else adds lines and changes a key the dialog does not touch */
  outside = g_strconcat ("DefaultModule \"sherpa-onnx\"\n"
                         "DefaultLanguage \"ru\"\n"
                         "DefaultRate 10\n"
                         "LanguageDefaultModule \"de\" \"sherpa-onnx\"\n", NULL);
  g_assert_true (g_file_set_contents (path, outside, -1, NULL));

  /* the dialog changes the language and drops the rate */
  speechd_config_set (config, "DefaultLanguage", "de");
  speechd_config_unset (config, "DefaultRate");
  g_assert_true (speechd_config_save (config, NULL));

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
                   "DefaultModule \"sherpa-onnx\"\n"
                   "DefaultLanguage \"de\"\n"
                   "# DefaultRate 10\n"
                   "LanguageDefaultModule \"de\" \"sherpa-onnx\"\n");
  g_free (contents);

  /* the edits were written: a later change outside is not undone by them */
  g_assert_true (g_file_set_contents (path,
                                      "DefaultModule \"sherpa-onnx\"\n"
                                      "DefaultLanguage \"en\"\n", -1, NULL));
  speechd_config_set_int (config, "DefaultPitch", 5);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
                   "DefaultModule \"sherpa-onnx\"\n"
                   "DefaultLanguage \"en\"\n"
                   "DefaultPitch 5\n");
  g_free (contents);

  speechd_config_free (config);
  g_free (outside);
  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



static void
test_language_module (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents, *module;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_assert_true (g_file_set_contents (path,
    "DefaultModule \"vosk\"\n"
    "DefaultLanguage \"ru\"\n", -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_null (speechd_config_get_language_module (config, "de"));

  /* a new line goes after DefaultModule, the next after it */
  speechd_config_set_language_module (config, "de", "sherpa-onnx");
  speechd_config_set_language_module (config, "en", "rhvoice");
  speechd_config_set_language_module (config, "de", "piper");
  speechd_config_set_language_module (config, "en", "rhvoice");
  module = speechd_config_get_language_module (config, "DE");
  g_assert_cmpstr (module, ==, "piper");
  g_free (module);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
    "DefaultModule \"vosk\"\n"
    "LanguageDefaultModule \"de\" \"piper\"\n"
    "LanguageDefaultModule \"en\" \"rhvoice\"\n"
    "DefaultLanguage \"ru\"\n");
  g_free (contents);

  /* removing comments out every line of the language, also a duplicate
   * written by hand meanwhile */
  g_assert_true (g_file_set_contents (path,
    "DefaultModule \"vosk\"\n"
    "LanguageDefaultModule \"de\" \"piper\"\n"
    "LanguageDefaultModule \"en\" \"rhvoice\"\n"
    "LanguageDefaultModule \"de\" \"espeak-ng\"\n", -1, NULL));
  speechd_config_set_language_module (config, "de", NULL);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
    "DefaultModule \"vosk\"\n"
    "# LanguageDefaultModule \"de\" \"piper\"\n"
    "LanguageDefaultModule \"en\" \"rhvoice\"\n"
    "# LanguageDefaultModule \"de\" \"espeak-ng\"\n");
  g_free (contents);
  g_assert_null (speechd_config_get_language_module (config, "de"));

  speechd_config_free (config);
  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



/* A configuration that is a symbolic link (dotfiles) stays one */
static void
test_symlink (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *real_dir, *real, *contents, *link;
  SpeechdConfig *config;
  GStatBuf       st;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  real_dir = g_build_filename (dir, "dotfiles", NULL);
  real = g_build_filename (real_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_mkdir_with_parents (real_dir, 0700);
  g_assert_true (g_file_set_contents (real, sample, -1, NULL));
  g_assert_cmpint (g_chmod (real, 0640), ==, 0);
  /* relative, as a dotfiles tool makes it */
  g_assert_cmpint (symlink ("../dotfiles/speechd.conf", path), ==, 0);

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);
  speechd_config_set_int (config, "DefaultRate", -20);
  g_assert_true (speechd_config_save (config, NULL));
  speechd_config_free (config);

  g_assert_true (g_file_test (path, G_FILE_TEST_IS_SYMLINK));
  link = g_file_read_link (path, NULL);
  g_assert_cmpstr (link, ==, "../dotfiles/speechd.conf");
  g_assert_true (g_file_get_contents (real, &contents, NULL, NULL));
  g_assert_nonnull (strstr (contents, "DefaultRate -20"));
  g_assert_cmpint (g_stat (real, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0640);

  g_free (contents);
  g_free (link);
  g_free (real);
  g_free (real_dir);
  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

  g_test_add_func ("/config/edit", test_edit);
  g_test_add_func ("/config/unset-set", test_unset_set);
  g_test_add_func ("/config/no-user-file", test_no_user_file);
  g_test_add_func ("/config/detect-modules", test_detect_modules);
  g_test_add_func ("/config/module-option", test_module_option);
  g_test_add_func ("/config/module-layered", test_module_layered);
  g_test_add_func ("/config/module-migrate-once", test_module_migrate_once);
  g_test_add_func ("/config/module-variant", test_module_variant);
  g_test_add_func ("/config/changed-on-disk", test_changed_on_disk);
  g_test_add_func ("/config/language-module", test_language_module);
  g_test_add_func ("/config/symlink", test_symlink);

  return g_test_run ();
}
