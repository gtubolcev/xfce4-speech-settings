/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The voice keys from the network end up in file names and URLs; the zip
 * archives of RHVoice are unpacked from the network too.
 */

#include <string.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "src/voice-installer-private.h"

/* A zip archive of stored files; @mode: Unix mode of each one, 0: none */
typedef struct
{
  const gchar *name;
  const gchar *data;
  guint32      mode;
} Entry;

static void
put16 (GByteArray *a,
       guint       v)
{
  guint8 b[2] = { v & 0xff, (v >> 8) & 0xff };

  g_byte_array_append (a, b, 2);
}

static void
put32 (GByteArray *a,
       guint32     v)
{
  put16 (a, v & 0xffff);
  put16 (a, v >> 16);
}

static gchar *
make_zip (const gchar *dir,
          const Entry *entries,
          guint        n)
{
  GByteArray *zip = g_byte_array_new ();
  GByteArray *central = g_byte_array_new ();
  gchar      *path = g_build_filename (dir, "test.zip", NULL);
  guint       i;

  for (i = 0; i < n; i++)
    {
      guint32 offset = zip->len;
      gsize   name = strlen (entries[i].name);
      gsize   size = strlen (entries[i].data);
      guint32 crc = 0;

      put32 (zip, 0x04034b50); put16 (zip, 10); put16 (zip, 0); put16 (zip, 0);
      put32 (zip, 0); put32 (zip, crc); put32 (zip, size); put32 (zip, size);
      put16 (zip, name); put16 (zip, 0);
      g_byte_array_append (zip, (const guint8 *) entries[i].name, name);
      g_byte_array_append (zip, (const guint8 *) entries[i].data, size);

      put32 (central, 0x02014b50); put16 (central, (3 << 8) | 20); put16 (central, 10);
      put16 (central, 0); put16 (central, 0); put32 (central, 0); put32 (central, crc);
      put32 (central, size); put32 (central, size); put16 (central, name);
      put16 (central, 0); put16 (central, 0); put16 (central, 0); put16 (central, 0);
      put32 (central, entries[i].mode << 16); put32 (central, offset);
      g_byte_array_append (central, (const guint8 *) entries[i].name, name);
    }

  i = zip->len;
  g_byte_array_append (zip, central->data, central->len);
  put32 (zip, 0x06054b50); put16 (zip, 0); put16 (zip, 0); put16 (zip, n); put16 (zip, n);
  put32 (zip, central->len); put32 (zip, i); put16 (zip, 0);

  g_assert_true (g_file_set_contents (path, (const gchar *) zip->data, zip->len, NULL));
  g_byte_array_unref (zip);
  g_byte_array_unref (central);

  return path;
}

/* unpacks @entries into a new directory; the result of vi_unzip() */
static gboolean
try_unzip (const Entry *entries,
           guint        n,
           gchar      **out_dir)
{
  gchar   *dir = g_dir_make_tmp ("vi-unzip-XXXXXX", NULL);
  gchar   *zip = make_zip (dir, entries, n);
  gchar   *dest = g_build_filename (dir, "out", NULL);
  GError  *error = NULL;
  gboolean ok;

  g_mkdir (dest, 0700);
  ok = vi_unzip (zip, dest, NULL, &error);
  if (!ok)
    {
      g_assert_nonnull (error);
      g_error_free (error);
    }
  g_free (zip);
  g_free (dest);
  *out_dir = dir;

  return ok;
}

static void
test_unzip (void)
{
  const Entry good[] = {
    { "voice.info", "name=Test\n", 0100644 },
    { "16000/", "", 040755 },
    { "16000/dur.pdf", "abc", 0 },
  };
  const Entry *bad[] = {
    (const Entry[]) { { "../evil", "x", 0 } },
    (const Entry[]) { { "/tmp/evil", "x", 0 } },
    (const Entry[]) { { "a/../../evil", "x", 0 } },
    (const Entry[]) { { "a\\evil", "x", 0 } },
    (const Entry[]) { { "link", "/etc/passwd", 0120777 } },
    (const Entry[]) { { "", "x", 0 } },
  };
  const Entry twice[] = {
    { "a", "1", 0 },
    { "a", "2", 0 },
  };
  gchar *dir, *path, *text;
  guint  i;

  g_assert_true (try_unzip (good, G_N_ELEMENTS (good), &dir));
  path = g_build_filename (dir, "out", "16000", "dur.pdf", NULL);
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "abc");
  g_free (text);
  g_free (path);
  g_assert_true (vi_remove_tree (dir));
  g_free (dir);

  for (i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_assert_false (try_unzip (bad[i], 1, &dir));
      path = g_build_filename (dir, "evil", NULL);
      g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
      g_free (path);
      path = g_build_filename (dir, "out", "link", NULL);
      g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));
      g_free (path);
      g_assert_true (vi_remove_tree (dir));
      g_free (dir);
    }

  g_assert_false (try_unzip (twice, 2, &dir));
  g_assert_true (vi_remove_tree (dir));
  g_free (dir);
}

static void
test_damaged_zip (void)
{
  gchar  *dir = g_dir_make_tmp ("vi-unzip-XXXXXX", NULL);
  gchar  *zip = g_build_filename (dir, "test.zip", NULL);
  GError *error = NULL;
  guint8  junk[100];
  guint   i;

  for (i = 0; i < sizeof junk; i++)
    junk[i] = i * 7;
  /* an end record pointing far beyond the file */
  memcpy (junk + 78, "PK\x05\x06\0\0\0\0\x01\0\x01\0\xff\xff\0\0\xff\xff\0\0\0\0", 22);
  g_assert_true (g_file_set_contents (zip, (const gchar *) junk, sizeof junk, NULL));
  g_assert_false (vi_unzip (zip, dir, NULL, &error));
  g_clear_error (&error);
  g_assert_true (g_file_set_contents (zip, "PK", 2, NULL));
  g_assert_false (vi_unzip (zip, dir, NULL, &error));
  g_clear_error (&error);
  g_assert_true (vi_remove_tree (dir));
  g_free (zip);
  g_free (dir);
}

static void
test_licence_line (void)
{
  const gchar *readme = "# Anatol\n\nUkrainian voice.\n\n## License ##\n\n"
                        "[CC BY-NC-ND 4.0](https://creativecommons.org/)\n";
  const gchar *card = "# Model card\n\n* License: cc0\n";
  gchar       *licence;

  licence = vi_licence_line (readme, strlen (readme));
  g_assert_cmpstr (licence, ==, "CC BY-NC-ND 4.0");
  g_free (licence);
  licence = vi_licence_line (card, strlen (card));
  g_assert_cmpstr (licence, ==, "cc0");
  g_free (licence);
  g_assert_null (vi_licence_line ("nothing", 7));
}

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
  g_test_add_func ("/voice-installer/unzip", test_unzip);
  g_test_add_func ("/voice-installer/damaged-zip", test_damaged_zip);
  g_test_add_func ("/voice-installer/licence-line", test_licence_line);

  return g_test_run ();
}
