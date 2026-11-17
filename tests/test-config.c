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

#include <gio/gio.h>

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
  path = g_build_filename (conf_dir, "kokoro.conf", NULL);
  g_assert_true (g_file_set_contents (path, start, -1, NULL));

  g_assert_true (speechd_module_has_option ("kokoro", "KokoroPreload"));
  g_assert_false (speechd_module_has_option ("kokoro", "KokoroBert"));
  g_assert_null (speechd_module_get_option ("kokoro", "KokoroPreload"));

  /* under the example; numbers unquoted, names quoted */
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroPreload", "1", NULL));
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroIdleTimeout", "0", NULL));
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroVoice", "a b", NULL));
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
  value = speechd_module_get_option ("kokoro", "KokoroPreload");
  g_assert_cmpstr (value, ==, "1");
  g_free (value);

  /* a new value replaces the line in place */
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroIdleTimeout", "60", NULL));
  value = speechd_module_get_option ("kokoro", "KokoroIdleTimeout");
  g_assert_cmpstr (value, ==, "60");
  g_free (value);

  /* removing gives back the file as it was */
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroPreload", NULL, NULL));
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroIdleTimeout", NULL, NULL));
  g_assert_true (speechd_module_set_option ("kokoro", "KokoroVoice", NULL, NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, start);
  g_free (contents);

  /* bad input is refused, nothing written */
  g_assert_false (speechd_module_set_option ("kokoro", "KokoroPreload", "x\"y", NULL));
  g_assert_false (speechd_module_set_option ("kokoro", "KokoroPreload", "1\nZ 2", NULL));
  g_assert_false (speechd_module_set_option ("../x", "KokoroPreload", "1", NULL));
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, start);
  g_free (contents);

  g_free (path);
  g_free (conf_dir);
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
  g_assert_cmpstr (speechd_module_base ("espeak-ng"), ==, "espeak-ng");
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
  g_test_add_func ("/config/module-variant", test_module_variant);
  g_test_add_func ("/config/changed-on-disk", test_changed_on_disk);
  g_test_add_func ("/config/language-module", test_language_module);

  return g_test_run ();
}
