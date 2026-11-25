/*
 * Copyright (C) 2026 Grigory Tuboltsev
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * The voices of RHVoice, from the catalogue of rhvoice.org. A voice comes
 * from the distribution when it has a package for the language
 * (rhvoice-<language>, through PackageKit); otherwise the zip archives of
 * the language and the voice are unpacked into ~/.local/share/RHVoice,
 * where a patched speech-dispatcher-rhvoice finds them.
 *
 * Each archive is checked against the MD5 sum of the catalogue. The sum
 * comes from the same site as the archive, so it finds a damaged download,
 * not a changed one: that is left to HTTPS.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memmem() */
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>
#ifdef HAVE_XFCE
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#include "lang-names.h"
#include "voice-installer-private.h"

#define MODULE_PATH       "/usr/lib/speech-dispatcher-modules/sd_rhvoice"
#define SYSTEM_DIR        "/usr/share/RHVoice"
/* the largest voices are some 100 MB, languages some 30 MB */
#define ZIP_MAX_SIZE      (512 * 1024 * 1024)
#define README_MAX_SIZE   (256 * 1024)
#define DATA_URL_PREFIX   "https://github.com/RHVoice/"
#define README_URL        "https://raw.githubusercontent.com/RHVoice/%s/HEAD/README.md"

typedef struct
{
  gint   refs;
  gchar *id;        /* "brazilian_portuguese" */
  gchar *name;      /* "Brazilian-Portuguese": its directory */
  gchar *code;      /* "pt" */
  gint   major;
  gint   minor;
  gchar *url;
  gchar *md5;       /* hex */
} RhvLanguage;

typedef struct
{
  RhvLanguage *language;
  gchar       *id;      /* "evgeniy_eng" */
  gchar       *dir;     /* "evgeniy-eng" */
  gchar       *name;
  gint         major;
  gint         minor;
  gchar       *url;
  gchar       *md5;
  gchar       *repo;    /* "evgeniy-eng" of github.com/RHVoice, or NULL */
} RhvVoice;

typedef struct
{
  GHashTable *licences;  /* repo -> licence */
} RhvData;

typedef struct
{
  Installer *in;
  RhvVoice  *voice;
  gchar     *zip;
  gchar     *unpacked;
  gboolean   language;   /* the language is downloaded too */
} RhvInstall;



static RhvLanguage *
language_unref (RhvLanguage *language)
{
  if (language != NULL && --language->refs == 0)
    {
      g_free (language->id);
      g_free (language->name);
      g_free (language->code);
      g_free (language->url);
      g_free (language->md5);
      g_free (language);
    }
  return NULL;
}



static void
rhv_free_voice (gpointer data)
{
  RhvVoice *voice = data;

  language_unref (voice->language);
  g_free (voice->id);
  g_free (voice->dir);
  g_free (voice->name);
  g_free (voice->url);
  g_free (voice->md5);
  g_free (voice->repo);
  g_free (voice);
}



static gpointer
rhv_new_data (void)
{
  RhvData *data = g_new0 (RhvData, 1);

  data->licences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return data;
}



static void
rhv_free_data (gpointer data)
{
  RhvData *rhv = data;

  g_hash_table_unref (rhv->licences);
  g_free (rhv);
}



/* ~/.local/share/RHVoice, as the module of speechd looks for it */
static gchar *
user_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "RHVoice", NULL);
}



static gchar *
user_path (const gchar *type,
           const gchar *name)
{
  gchar *base = user_dir ();
  gchar *path = g_build_filename (base, type, name, NULL);

  g_free (base);
  return path;
}



/* The module only finds voices in the home directory with our patch */
static gboolean
module_reads_user_dir (void)
{
  static gint known = -1;
  GMappedFile *file;

  if (known >= 0)
    return known;

  known = FALSE;
  file = g_mapped_file_new (MODULE_PATH, FALSE, NULL);
  if (file != NULL)
    {
      known = g_mapped_file_get_length (file) > 0
              && memmem (g_mapped_file_get_contents (file), g_mapped_file_get_length (file),
                         "XDG_DATA_HOME", strlen ("XDG_DATA_HOME")) != NULL;
      g_mapped_file_unref (file);
    }

  return known;
}



/* "format=4" and "revision=2" of voice.info or language.info; FALSE: none */
static gboolean
read_version (const gchar *dir,
              const gchar *info,
              gint        *major,
              gint        *minor)
{
  gchar  *path = g_build_filename (dir, info, NULL);
  gchar  *text = NULL;
  gchar **lines;
  guint   i;
  gsize   length;

  *major = *minor = -1;
  if (!g_file_get_contents (path, &text, &length, NULL) || length > 64 * 1024)
    {
      g_free (text);
      g_free (path);
      return FALSE;
    }

  lines = g_strsplit (text, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      g_strstrip (lines[i]);
      if (g_str_has_prefix (lines[i], "format="))
        *major = (gint) g_ascii_strtoll (lines[i] + 7, NULL, 10);
      else if (g_str_has_prefix (lines[i], "revision="))
        *minor = (gint) g_ascii_strtoll (lines[i] + 9, NULL, 10);
    }
  if (*major >= 0 && *minor < 0)
    *minor = 0;
  g_strfreev (lines);
  g_free (text);
  g_free (path);

  return *major >= 0;
}



static gint
version_compare (gint major_a,
                 gint minor_a,
                 gint major_b,
                 gint minor_b)
{
  if (major_a != major_b)
    return major_a < major_b ? -1 : 1;
  if (minor_a != minor_b)
    return minor_a < minor_b ? -1 : 1;
  return 0;
}



static gboolean
system_voice_dir (RhvVoice *voice)
{
  gchar   *path = g_build_filename (SYSTEM_DIR, "voices", voice->dir, NULL);
  gchar   *info = g_build_filename (path, "voice.info", NULL);
  gboolean found = g_file_test (info, G_FILE_TEST_EXISTS);

  g_free (info);
  g_free (path);
  return found;
}



static VoiceState
rhv_state (Installer *in,
           gpointer   data)
{
  RhvVoice *voice = data;
  gchar    *path = user_path ("voices", voice->dir);
  gchar    *info = g_build_filename (path, "voice.info", NULL);
  gboolean  user = g_file_test (info, G_FILE_TEST_EXISTS);

  g_free (info);
  g_free (path);

  if (user)
    return STATE_USER;
  return system_voice_dir (voice) ? STATE_SYSTEM : STATE_NONE;
}



/* The newest installed version of @language; FALSE: there is none.
 * @system: only the one of the system */
static gboolean
language_version (RhvLanguage *language,
                  gboolean     system,
                  gint        *major,
                  gint        *minor)
{
  gchar   *path = g_build_filename (SYSTEM_DIR, "languages", language->name, NULL);
  gint     user_major, user_minor;
  gboolean found = read_version (path, "language.info", major, minor);

  g_free (path);
  if (system)
    return found;

  path = user_path ("languages", language->name);
  if (read_version (path, "language.info", &user_major, &user_minor)
      && (!found || version_compare (user_major, user_minor, *major, *minor) > 0))
    {
      *major = user_major;
      *minor = user_minor;
      found = TRUE;
    }
  g_free (path);

  return found;
}



/* ------------------------------------------------------------ catalogue */

/* "0HiRfVZ5n0OiZfitp9FUyg==" -> hex; NULL if not an MD5 sum */
static gchar *
md5_hex (const gchar *base64)
{
  guchar  *raw;
  gsize    length = 0;
  GString *hex;
  gsize    i;

  if (base64 == NULL || !g_regex_match_simple ("^[A-Za-z0-9+/]{22}==$", base64, 0, 0))
    return NULL;

  raw = g_base64_decode (base64, &length);
  if (length != 16)
    {
      g_free (raw);
      return NULL;
    }
  hex = g_string_sized_new (32);
  for (i = 0; i < length; i++)
    g_string_append_printf (hex, "%02x", raw[i]);
  g_free (raw);

  return g_string_free (hex, FALSE);
}



/* "https://github.com/RHVoice/<repo>/releases/download/…zip", @repo: "<repo>";
 * or "https://rhvoice.org/download/…zip", @repo: NULL */
static gboolean
valid_url (const gchar *url,
           gchar      **repo)
{
  GMatchInfo *match = NULL;
  gboolean    valid;
  GRegex     *regex;

  if (url == NULL || strstr (url, "..") != NULL)
    return FALSE;

  if (g_regex_match_simple ("^https://rhvoice\\.org/download/[A-Za-z0-9_.+-]+\\.zip$", url, 0, 0))
    return TRUE;

  regex = g_regex_new ("^https://github\\.com/RHVoice/([A-Za-z0-9_.-]+)/releases/"
                       "download/[A-Za-z0-9_.+-]+/[A-Za-z0-9_.+-]+\\.zip$", 0, 0, NULL);
  valid = g_regex_match (regex, url, 0, &match);
  if (valid && repo != NULL)
    *repo = g_match_info_fetch (match, 1);
  g_match_info_free (match);
  g_regex_unref (regex);

  return valid;
}



static gboolean
read_catalogue_version (JsonObject *object,
                        gint       *major,
                        gint       *minor)
{
  JsonObject *version = vi_json_object (object, "version");

  *major = (gint) vi_json_int (version, "major", -1);
  *minor = (gint) vi_json_int (version, "minor", -1);

  return *major >= 0 && *major < 10000 && *minor >= 0 && *minor < 10000;
}



static RhvLanguage *
load_language (JsonObject *object)
{
  const gchar *id = vi_json_string (object, "id");
  const gchar *name = vi_json_string (object, "name");
  const gchar *code = vi_json_string (object, "lang2code");
  const gchar *url = vi_json_string (object, "dataUrl");
  gchar       *md5 = md5_hex (vi_json_string (object, "dataMd5"));
  RhvLanguage *language;
  gint         major, minor;

  if (id == NULL || name == NULL || code == NULL || md5 == NULL
      || !g_regex_match_simple ("^[a-z][a-z_]{1,40}$", id, 0, 0)
      || !g_regex_match_simple ("^[A-Za-z][A-Za-z-]{1,40}$", name, 0, 0)
      || !g_regex_match_simple ("^[a-z]{2,3}$", code, 0, 0)
      || !valid_url (url, NULL)
      || !read_catalogue_version (object, &major, &minor))
    {
      g_free (md5);
      return NULL;
    }

  language = g_new0 (RhvLanguage, 1);
  language->refs = 1;
  language->id = g_strdup (id);
  language->name = g_strdup (name);
  language->code = g_strdup (code);
  language->major = major;
  language->minor = minor;
  language->url = g_strdup (url);
  language->md5 = md5;

  return language;
}



static void
load_voice (Installer   *in,
            RhvLanguage *language,
            JsonObject  *object)
{
  const gchar *id = vi_json_string (object, "id");
  const gchar *name = vi_json_string (object, "name");
  const gchar *country = vi_json_string (object, "ctry2code");
  const gchar *accent = vi_json_string (object, "accent");
  const gchar *url = vi_json_string (object, "dataUrl");
  gchar       *md5 = md5_hex (vi_json_string (object, "dataMd5"));
  gchar       *repo = NULL;
  gchar       *label, *upper, *english;
  RhvVoice    *voice;
  gint         major, minor;

  if (id == NULL || name == NULL || md5 == NULL
      || !g_regex_match_simple ("^[a-z][a-z0-9_]{0,40}$", id, 0, 0)
      || !g_utf8_validate (name, -1, NULL) || strlen (name) > 64
      || !valid_url (url, &repo)
      || !read_catalogue_version (object, &major, &minor))
    {
      g_free (md5);
      g_free (repo);
      return;
    }

  voice = g_new0 (RhvVoice, 1);
  voice->language = language;
  language->refs++;
  voice->id = g_strdup (id);
  voice->dir = g_strdelimit (g_strdup (id), "_", '-');
  voice->name = g_strdup (name);
  voice->major = major;
  voice->minor = minor;
  voice->url = g_strdup (url);
  voice->md5 = md5;
  voice->repo = repo;

  /* "English (US)", "English (GB, Scotland)" */
  upper = country != NULL && *country != '\0' && strlen (country) <= 3
          ? g_ascii_strup (country, -1) : NULL;
  english = g_strdelimit (g_strdup (language->name), "-", ' ');
  /* "Englisch" in the user's language when iso-codes knows it */
  label = lang_names_language (language->code, english);
  g_free (english);
  if (upper != NULL)
    {
      gchar *place = lang_names_region (upper, upper);

      g_free (upper);
      upper = place;
    }
  if (upper != NULL)
    {
      gchar *with = accent != NULL && *accent != '\0' && strlen (accent) <= 40
                    && g_utf8_validate (accent, -1, NULL)
                    ? g_strdup_printf ("%s (%s, %s)", label, upper, accent)
                    : g_strdup_printf ("%s (%s)", label, upper);

      g_free (label);
      label = with;
    }

  installer_add_voice (in, voice, language->code, label, name, NULL, 0);
  g_free (label);
  g_free (upper);
}



static gboolean
rhv_load (Installer *in,
          JsonNode  *root,
          GError   **error)
{
  JsonArray *languages;
  guint      i, j;

  if (!JSON_NODE_HOLDS_OBJECT (root)
      || (languages = vi_json_array (json_node_get_object (root), "languages")) == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of voices is damaged"));
      return FALSE;
    }

  for (i = 0; i < json_array_get_length (languages); i++)
    {
      JsonNode    *node = json_array_get_element (languages, i);
      JsonObject  *object = JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
      RhvLanguage *language = object != NULL ? load_language (object) : NULL;
      JsonArray   *voices = language != NULL ? vi_json_array (object, "voices") : NULL;

      for (j = 0; voices != NULL && j < json_array_get_length (voices); j++)
        {
          JsonNode *voice = json_array_get_element (voices, j);

          if (JSON_NODE_HOLDS_OBJECT (voice))
            load_voice (in, language, json_node_get_object (voice));
        }
      language_unref (language);
    }

  return TRUE;
}



/* ------------------------------------------------------------- licences */

typedef struct
{
  Installer *in;
  gchar     *repo;
} ReadmeRequest;

static void rhv_licence (Installer *in, gpointer data);

static void
rhv_readme_done (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
  ReadmeRequest *request = data;
  Installer     *in = request->in;
  RhvData       *rhv = in->data;
  SoupMessage   *msg = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  GBytes        *bytes;
  gchar         *licence = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, NULL);
  if (bytes != NULL && installer_http_ok (msg, NULL)
      && g_bytes_get_size (bytes) <= README_MAX_SIZE)
    {
      gsize        length;
      const gchar *text = g_bytes_get_data (bytes, &length);

      licence = vi_licence_line (text, length);
    }
  if (bytes != NULL)
    g_bytes_unref (bytes);

  g_hash_table_replace (rhv->licences, request->repo,
                        licence != NULL ? licence : g_strdup (_("not stated")));
  if (!in->closed)
    rhv_licence (in, installer_selected (in));
  installer_unref (in);
  g_free (request);
}



/* From the README of the voice on GitHub, fetched once per voice */
static void
rhv_licence (Installer *in,
             gpointer   data)
{
  RhvVoice    *voice = data;
  RhvData     *rhv = in->data;
  const gchar *licence;
  gchar       *url;
  SoupMessage *msg;

  /* the voices on rhvoice.org have no README to look at */
  if (voice == NULL || voice->repo == NULL)
    {
      installer_set_licence (in, NULL);
      return;
    }

  licence = g_hash_table_lookup (rhv->licences, voice->repo);
  if (licence != NULL)
    {
      installer_set_licence (in, licence);
      return;
    }

  installer_set_licence (in, "…");
  g_hash_table_replace (rhv->licences, g_strdup (voice->repo), g_strdup ("…"));
  url = g_strdup_printf (README_URL, voice->repo);
  msg = soup_message_new (SOUP_METHOD_GET, url);
  if (msg != NULL)
    {
      ReadmeRequest *request = g_new (ReadmeRequest, 1);

      request->in = installer_ref (in);
      request->repo = g_strdup (voice->repo);
      soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                        in->cancellable, rhv_readme_done, request);
      g_object_unref (msg);
    }
  g_free (url);
}



/* -------------------------------------------------------------- install */

static void
rhv_end (RhvInstall *install,
         GError     *error)
{
  Installer *in = install->in;

  g_free (install->zip);
  g_free (install->unpacked);
  g_free (install);
  installer_finish (in, error);
}



/* @from in place of @to; an older @to goes into the unpack directory */
static gboolean
put_in_place (Installer   *in,
              const gchar *from,
              const gchar *to,
              GError     **error)
{
  gchar   *dir = g_path_get_dirname (to);
  gchar   *base = g_path_get_basename (to);
  gchar   *old = g_strconcat (in->unpack, "/old-", base, NULL);
  gboolean ok;

  ok = g_mkdir_with_parents (dir, 0755) == 0
       && (!g_file_test (to, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK)
           || g_rename (to, old) == 0)
       && g_rename (from, to) == 0;
  if (!ok)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s",
                 to, g_strerror (errno));
  g_free (old);
  g_free (base);
  g_free (dir);

  return ok;
}



static void rhv_download (RhvInstall *install,
                          gboolean    language);

static void
rhv_unzipped (GObject      *source,
              GAsyncResult *result,
              gpointer      data)
{
  RhvInstall *install = data;
  Installer  *in = install->in;
  GError     *error = NULL;
  gchar      *target;
  const gchar *info = install->language ? "language.info" : "voice.info";
  gint        major, minor;

  if (!vi_unzip_finish (result, &error))
    {
      rhv_end (install, error);
      g_error_free (error);
      return;
    }
  if (g_cancellable_set_error_if_cancelled (in->cancellable, &error))
    {
      rhv_end (install, error);
      g_error_free (error);
      return;
    }

  if (!read_version (install->unpacked, info, &major, &minor))
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("%s is missing in the archive"), info);
      rhv_end (install, error);
      g_error_free (error);
      return;
    }

  target = install->language
           ? user_path ("languages", install->voice->language->name)
           : user_path ("voices", install->voice->dir);
  if (!put_in_place (in, install->unpacked, target, &error))
    {
      g_free (target);
      rhv_end (install, error);
      g_error_free (error);
      return;
    }
  g_free (target);

  if (install->language)
    rhv_download (install, FALSE);
  else
    rhv_end (install, NULL);
}



static void
rhv_downloaded (Installer   *in,
                const gchar *md5,
                GError      *error,
                gpointer     data)
{
  RhvInstall  *install = data;
  const gchar *wanted = install->language ? install->voice->language->md5
                                          : install->voice->md5;
  GError      *local = NULL;

  if (error != NULL)
    {
      rhv_end (install, error);
      return;
    }
  if (md5 == NULL || g_ascii_strcasecmp (md5, wanted) != 0)
    {
      local = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   _("the download is damaged (wrong checksum)"));
      rhv_end (install, local);
      g_error_free (local);
      return;
    }

  installer_set_status (in, _("Unpacking…"));
  g_free (install->unpacked);
  install->unpacked = g_build_filename (in->unpack,
                                        install->language ? "language" : "voice", NULL);
  vi_unzip_async (install->zip, install->unpacked, in->cancellable, rhv_unzipped, install);
}



/* The archive of the language (when needed) or of the voice */
static void
rhv_download (RhvInstall *install,
              gboolean    language)
{
  Installer *in = install->in;
  RhvVoice  *voice = install->voice;
  gint       major, minor;

  /* the voice may need a newer language than the one installed */
  if (language && language_version (voice->language, FALSE, &major, &minor)
      && version_compare (major, minor, voice->language->major, voice->language->minor) >= 0)
    language = FALSE;

  install->language = language;
  g_free (install->zip);
  install->zip = g_build_filename (in->unpack, language ? "language.zip" : "voice.zip", NULL);
  if (language)
    installer_download (in, voice->language->url, install->zip, ZIP_MAX_SIZE,
                        0.0, 0.3, rhv_downloaded, install);
  else
    /* after a language, the voice gets the rest of the bar */
    installer_download (in, voice->url, install->zip, ZIP_MAX_SIZE,
                        install->unpacked != NULL ? 0.3 : 0.0,
                        install->unpacked != NULL ? 0.7 : 1.0, rhv_downloaded, install);
}



static void
rhv_download_start (RhvInstall *install)
{
  Installer *in = install->in;
  gchar     *base;
  GError    *error = NULL;

  if (!module_reads_user_dir ())
    {
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                   _("the distribution has no package for this voice, and "
                                     "its RHVoice cannot use voices installed for you"));
      rhv_end (install, error);
      g_error_free (error);
      return;
    }

  base = user_dir ();
  in->unpack = g_strconcat (base, "/.unpack-", install->voice->dir, NULL);
  g_free (base);
  vi_remove_tree (in->unpack);
  if (g_mkdir_with_parents (in->unpack, 0700) != 0)
    {
      error = g_error_new (G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s",
                           in->unpack, g_strerror (errno));
      rhv_end (install, error);
      g_error_free (error);
      return;
    }

  rhv_download (install, TRUE);
}



static void
rhv_package_done (GObject      *source,
                  GAsyncResult *result,
                  gpointer      data)
{
  RhvInstall     *install = data;
  GError         *error = NULL;
  ViPackageResult package = vi_package_install_finish (result, &error);

  if (error != NULL)
    {
      rhv_end (install, error);
      g_error_free (error);
      return;
    }

  /* the package may not have this voice yet */
  if (package == VI_PACKAGE_INSTALLED && system_voice_dir (install->voice))
    rhv_end (install, NULL);
  else
    rhv_download_start (install);
}



static void
rhv_install (Installer *in,
             gpointer   data)
{
  RhvVoice   *voice = data;
  RhvInstall *install = g_new0 (RhvInstall, 1);
  gint        major, minor;

  install->in = in;
  install->voice = voice;

  /* the language of the system has its package already */
  if (!language_version (voice->language, TRUE, &major, &minor))
    {
      gchar *package = g_strconcat ("rhvoice-", voice->language->id, NULL);
      gchar *status;

      g_strdelimit (package, "_", '-');
      status = g_strdup_printf (_("Looking for the package %s…"), package);
      installer_set_status (in, status);
      vi_package_install_async (package, in->cancellable, rhv_package_done, install);
      g_free (status);
      g_free (package);
    }
  else
    rhv_download_start (install);
}



/* The language of the user goes with its last voice of the user */
static gboolean
language_has_user_voices (RhvLanguage *language)
{
  gchar       *base = user_path ("voices", NULL);
  GDir        *dir = g_dir_open (base, 0, NULL);
  const gchar *name;
  gboolean     found = FALSE;

  while (!found && dir != NULL && (name = g_dir_read_name (dir)) != NULL)
    {
      gchar  *path, *text = NULL;
      gchar **lines;
      guint   i;

      if (*name == '.')
        continue;
      path = g_build_filename (base, name, "voice.info", NULL);
      if (g_file_get_contents (path, &text, NULL, NULL))
        {
          lines = g_strsplit (text, "\n", -1);
          for (i = 0; lines[i] != NULL; i++)
            if (g_str_has_prefix (lines[i], "language=")
                && g_ascii_strcasecmp (g_strstrip (lines[i] + 9), language->name) == 0)
              found = TRUE;
          g_strfreev (lines);
        }
      g_free (text);
      g_free (path);
    }
  if (dir != NULL)
    g_dir_close (dir);
  g_free (base);

  return found;
}



static gboolean
rhv_remove (Installer *in,
            gpointer   data,
            GError   **error)
{
  RhvVoice *voice = data;
  gchar    *path = user_path ("voices", voice->dir);
  gboolean  ok = vi_remove_tree (path);

  if (!ok)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s", g_strerror (errno));
  g_free (path);

  if (ok && !language_has_user_voices (voice->language))
    {
      path = user_path ("languages", voice->language->name);
      vi_remove_tree (path);
      g_free (path);
    }

  return ok;
}



const VoiceSource voice_source_rhvoice =
{
  .module          = "rhvoice",
  .module_path     = MODULE_PATH,
  .title           = N_("RHVoice Voices"),
  .install_tooltip = N_("Install the voice from the distribution or from the "
                        "RHVoice voice collection"),
  .quality_title   = NULL,
  .no_size         = TRUE,
  .catalogue_url   = "https://rhvoice.org/download/packages-1.14.json",
  .cache_name      = "rhvoice-voices.json",
  .new_data        = rhv_new_data,
  .free_data       = rhv_free_data,
  .free_voice      = rhv_free_voice,
  .load            = rhv_load,
  .state           = rhv_state,
  .install         = rhv_install,
  .remove          = rhv_remove,
  .licence         = rhv_licence,
};
