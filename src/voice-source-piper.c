/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
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
 * Piper voices for the synthesizer sherpa-onnx, installed into
 * ~/.local/share/sherpa-onnx/tts, where sd_sherpa-onnx finds them.
 *
 * The list of voices is voices.json of the Piper voice collection
 * (rhasspy/piper-voices on Hugging Face); the licence of a voice is the
 * "License:" line of its MODEL_CARD there. The voice itself (<key>.onnx and
 * <key>.onnx.json) comes from there too, checked against the MD5 sums of
 * voices.json, and is made into the form sherpa-onnx reads the way its
 * scripts/piper/add_meta_data.py does: tokens.txt from the phoneme map, the
 * metadata appended to the model, espeak-ng-data a link to the shared copy
 * of the piper-espeak-ng-data package.
 *
 * Everything is written into a hidden directory next to the voices and only
 * renamed into place when whole, so a half-installed voice is never seen by
 * the synthesizer.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>
#ifdef HAVE_XFCE
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#include "voice-installer-private.h"

#define COLLECTION_URL "https://huggingface.co/rhasspy/piper-voices/resolve/main/"
#define SYSTEM_DIR "/usr/share/sherpa-onnx/tts"
#define SHARED_ESPEAK_DATA "/usr/share/sherpa-onnx/piper-espeak-ng-data"

#define CARD_MAX_SIZE (64 * 1024)
#define CONFIG_MAX_SIZE (1024 * 1024)
#define VOICE_MAX_SIZE ((goffset) 1024 * 1024 * 1024)

typedef struct
{
  gchar *key;
  gchar *card;        /* path of the MODEL_CARD in the collection */
  gchar *onnx;        /* path of the model in the collection */
  gchar *onnx_md5;
  gchar *config;      /* path of its .onnx.json */
  gchar *config_md5;
} PiperVoice;

typedef struct
{
  GHashTable *licences;   /* MODEL_CARD path -> licence */
} PiperData;

/* one installation */
typedef struct
{
  Installer  *in;
  PiperVoice *voice;
  gchar      *dir;        /* the voice in the hidden directory */
  gchar      *onnx_path;
  GByteArray *meta;       /* to append to the model */
} PiperInstall;



/* "ru_RU-dmitri-medium": letters, digits and "_" in three parts. Keys come
 * from the network and end up in file names and URLs. */
gboolean
voice_installer_valid_key (const gchar *key)
{
  return key != NULL
         && strlen (key) < 100
         && g_regex_match_simple ("^[a-zA-Z]{2,3}_[a-zA-Z]{2}-[a-zA-Z0-9_]+-(x_low|low|medium|high)$",
                                  key, G_REGEX_DOLLAR_ENDONLY, 0);
}



gchar *
voice_installer_user_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "sherpa-onnx", "tts", NULL);
}



static gchar *
voice_dir_name (const gchar *key)
{
  return g_strconcat ("vits-piper-", key, NULL);
}



static gpointer
piper_new_data (void)
{
  PiperData *data = g_new0 (PiperData, 1);

  data->licences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return data;
}



static void
piper_free_data (gpointer data)
{
  PiperData *piper = data;

  g_hash_table_unref (piper->licences);
  g_free (piper);
}



static void
piper_free_voice (gpointer data)
{
  PiperVoice *voice = data;

  g_free (voice->key);
  g_free (voice->card);
  g_free (voice->onnx);
  g_free (voice->onnx_md5);
  g_free (voice->config);
  g_free (voice->config_md5);
  g_free (voice);
}



static VoiceState
piper_state (Installer *in,
             gpointer   data)
{
  PiperVoice *voice = data;
  gchar      *name = voice_dir_name (voice->key);
  gchar      *user_dir = voice_installer_user_dir ();
  gchar      *path = g_build_filename (user_dir, name, NULL);
  VoiceState  state = STATE_NONE;

  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    state = STATE_USER;
  else
    {
      g_free (path);
      path = g_build_filename (SYSTEM_DIR, name, NULL);
      if (g_file_test (path, G_FILE_TEST_IS_DIR))
        state = STATE_SYSTEM;
    }

  g_free (path);
  g_free (user_dir);
  g_free (name);

  return state;
}



/* ------------------------------------------------------------- licences */

typedef struct
{
  Installer *in;
  gchar     *card;
} CardRequest;

static void piper_licence (Installer *in, gpointer data);

static void
piper_card_done (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
  CardRequest *request = data;
  Installer   *in = request->in;
  PiperData   *piper = in->data;
  GBytes      *bytes;
  GError      *error = NULL;
  gchar       *licence = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes != NULL && g_bytes_get_size (bytes) <= CARD_MAX_SIZE)
    {
      gsize        length;
      const gchar *card = g_bytes_get_data (bytes, &length);

      licence = vi_licence_line (card, length);
    }
  if (bytes != NULL)
    g_bytes_unref (bytes);
  g_clear_error (&error);

  /* not found also counts: it is not asked for again */
  g_hash_table_replace (piper->licences, request->card,
                        licence != NULL ? licence : g_strdup (_("not stated")));
  if (!in->closed)
    piper_licence (in, installer_selected (in));
  installer_unref (in);
  g_free (request);
}



/* The licence of the selected voice, fetched once per voice */
static void
piper_licence (Installer *in,
               gpointer   data)
{
  PiperVoice  *voice = data;
  PiperData   *piper = in->data;
  const gchar *licence;

  if (voice == NULL)
    {
      installer_set_licence (in, NULL);
      return;
    }
  if (voice->card == NULL)
    {
      installer_set_licence (in, _("not stated"));
      return;
    }

  licence = g_hash_table_lookup (piper->licences, voice->card);
  if (licence != NULL)
    installer_set_licence (in, licence);
  else
    {
      gchar       *url = g_strconcat (COLLECTION_URL, voice->card, NULL);
      SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);

      installer_set_licence (in, "…");
      /* asked for, the answer is on its way */
      g_hash_table_replace (piper->licences, g_strdup (voice->card), g_strdup ("…"));
      if (msg != NULL)
        {
          CardRequest *request = g_new (CardRequest, 1);

          request->in = installer_ref (in);
          request->card = g_strdup (voice->card);
          soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                            in->cancellable, piper_card_done,
                                            request);
          g_object_unref (msg);
        }
      g_free (url);
    }
}



/* ------------------------------------------------------------ catalogue */

/* "ru/ru_RU/dmitri/medium/MODEL_CARD": a relative path without ".." that
 * ends in @suffix */
static gboolean
valid_file_path (const gchar *path,
                 const gchar *suffix)
{
  return g_regex_match_simple ("^[a-zA-Z0-9_]+(/[a-zA-Z0-9_][a-zA-Z0-9_.-]*)*$",
                               path, G_REGEX_DOLLAR_ENDONLY, 0)
         && strstr (path, "..") == NULL
         && g_str_has_suffix (path, suffix);
}



/* the "md5_digest" of a file of voices.json if it looks like one */
static const gchar *
file_md5 (JsonObject *file)
{
  const gchar *md5 = vi_json_string (file, "md5_digest");

  return md5 != NULL && g_regex_match_simple ("^[0-9a-fA-F]{32}$", md5,
                                              G_REGEX_DOLLAR_ENDONLY, 0) ? md5 : NULL;
}



/* One voice of voices.json into the list; FALSE if it is not usable */
static gboolean
piper_add_voice (Installer   *in,
                 const gchar *key,
                 JsonObject  *object)
{
  JsonObject  *language = vi_json_object (object, "language");
  JsonObject  *files = vi_json_object (object, "files");
  const gchar *name = vi_json_string (object, "name");
  const gchar *quality = vi_json_string (object, "quality");
  const gchar *english = vi_json_string (language, "name_english");
  const gchar *country = vi_json_string (language, "country_english");
  const gchar *card = NULL, *onnx = NULL, *onnx_md5 = NULL;
  const gchar *config = NULL, *config_md5 = NULL;
  gchar       *family, *label, *onnx_name, *config_name;
  gint64       bytes = 0;
  GList       *members, *l;
  PiperVoice  *voice;

  if (!voice_installer_valid_key (key) || name == NULL || quality == NULL
      || english == NULL || files == NULL)
    return FALSE;

  onnx_name = g_strdup_printf ("/%s.onnx", key);
  config_name = g_strdup_printf ("/%s.onnx.json", key);
  members = json_object_get_members (files);
  for (l = members; l != NULL; l = l->next)
    {
      const gchar *path = l->data;
      JsonObject  *file = vi_json_object (files, path);

      if (valid_file_path (path, onnx_name) && file_md5 (file) != NULL)
        {
          onnx = path;
          onnx_md5 = file_md5 (file);
          bytes = vi_json_int (file, "size_bytes", 0);
        }
      else if (valid_file_path (path, config_name) && file_md5 (file) != NULL)
        {
          config = path;
          config_md5 = file_md5 (file);
        }
      else if (valid_file_path (path, "/MODEL_CARD"))
        card = path;
    }
  g_free (onnx_name);
  g_free (config_name);
  if (onnx == NULL || config == NULL)
    {
      g_list_free (members);
      return FALSE;
    }

  voice = g_new0 (PiperVoice, 1);
  voice->key = g_strdup (key);
  voice->card = g_strdup (card);
  voice->onnx = g_strdup (onnx);
  voice->onnx_md5 = g_strdup (onnx_md5);
  voice->config = g_strdup (config);
  voice->config_md5 = g_strdup (config_md5);
  g_list_free (members);

  family = g_strndup (key, strcspn (key, "_"));
  label = country != NULL ? g_strdup_printf ("%s (%s)", english, country)
                          : g_strdup (english);
  installer_add_voice (in, voice, family, label, name, quality, bytes);
  g_free (family);
  g_free (label);

  return TRUE;
}



static gboolean
piper_load (Installer  *in,
            JsonNode   *root,
            GError    **error)
{
  JsonObject *object;
  GList      *keys, *l;
  guint       n = 0;

  if (!JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of voices is damaged"));
      return FALSE;
    }

  object = json_node_get_object (root);
  keys = json_object_get_members (object);
  for (l = keys; l != NULL; l = l->next)
    if (piper_add_voice (in, l->data, vi_json_object (object, l->data)))
      n++;
  g_list_free (keys);

  return n > 0;
}



/* --------------------------------------------------------------- install */

static void
piper_install_free (PiperInstall *install)
{
  g_free (install->dir);
  g_free (install->onnx_path);
  if (install->meta != NULL)
    g_byte_array_unref (install->meta);
  g_free (install);
}



static void
piper_end (Installer    *in,
           PiperInstall *install,
           GError       *error)
{
  piper_install_free (install);
  installer_finish (in, error);
}



/* The downloaded voice made whole and moved into place */
static gboolean
piper_put_in_place (Installer    *in,
                    PiperInstall *install,
                    GError      **error)
{
  gchar   *name = voice_dir_name (install->voice->key);
  gchar   *user_dir = voice_installer_user_dir ();
  gchar   *dest = g_build_filename (user_dir, name, NULL);
  gchar   *espeak = g_build_filename (install->dir, "espeak-ng-data", NULL);
  gboolean ok = FALSE;

  if (!g_file_test (SHARED_ESPEAK_DATA, G_FILE_TEST_IS_DIR))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("the espeak-ng data of Piper is missing; install "
                           "the package piper-espeak-ng-data"));
  else if (symlink (SHARED_ESPEAK_DATA, espeak) != 0)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s: %s", espeak, g_strerror (errno));
  else if (g_file_test (dest, G_FILE_TEST_EXISTS))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                         _("the voice is installed already"));
  else if (g_rename (install->dir, dest) != 0)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s: %s", dest, g_strerror (errno));
  else
    ok = TRUE;

  g_free (espeak);
  g_free (dest);
  g_free (user_dir);
  g_free (name);

  return ok;
}



/* The whole model is there: checked, its metadata appended, put in place */
static void
piper_model_done (Installer   *in,
                  const gchar *md5,
                  GError      *error,
                  gpointer     data)
{
  PiperInstall  *install = data;
  GError        *local = NULL;
  GFile         *file;
  GOutputStream *out;

  if (md5 == NULL)
    {
      piper_end (in, install, error);
      return;
    }

  if (g_ascii_strcasecmp (md5, install->voice->onnx_md5) != 0)
    g_set_error_literal (&local, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("the download is damaged (wrong checksum)"));
  else
    {
      file = g_file_new_for_path (install->onnx_path);
      out = G_OUTPUT_STREAM (g_file_append_to (file, G_FILE_CREATE_NONE,
                                               in->cancellable, &local));
      g_object_unref (file);
      if (out != NULL)
        {
          if (g_output_stream_write_all (out, install->meta->data, install->meta->len,
                                         NULL, in->cancellable, &local)
              && g_output_stream_close (out, in->cancellable, &local))
            piper_put_in_place (in, install, &local);
          g_object_unref (out);
        }
    }

  piper_end (in, install, local);
  g_clear_error (&local);
}



/* protobuf, as far as ONNX metadata needs it: a length-delimited field */
static void
pb_varint (GByteArray *b,
           guint64     n)
{
  do
    {
      guint8 c = n & 0x7f;

      n >>= 7;
      if (n != 0)
        c |= 0x80;
      g_byte_array_append (b, &c, 1);
    }
  while (n != 0);
}

static void
pb_bytes (GByteArray   *b,
          guint         field,
          const guint8 *data,
          gsize         len)
{
  pb_varint (b, field << 3 | 2);
  pb_varint (b, len);
  g_byte_array_append (b, data, len);
}

/* ModelProto.metadata_props (14) = StringStringEntryProto {key = 1, value = 2}.
 * Repeated fields may come in any order, so appended to the end of the model
 * file they add to what is there. */
static void
onnx_add_meta (GByteArray  *b,
               const gchar *key,
               const gchar *value)
{
  GByteArray *entry = g_byte_array_new ();

  pb_bytes (entry, 1, (const guint8 *) key, strlen (key));
  pb_bytes (entry, 2, (const guint8 *) value, strlen (value));
  pb_bytes (b, 14, entry->data, entry->len);
  g_byte_array_unref (entry);
}



/* From the voice's .onnx.json, what sherpa-onnx needs besides the model, as
 * sherpa-onnx's scripts/piper/add_meta_data.py makes it: tokens.txt from the
 * phoneme_id_map and the metadata to append to the model */
static gboolean
piper_convert_config (PiperInstall *install,
                      GBytes       *config,
                      GError      **error)
{
  JsonParser  *parser = json_parser_new ();
  JsonObject  *root, *ids;
  const gchar *type, *espeak, *language;
  gint64       rate, speakers;
  GString     *tokens = NULL;
  GList       *members = NULL;
  gchar       *path, *value;
  gboolean     ok = FALSE;

  if (!json_parser_load_from_data (parser, g_bytes_get_data (config, NULL),
                                   g_bytes_get_size (config), NULL)
      || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    goto bad;
  root = json_node_get_object (json_parser_get_root (parser));
  ids = vi_json_object (root, "phoneme_id_map");
  type = vi_json_string (root, "phoneme_type");
  espeak = vi_json_string (vi_json_object (root, "espeak"), "voice");
  language = vi_json_string (vi_json_object (root, "language"), "name_english");
  rate = vi_json_int (vi_json_object (root, "audio"), "sample_rate", 0);
  speakers = vi_json_int (root, "num_speakers", 1);

  /* "text": letters, read by sd_sherpa as they are; "pinyin" etc.: not yet */
  if (type != NULL && strcmp (type, "espeak") != 0 && strcmp (type, "text") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   _("this voice uses \"%s\" phonemes, which are not supported yet"),
                   type);
      goto out;
    }
  if (ids == NULL || espeak == NULL || rate <= 0 || speakers <= 0)
    goto bad;
  if (rate == 22500)  /* a few voices say so, add_meta_data.py corrects it */
    rate = 22050;

  tokens = g_string_new (NULL);
  members = json_object_get_members (ids);
  for (GList *l = members; l != NULL; l = l->next)
    {
      const gchar *symbol = l->data;
      JsonNode    *node = json_object_get_member (ids, symbol);
      gint64       id = -1;

      if (strcmp (symbol, "\n") == 0)
        continue;
      if (JSON_NODE_HOLDS_ARRAY (node) && json_array_get_length (json_node_get_array (node)) > 0)
        node = json_array_get_element (json_node_get_array (node), 0);
      if (JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_INT64)
        id = json_node_get_int (node);
      if (id < 0 || strchr (symbol, '\n') != NULL || g_utf8_strlen (symbol, -1) != 1)
        goto bad;
      g_string_append_printf (tokens, "%s %" G_GINT64_FORMAT "\n", symbol, id);
    }
  if (tokens->len == 0)
    goto bad;

  path = g_build_filename (install->dir, "tokens.txt", NULL);
  ok = g_file_set_contents (path, tokens->str, tokens->len, error);
  g_free (path);
  if (!ok)
    goto out;
  value = g_strdup_printf ("%s.onnx.json", install->voice->key);
  path = g_build_filename (install->dir, value, NULL);
  ok = g_file_set_contents (path, g_bytes_get_data (config, NULL),
                            g_bytes_get_size (config), error);
  g_free (path);
  g_free (value);
  if (!ok)
    goto out;

  install->meta = g_byte_array_new ();
  onnx_add_meta (install->meta, "model_type", "vits");
  onnx_add_meta (install->meta, "comment", "piper");
  onnx_add_meta (install->meta, "language", language != NULL ? language : "");
  onnx_add_meta (install->meta, "voice", espeak);
  onnx_add_meta (install->meta, "version", "1");
  onnx_add_meta (install->meta, "has_espeak", "1");
  value = g_strdup_printf ("%" G_GINT64_FORMAT, speakers);
  onnx_add_meta (install->meta, "n_speakers", value);
  g_free (value);
  value = g_strdup_printf ("%" G_GINT64_FORMAT, rate);
  onnx_add_meta (install->meta, "sample_rate", value);
  g_free (value);
  goto out;

bad:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       _("the settings of the voice cannot be read"));
  ok = FALSE;
out:
  if (tokens != NULL)
    g_string_free (tokens, TRUE);
  g_list_free (members);
  g_object_unref (parser);
  return ok;
}



/* The .onnx.json is here: converted, then the model downloaded */
static void
piper_config_done (GObject      *source,
                   GAsyncResult *result,
                   gpointer      data)
{
  PiperInstall *install = data;
  Installer    *in = install->in;
  SoupMessage  *msg = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  GError       *error = NULL;
  GBytes       *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  gchar        *md5 = NULL, *url, *name;

  if (bytes == NULL || !installer_http_ok (msg, &error))
    goto failed;
  if (g_bytes_get_size (bytes) > CONFIG_MAX_SIZE)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("the download is too big"));
      goto failed;
    }
  md5 = g_compute_checksum_for_bytes (G_CHECKSUM_MD5, bytes);
  if (g_ascii_strcasecmp (md5, install->voice->config_md5) != 0)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("the download is damaged (wrong checksum)"));
      goto failed;
    }
  if (!piper_convert_config (install, bytes, &error))
    goto failed;

  name = g_strdup_printf ("%s.onnx", install->voice->key);
  install->onnx_path = g_build_filename (install->dir, name, NULL);
  g_free (name);
  url = g_strconcat (COLLECTION_URL, install->voice->onnx, NULL);
  installer_download (in, url, install->onnx_path, VOICE_MAX_SIZE, 0.0, 1.0,
                      piper_model_done, install);
  g_free (url);
  g_free (md5);
  g_bytes_unref (bytes);
  return;

failed:
  piper_end (in, install, error);
  g_error_free (error);
  g_free (md5);
  if (bytes != NULL)
    g_bytes_unref (bytes);
}



static void
piper_install (Installer *in,
               gpointer   data)
{
  PiperVoice   *voice = data;
  PiperInstall *install;
  gchar        *user_dir, *name, *url;
  SoupMessage  *msg = NULL;
  GError       *error = NULL;

  install = g_new0 (PiperInstall, 1);
  install->in = in;
  install->voice = voice;

  /* everything goes to a hidden directory first, renamed into place when whole */
  user_dir = voice_installer_user_dir ();
  name = g_strdup_printf (".unpack-%s", voice->key);
  in->unpack = g_build_filename (user_dir, name, NULL);
  g_free (name);
  name = voice_dir_name (voice->key);
  install->dir = g_build_filename (in->unpack, name, NULL);
  g_free (name);
  g_free (user_dir);
  vi_remove_tree (in->unpack);
  if (g_mkdir_with_parents (install->dir, 0755) != 0)
    g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s: %s", install->dir, g_strerror (errno));
  else
    {
      url = g_strconcat (COLLECTION_URL, voice->config, NULL);
      msg = soup_message_new (SOUP_METHOD_GET, url);
      g_free (url);
      if (msg == NULL)
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, voice->config);
    }

  if (error != NULL)
    {
      piper_end (in, install, error);
      g_error_free (error);
      return;
    }

  installer_set_status (in, _("Connecting…"));
  soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                    in->cancellable, piper_config_done, install);
  g_object_unref (msg);
}



static gboolean
piper_remove (Installer *in,
              gpointer   data,
              GError   **error)
{
  PiperVoice *voice = data;
  gchar      *user_dir = voice_installer_user_dir ();
  gchar      *name = voice_dir_name (voice->key);
  gchar      *path = g_build_filename (user_dir, name, NULL);
  gboolean    ok = vi_remove_tree (path);

  if (!ok)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s", g_strerror (errno));
  g_free (path);
  g_free (name);
  g_free (user_dir);

  return ok;
}



const VoiceSource voice_source_piper =
{
  .module          = "sherpa-onnx",
  .module_path     = "/usr/lib/speech-dispatcher-modules/sd_sherpa-onnx",
  .title           = N_("Piper Voices"),
  .install_tooltip = N_("Download the voice from the Piper voice collection "
                        "and install it for you"),
  .quality_title   = N_("Quality"),
  .catalogue_url   = "https://huggingface.co/rhasspy/piper-voices/resolve/main/voices.json",
  .cache_name      = "piper-voices.json",
  .new_data        = piper_new_data,
  .free_data       = piper_free_data,
  .free_voice      = piper_free_voice,
  .load            = piper_load,
  .state           = piper_state,
  .install         = piper_install,
  .remove          = piper_remove,
  .licence         = piper_licence,
};
