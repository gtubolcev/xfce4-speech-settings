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
 * Installs Piper voices for the synthesizer sherpa-onnx into
 * ~/.local/share/sherpa-onnx/tts, where sd_sherpa-onnx finds them.
 *
 * The list of voices is voices.json of the Piper voice collection
 * (rhasspy/piper-voices on Hugging Face), kept a week in ~/.cache; the
 * licence of a voice is the "License:" line of its MODEL_CARD there. The
 * voice itself comes from the sherpa-onnx release "tts-models", where every
 * Piper voice is converted to the form sherpa-onnx reads
 * (vits-piper-<key>.tar.bz2: the model, tokens.txt, espeak-ng-data).
 *
 * A download goes to a hidden file next to the voices, is unpacked with tar
 * into a hidden directory, checked, and only then renamed into place, so a
 * half-installed voice is never seen by the synthesizer. The espeak-ng data
 * of a voice is replaced by a link to the shared copy of the
 * piper-espeak-ng-data package when that is installed (19 MB less).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#ifdef HAVE_XFCE
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#endif
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "voice-installer.h"

#define CATALOGUE_URL "https://huggingface.co/rhasspy/piper-voices/resolve/main/voices.json"
#define CARD_URL "https://huggingface.co/rhasspy/piper-voices/resolve/main/"
#define VOICE_URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-%s.tar.bz2"
#define SYSTEM_DIR "/usr/share/sherpa-onnx/tts"
#define SHARED_ESPEAK_DATA "/usr/share/sherpa-onnx/piper-espeak-ng-data"

/* the list is fetched again after a week */
#define CATALOGUE_MAX_AGE (7 * G_TIME_SPAN_DAY)
/* nothing bigger is accepted; the largest voice is about 120 MB */
#define CATALOGUE_MAX_SIZE (16 * 1024 * 1024)
#define CARD_MAX_SIZE (64 * 1024)
#define VOICE_MAX_SIZE ((goffset) 1024 * 1024 * 1024)
#define CHUNK_SIZE (64 * 1024)

enum
{
  COL_KEY,
  COL_FAMILY,      /* "ru": the language without the region */
  COL_LANGUAGE,    /* "Russian (Russia)" */
  COL_NAME,
  COL_QUALITY,
  COL_SIZE,
  COL_STATE,
  COL_STATE_TEXT,
  COL_CARD,        /* path of the MODEL_CARD in the collection */
  N_COLS
};

typedef enum
{
  STATE_NONE,
  STATE_USER,      /* in ~/.local/share/sherpa-onnx/tts: can be removed */
  STATE_SYSTEM     /* from a package */
} VoiceState;

typedef struct
{
  /* the dialog is gone, but an operation still holds a reference */
  gboolean               closed;
  gint                   refs;

  GtkWidget             *dialog;
  GtkWidget             *language_combo;
  GtkWidget             *view;
  GtkWidget             *install_button;
  GtkWidget             *remove_button;
  GtkWidget             *progress;
  GtkWidget             *status_label;
  GtkWidget             *licence_label;
  GtkListStore          *store;
  GtkTreeModel          *filter;

  SoupSession           *session;
  GCancellable          *cancellable;
  gchar                 *language;
  GHashTable            *licences;   /* MODEL_CARD path -> licence */
  VoiceInstallerChanged  changed;
  gpointer               changed_data;

  /* the voice being installed */
  gboolean               busy;
  gchar                 *key;
  gchar                 *download;   /* the .tar.bz2 being written */
  gchar                 *unpack;     /* the directory tar unpacks into */
  GInputStream          *in;
  GOutputStream         *out;
  goffset                total;
  goffset                done;
  GSubprocess           *tar;
} Installer;



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



static VoiceState
voice_state (const gchar *key)
{
  gchar     *name = voice_dir_name (key);
  gchar     *user_dir = voice_installer_user_dir ();
  gchar     *path = g_build_filename (user_dir, name, NULL);
  VoiceState state = STATE_NONE;

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



static const gchar *
state_text (VoiceState state)
{
  switch (state)
    {
    case STATE_USER:
      return _("Installed");
    case STATE_SYSTEM:
      return _("Installed with the system");
    default:
      return "";
    }
}



/* Removes @path and everything below it; links are removed, not followed */
static gboolean
remove_tree (const gchar *path)
{
  GStatBuf st;

  if (g_lstat (path, &st) != 0)
    return errno == ENOENT;

  if (S_ISDIR (st.st_mode))
    {
      GDir        *dir = g_dir_open (path, 0, NULL);
      const gchar *name;

      while (dir != NULL && (name = g_dir_read_name (dir)) != NULL)
        {
          gchar *child = g_build_filename (path, name, NULL);

          remove_tree (child);
          g_free (child);
        }
      if (dir != NULL)
        g_dir_close (dir);

      return g_rmdir (path) == 0;
    }

  return g_unlink (path) == 0;
}



/* TRUE if something below @path is a link or not a plain file or directory:
 * a voice from the network must not point anywhere else */
static gboolean
has_odd_files (const gchar *path)
{
  GStatBuf     st;
  GDir        *dir;
  const gchar *name;
  gboolean     odd = FALSE;

  if (g_lstat (path, &st) != 0)
    return TRUE;
  if (S_ISREG (st.st_mode))
    return FALSE;
  if (!S_ISDIR (st.st_mode))
    return TRUE;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL)
    return TRUE;
  while (!odd && (name = g_dir_read_name (dir)) != NULL)
    {
      gchar *child = g_build_filename (path, name, NULL);

      odd = has_odd_files (child);
      g_free (child);
    }
  g_dir_close (dir);

  return odd;
}



static Installer *
installer_ref (Installer *in)
{
  in->refs++;
  return in;
}



static void
installer_unref (Installer *in)
{
  if (--in->refs > 0)
    return;

  g_clear_object (&in->session);
  g_clear_object (&in->cancellable);
  g_clear_object (&in->store);
  g_clear_object (&in->filter);
  g_hash_table_unref (in->licences);
  g_free (in->language);
  g_free (in);
}



static void
installer_set_status (Installer   *in,
                      const gchar *text)
{
  if (!in->closed)
    gtk_label_set_text (GTK_LABEL (in->status_label), text != NULL ? text : "");
}



/* The selected row of the store, or FALSE */
static gboolean
installer_get_selected (Installer   *in,
                        GtkTreeIter *iter)
{
  GtkTreeSelection *selection;
  GtkTreeIter       filter_iter;

  if (in->closed)
    return FALSE;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (in->view));
  if (!gtk_tree_selection_get_selected (selection, NULL, &filter_iter))
    return FALSE;

  gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (in->filter),
                                                    iter, &filter_iter);
  return TRUE;
}



static gboolean
installer_find_key (Installer   *in,
                    const gchar *key,
                    GtkTreeIter *iter)
{
  GtkTreeModel *model = GTK_TREE_MODEL (in->store);
  gboolean      valid;

  for (valid = gtk_tree_model_get_iter_first (model, iter); valid;
       valid = gtk_tree_model_iter_next (model, iter))
    {
      gchar   *k;
      gboolean found;

      gtk_tree_model_get (model, iter, COL_KEY, &k, -1);
      found = g_strcmp0 (k, key) == 0;
      g_free (k);
      if (found)
        return TRUE;
    }

  return FALSE;
}



static void
installer_update_row (Installer   *in,
                      const gchar *key)
{
  GtkTreeIter iter;
  VoiceState  state = voice_state (key);

  if (installer_find_key (in, key, &iter))
    gtk_list_store_set (in->store, &iter, COL_STATE, state,
                        COL_STATE_TEXT, state_text (state), -1);
}



static void installer_show_licence (Installer *in);

static void
installer_update_buttons (Installer *in)
{
  GtkTreeIter iter;
  gint        state = -1;

  if (in->closed)
    return;

  if (installer_get_selected (in, &iter))
    gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_STATE, &state, -1);

  gtk_widget_set_sensitive (in->install_button, !in->busy && state == STATE_NONE);
  gtk_widget_set_sensitive (in->remove_button, !in->busy && state == STATE_USER);
  gtk_widget_set_sensitive (in->language_combo, !in->busy);
}



/* ------------------------------------------------------------- licences */

/* "* License: CC0" -> "CC0": the first "License:" line of a MODEL_CARD */
static gchar *
licence_from_card (const gchar *card,
                   gsize        length)
{
  gchar  *text = g_strndup (card, length);
  gchar **lines;
  gchar  *licence = NULL;
  guint   i;

  if (!g_utf8_validate (text, -1, NULL))
    {
      g_free (text);
      return NULL;
    }

  lines = g_strsplit (text, "\n", -1);
  for (i = 0; licence == NULL && lines[i] != NULL; i++)
    {
      gchar *lower = g_utf8_strdown (lines[i], -1);
      gchar *at = strstr (lower, "license:");

      if (at != NULL)
        {
          licence = g_strstrip (g_strdup (lines[i] + (at - lower) + strlen ("license:")));
          if (g_utf8_strlen (licence, -1) > 120)
            {
              gchar *cut = g_utf8_substring (licence, 0, 120);

              g_free (licence);
              licence = cut;
            }
        }
      g_free (lower);
    }

  g_strfreev (lines);
  g_free (text);

  return licence;
}



typedef struct
{
  Installer *in;
  gchar     *card;
} CardRequest;

static void
installer_card_done (GObject      *source,
                     GAsyncResult *result,
                     gpointer      data)
{
  CardRequest *request = data;
  Installer   *in = request->in;
  gchar       *path = request->card;
  GBytes      *bytes;
  GError    *error = NULL;
  gchar     *licence = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes != NULL && g_bytes_get_size (bytes) <= CARD_MAX_SIZE)
    {
      gsize         length;
      const gchar  *card = g_bytes_get_data (bytes, &length);

      licence = licence_from_card (card, length);
    }
  if (bytes != NULL)
    g_bytes_unref (bytes);
  g_clear_error (&error);

  /* not found also counts: it is not asked for again */
  g_hash_table_replace (in->licences, path,
                        licence != NULL ? licence : g_strdup (_("not stated")));
  installer_show_licence (in);
  installer_unref (in);
  g_free (request);
}



/* The licence of the selected voice, fetched once per voice */
static void
installer_show_licence (Installer *in)
{
  GtkTreeIter  iter;
  gchar       *card = NULL;
  const gchar *licence;

  if (in->closed)
    return;

  if (!installer_get_selected (in, &iter))
    {
      gtk_label_set_text (GTK_LABEL (in->licence_label), "");
      return;
    }

  gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_CARD, &card, -1);
  if (card == NULL)
    {
      gtk_label_set_text (GTK_LABEL (in->licence_label), _("Licence: not stated"));
      return;
    }

  licence = g_hash_table_lookup (in->licences, card);
  if (licence != NULL)
    {
      gchar *text = g_strdup_printf (_("Licence: %s"), licence);

      gtk_label_set_text (GTK_LABEL (in->licence_label), text);
      g_free (text);
      g_free (card);
    }
  else
    {
      gchar       *url = g_strconcat (CARD_URL, card, NULL);
      SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);

      gtk_label_set_text (GTK_LABEL (in->licence_label), _("Licence: …"));
      /* an empty entry: asked for, the answer is on its way */
      g_hash_table_replace (in->licences, g_strdup (card), g_strdup ("…"));
      if (msg != NULL)
        {
          CardRequest *request = g_new (CardRequest, 1);

          request->in = installer_ref (in);
          request->card = card;
          soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                            in->cancellable, installer_card_done,
                                            request);
          g_object_unref (msg);
        }
      else
        g_free (card);
      g_free (url);
    }
}



/* ------------------------------------------------------------ catalogue */

static const gchar *
json_string (JsonObject  *object,
             const gchar *member)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node)
      || json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return json_node_get_string (node);
}



static JsonObject *
json_object (JsonObject  *object,
             const gchar *member)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  return node != NULL && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}



/* "ru/ru_RU/dmitri/medium/MODEL_CARD": a relative path without ".." */
static gboolean
valid_card_path (const gchar *path)
{
  return g_regex_match_simple ("^[a-zA-Z0-9_]+(/[a-zA-Z0-9_][a-zA-Z0-9_.-]*)*/MODEL_CARD$",
                               path, G_REGEX_DOLLAR_ENDONLY, 0)
         && strstr (path, "..") == NULL;
}



/* One voice of voices.json into the store; FALSE if it is not usable */
static gboolean
installer_add_voice (Installer   *in,
                     const gchar *key,
                     JsonObject  *voice)
{
  JsonObject  *language = json_object (voice, "language");
  JsonObject  *files = json_object (voice, "files");
  const gchar *name = json_string (voice, "name");
  const gchar *quality = json_string (voice, "quality");
  const gchar *english = json_string (language, "name_english");
  const gchar *country = json_string (language, "country_english");
  const gchar *card = NULL;
  gchar       *family, *label, *size;
  gint64       bytes = 0;
  GList       *members, *l;
  GtkTreeIter  iter;
  VoiceState   state;

  if (!voice_installer_valid_key (key) || name == NULL || quality == NULL
      || english == NULL || files == NULL)
    return FALSE;

  members = json_object_get_members (files);
  for (l = members; l != NULL; l = l->next)
    {
      const gchar *path = l->data;
      JsonObject  *file = json_object (files, path);

      if (g_str_has_suffix (path, ".onnx") && file != NULL
          && json_object_has_member (file, "size_bytes"))
        bytes = json_object_get_int_member (file, "size_bytes");
      else if (g_str_has_suffix (path, "/MODEL_CARD") && valid_card_path (path))
        card = path;
    }

  family = g_strndup (key, strcspn (key, "_"));
  label = country != NULL ? g_strdup_printf ("%s (%s)", english, country)
                          : g_strdup (english);
  size = bytes > 0 ? g_format_size (bytes) : g_strdup ("");
  state = voice_state (key);

  gtk_list_store_insert_with_values (in->store, &iter, -1,
                                     COL_KEY, key,
                                     COL_FAMILY, family,
                                     COL_LANGUAGE, label,
                                     COL_NAME, name,
                                     COL_QUALITY, quality,
                                     COL_SIZE, size,
                                     COL_STATE, state,
                                     COL_STATE_TEXT, state_text (state),
                                     COL_CARD, card,
                                     -1);
  g_list_free (members);
  g_free (family);
  g_free (label);
  g_free (size);

  return TRUE;
}



/* The languages of the store into the combo box: "All languages" first,
 * then by name; the one of the speech settings is chosen */
static void
installer_fill_languages (Installer *in)
{
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT (in->language_combo);
  GtkTreeModel    *model = GTK_TREE_MODEL (in->store);
  GHashTable      *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GList           *families, *l;
  GtkTreeIter      iter;
  gboolean         valid;
  gchar           *wanted;

  for (valid = gtk_tree_model_get_iter_first (model, &iter); valid;
       valid = gtk_tree_model_iter_next (model, &iter))
    {
      gchar *family, *language;

      gtk_tree_model_get (model, &iter, COL_FAMILY, &family, COL_LANGUAGE, &language, -1);
      /* "Russian (Russia)" -> "Russian" */
      language[strcspn (language, "(")] = '\0';
      g_strstrip (language);
      if (g_hash_table_contains (names, family))
        {
          g_free (family);
          g_free (language);
        }
      else
        g_hash_table_insert (names, family, language);
    }

  gtk_combo_box_text_remove_all (combo);
  gtk_combo_box_text_append (combo, "", _("All languages"));
  families = g_list_sort (g_hash_table_get_keys (names), (GCompareFunc) g_strcmp0);
  /* sorted by family code, which is close to the English names */
  for (l = families; l != NULL; l = l->next)
    gtk_combo_box_text_append (combo, l->data, g_hash_table_lookup (names, l->data));
  g_list_free (families);

  wanted = g_ascii_strdown (in->language != NULL ? in->language : "", -1);
  wanted[strcspn (wanted, "-_")] = '\0';
  if (!gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), wanted))
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
  g_free (wanted);
  g_hash_table_unref (names);
}



static gboolean
installer_load_catalogue (Installer   *in,
                          const gchar *data,
                          gsize        length,
                          GError     **error)
{
  JsonParser *parser = json_parser_new ();
  JsonNode   *root;
  GList      *keys, *l;
  guint       n = 0;

  if (!json_parser_load_from_data (parser, data, length, error))
    {
      g_object_unref (parser);
      return FALSE;
    }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of voices is damaged"));
      g_object_unref (parser);
      return FALSE;
    }

  gtk_list_store_clear (in->store);
  keys = json_object_get_members (json_node_get_object (root));
  for (l = keys; l != NULL; l = l->next)
    if (installer_add_voice (in, l->data, json_object (json_node_get_object (root), l->data)))
      n++;
  g_list_free (keys);
  g_object_unref (parser);

  if (n == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of voices is empty"));
      return FALSE;
    }

  installer_fill_languages (in);

  return TRUE;
}



static gchar *
catalogue_cache_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), PACKAGE, "piper-voices.json", NULL);
}



/* The cached list, if any; @fresh: younger than a week */
static gboolean
installer_load_cached (Installer *in,
                       gboolean   fresh)
{
  gchar    *path = catalogue_cache_path ();
  gchar    *data = NULL;
  gsize     length;
  GStatBuf  st;
  gboolean  loaded = FALSE;

  if (g_stat (path, &st) == 0
      && (!fresh || g_get_real_time () - (gint64) st.st_mtime * G_USEC_PER_SEC < CATALOGUE_MAX_AGE)
      && g_file_get_contents (path, &data, &length, NULL))
    loaded = installer_load_catalogue (in, data, length, NULL);

  g_free (data);
  g_free (path);

  return loaded;
}



static void
installer_catalogue_done (GObject      *source,
                          GAsyncResult *result,
                          gpointer      data)
{
  Installer *in = data;
  GBytes    *bytes;
  GError    *error = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (in->closed)
    goto out;

  if (bytes != NULL && g_bytes_get_size (bytes) > CATALOGUE_MAX_SIZE)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("The list of voices is too big"));

  if (error == NULL
      && installer_load_catalogue (in, g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes), &error))
    {
      gchar *path = catalogue_cache_path ();
      gchar *dir = g_path_get_dirname (path);

      g_mkdir_with_parents (dir, 0700);
      g_file_set_contents (path, g_bytes_get_data (bytes, NULL),
                           g_bytes_get_size (bytes), NULL);
      installer_set_status (in, NULL);
      g_free (dir);
      g_free (path);
    }
  else if (installer_load_cached (in, FALSE))
    installer_set_status (in, _("The list of voices could not be updated; "
                                "an older one is shown."));
  else
    {
      gchar *text = g_strdup_printf (_("The list of voices could not be "
                                       "loaded: %s"), error->message);

      installer_set_status (in, text);
      g_free (text);
    }

out:
  if (bytes != NULL)
    g_bytes_unref (bytes);
  g_clear_error (&error);
  installer_unref (in);
}



static void
installer_fetch_catalogue (Installer *in)
{
  SoupMessage *msg;

  if (installer_load_cached (in, TRUE))
    return;

  installer_set_status (in, _("Loading the list of voices…"));
  msg = soup_message_new (SOUP_METHOD_GET, CATALOGUE_URL);
  soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                    in->cancellable, installer_catalogue_done,
                                    installer_ref (in));
  g_object_unref (msg);
}



/* --------------------------------------------------------------- install */

/* Ends an installation; @error NULL: done */
static void
installer_finish (Installer *in,
                  GError    *error)
{
  if (in->download != NULL)
    g_unlink (in->download);
  if (in->unpack != NULL)
    remove_tree (in->unpack);
  g_clear_pointer (&in->download, g_free);
  g_clear_pointer (&in->unpack, g_free);
  g_clear_object (&in->in);
  g_clear_object (&in->out);
  g_clear_object (&in->tar);
  in->busy = FALSE;

  if (!in->closed)
    {
      gtk_widget_hide (in->progress);
      if (error == NULL)
        installer_set_status (in, NULL);
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          gchar *text = g_strdup_printf (_("The voice could not be installed: %s"),
                                         error->message);

          installer_set_status (in, text);
          g_free (text);
        }
      installer_update_row (in, in->key);
      installer_update_buttons (in);
    }

  if (error == NULL && in->changed != NULL)
    in->changed (in->changed_data);

  g_clear_pointer (&in->key, g_free);
  installer_unref (in);
}



/* The unpacked voice checked and moved into place */
static gboolean
installer_put_in_place (Installer *in,
                        GError   **error)
{
  gchar   *name = voice_dir_name (in->key);
  gchar   *src = g_build_filename (in->unpack, name, NULL);
  gchar   *user_dir = voice_installer_user_dir ();
  gchar   *dest = g_build_filename (user_dir, name, NULL);
  gchar   *onnx = g_strdup_printf ("%s/%s.onnx", src, in->key);
  gchar   *json = g_strdup_printf ("%s/%s.onnx.json", src, in->key);
  gchar   *tokens = g_build_filename (src, "tokens.txt", NULL);
  gchar   *espeak = g_build_filename (src, "espeak-ng-data", NULL);
  gchar   *text = NULL;
  gboolean ok = FALSE;

  if (has_odd_files (in->unpack)
      || !g_file_test (onnx, G_FILE_TEST_IS_REGULAR)
      || !g_file_test (tokens, G_FILE_TEST_IS_REGULAR)
      || !g_file_test (espeak, G_FILE_TEST_IS_DIR)
      || !g_file_get_contents (json, &text, NULL, NULL))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("the download is not a voice"));
  else if (g_regex_match_simple ("\"phoneme_type\"\\s*:\\s*\"text\"", text, 0, 0))
    /* see sd_sherpa.c: such voices read letters, not phonemes */
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         _("this voice reads letters instead of sounds, "
                           "which is not supported yet"));
  else if (g_file_test (dest, G_FILE_TEST_EXISTS))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                         _("the voice is installed already"));
  else
    {
      /* the shared data of the piper-espeak-ng-data package, if installed */
      if (g_file_test (SHARED_ESPEAK_DATA, G_FILE_TEST_IS_DIR)
          && remove_tree (espeak))
        {
          if (symlink (SHARED_ESPEAK_DATA, espeak) != 0)
            g_warning ("symlink %s: %s", espeak, g_strerror (errno));
        }

      if (g_file_test (espeak, G_FILE_TEST_IS_DIR) && g_rename (src, dest) == 0)
        ok = TRUE;
      else
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "%s: %s", dest, g_strerror (errno));
    }

  g_free (text);
  g_free (espeak);
  g_free (tokens);
  g_free (json);
  g_free (onnx);
  g_free (dest);
  g_free (user_dir);
  g_free (src);
  g_free (name);

  return ok;
}



static void
installer_tar_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      data)
{
  Installer *in = data;
  GError    *error = NULL;

  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error))
    {
      /* tar goes on after a cancelled wait: stop it before its directory goes */
      g_subprocess_force_exit (G_SUBPROCESS (source));
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               _("the download could not be unpacked"));
        }
    }
  else
    installer_put_in_place (in, &error);

  installer_finish (in, error);
  g_clear_error (&error);
}



static void
installer_unpack (Installer *in)
{
  gchar  *user_dir = voice_installer_user_dir ();
  gchar  *dir_name = g_strdup_printf (".unpack-%s", in->key);
  GError *error = NULL;

  in->unpack = g_build_filename (user_dir, dir_name, NULL);
  remove_tree (in->unpack);
  if (g_mkdir (in->unpack, 0755) != 0)
    g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s: %s", in->unpack, g_strerror (errno));
  else
    in->tar = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error, "tar", "-xjf", in->download,
                                "-C", in->unpack, "--no-same-owner",
                                "--no-same-permissions", NULL);

  if (in->tar != NULL)
    {
      installer_set_status (in, _("Unpacking…"));
      g_subprocess_wait_check_async (in->tar, in->cancellable, installer_tar_done, in);
    }
  else
    {
      installer_finish (in, error);
      g_error_free (error);
    }

  g_free (dir_name);
  g_free (user_dir);
}



static void installer_read_done (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      data);

static void
installer_read_next (Installer *in)
{
  g_input_stream_read_bytes_async (in->in, CHUNK_SIZE, G_PRIORITY_DEFAULT,
                                   in->cancellable, installer_read_done, in);
}



static void
installer_read_done (GObject      *source,
                     GAsyncResult *result,
                     gpointer      data)
{
  Installer *in = data;
  GError    *error = NULL;
  GBytes    *bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source),
                                                       result, &error);

  if (bytes == NULL)
    {
      installer_finish (in, error);
      g_error_free (error);
      return;
    }

  if (g_bytes_get_size (bytes) == 0)
    {
      g_bytes_unref (bytes);
      if (!g_output_stream_close (in->out, NULL, &error)
          || (in->total > 0 && in->done != in->total))
        {
          if (error == NULL)
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                                 _("the download was cut short"));
          installer_finish (in, error);
          g_error_free (error);
        }
      else
        installer_unpack (in);
      return;
    }

  in->done += g_bytes_get_size (bytes);
  if (in->done > VOICE_MAX_SIZE)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("the download is too big"));
  else
    g_output_stream_write_all (in->out, g_bytes_get_data (bytes, NULL),
                               g_bytes_get_size (bytes), NULL, in->cancellable, &error);
  g_bytes_unref (bytes);
  if (error != NULL)
    {
      installer_finish (in, error);
      g_error_free (error);
      return;
    }

  if (!in->closed)
    {
      if (in->total > 0)
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (in->progress),
                                       (gdouble) in->done / in->total);
      else
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (in->progress));
    }
  installer_read_next (in);
}



static void
installer_send_done (GObject      *source,
                     GAsyncResult *result,
                     gpointer      data)
{
  Installer   *in = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  GError      *error = NULL;
  GFile       *file;

  in->in = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  if (in->in == NULL)
    goto failed;

  if (soup_message_get_status (msg) == SOUP_STATUS_NOT_FOUND)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("sherpa-onnx does not offer this voice"));
      goto failed;
    }
  if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                   soup_message_get_status (msg),
                   soup_message_get_reason_phrase (msg));
      goto failed;
    }

  in->total = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));
  if (in->total > VOICE_MAX_SIZE)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("the download is too big"));
      goto failed;
    }

  file = g_file_new_for_path (in->download);
  in->out = G_OUTPUT_STREAM (g_file_replace (file, NULL, FALSE, G_FILE_CREATE_PRIVATE,
                                             NULL, &error));
  g_object_unref (file);
  if (in->out == NULL)
    goto failed;

  installer_set_status (in, _("Downloading…"));
  installer_read_next (in);
  return;

failed:
  installer_finish (in, error);
  g_error_free (error);
}



static void
installer_install (Installer *in)
{
  GtkTreeIter  iter;
  gchar       *user_dir, *name, *url;
  SoupMessage *msg;

  if (in->busy || !installer_get_selected (in, &iter))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_KEY, &in->key, -1);
  if (!voice_installer_valid_key (in->key))
    {
      g_clear_pointer (&in->key, g_free);
      return;
    }

  user_dir = voice_installer_user_dir ();
  if (g_mkdir_with_parents (user_dir, 0755) != 0)
    {
      gchar *text = g_strdup_printf (_("The voice could not be installed: %s"),
                                     g_strerror (errno));

      installer_set_status (in, text);
      g_free (text);
      g_free (user_dir);
      g_clear_pointer (&in->key, g_free);
      return;
    }

  in->busy = TRUE;
  in->total = in->done = 0;
  name = g_strdup_printf (".download-%s.tar.bz2", in->key);
  in->download = g_build_filename (user_dir, name, NULL);
  url = g_strdup_printf (VOICE_URL, in->key);
  msg = soup_message_new (SOUP_METHOD_GET, url);

  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (in->progress), 0.0);
  gtk_widget_show (in->progress);
  installer_set_status (in, _("Connecting…"));
  installer_update_buttons (in);
  soup_session_send_async (in->session, msg, G_PRIORITY_DEFAULT, in->cancellable,
                           installer_send_done, installer_ref (in));

  g_object_unref (msg);
  g_free (url);
  g_free (name);
  g_free (user_dir);
}



static void
installer_remove (Installer *in)
{
  GtkTreeIter iter;
  gchar      *key = NULL, *user_dir, *name, *path;

  if (in->busy || !installer_get_selected (in, &iter))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_KEY, &key, -1);
  if (!voice_installer_valid_key (key))
    {
      g_free (key);
      return;
    }

  user_dir = voice_installer_user_dir ();
  name = voice_dir_name (key);
  path = g_build_filename (user_dir, name, NULL);
  if (remove_tree (path))
    {
      installer_set_status (in, NULL);
      if (in->changed != NULL)
        in->changed (in->changed_data);
    }
  else
    {
      gchar *text = g_strdup_printf (_("The voice could not be removed: %s"),
                                     g_strerror (errno));

      installer_set_status (in, text);
      g_free (text);
    }
  installer_update_row (in, key);
  installer_update_buttons (in);

  g_free (path);
  g_free (name);
  g_free (user_dir);
  g_free (key);
}



/* ---------------------------------------------------------------- dialog */

static gboolean
installer_visible (GtkTreeModel *model,
                   GtkTreeIter  *iter,
                   gpointer      data)
{
  Installer   *in = data;
  const gchar *wanted;
  gchar       *family;
  gboolean     visible;

  if (in->closed)
    return TRUE;

  wanted = gtk_combo_box_get_active_id (GTK_COMBO_BOX (in->language_combo));
  if (wanted == NULL || *wanted == '\0')
    return TRUE;

  gtk_tree_model_get (model, iter, COL_FAMILY, &family, -1);
  visible = g_strcmp0 (family, wanted) == 0;
  g_free (family);

  return visible;
}



static void
installer_language_changed (GtkComboBox *combo,
                            Installer   *in)
{
  gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (in->filter));
  installer_update_buttons (in);
  installer_show_licence (in);
}



static void
installer_selection_changed (GtkTreeSelection *selection,
                             Installer        *in)
{
  installer_update_buttons (in);
  installer_show_licence (in);
}



enum
{
  RESPONSE_INSTALL = 1,
  RESPONSE_REMOVE
};

static void
installer_response (GtkDialog *dialog,
                    gint       response,
                    Installer *in)
{
  if (response == RESPONSE_INSTALL)
    installer_install (in);
  else if (response == RESPONSE_REMOVE)
    installer_remove (in);
  else
    gtk_widget_destroy (GTK_WIDGET (dialog));
}



static void
installer_destroyed (GtkWidget *dialog,
                     Installer *in)
{
  in->closed = TRUE;
  g_cancellable_cancel (in->cancellable);
  installer_unref (in);
}



static void
installer_add_column (GtkTreeView *view,
                      const gchar *title,
                      gint         column,
                      gboolean     expand)
{
  GtkCellRenderer   *renderer = gtk_cell_renderer_text_new ();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes (title, renderer,
                                                                     "text", column,
                                                                     NULL);

  gtk_tree_view_column_set_expand (col, expand);
  gtk_tree_view_column_set_sort_column_id (col, column);
  gtk_tree_view_column_set_resizable (col, TRUE);
  gtk_tree_view_append_column (view, col);
}



void
voice_installer_show (GtkWindow             *parent,
                      const gchar           *language,
                      VoiceInstallerChanged  changed,
                      gpointer               data)
{
  Installer *in = g_new0 (Installer, 1);
  GtkWidget *content, *box, *label, *scrolled;
  GtkTreeSortable *sortable;

  in->refs = 1;
  in->language = g_strdup (language);
  in->changed = changed;
  in->changed_data = data;
  in->licences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  in->cancellable = g_cancellable_new ();
  in->session = soup_session_new_with_options ("user-agent", PACKAGE "/" PACKAGE_VERSION,
                                               "timeout", 60, NULL);

  in->dialog = gtk_dialog_new_with_buttons (_("Piper Voices"), parent,
                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            _("_Remove"), RESPONSE_REMOVE,
                                            _("_Install"), RESPONSE_INSTALL,
                                            _("_Close"), GTK_RESPONSE_CLOSE,
                                            NULL);
  in->install_button = gtk_dialog_get_widget_for_response (GTK_DIALOG (in->dialog),
                                                           RESPONSE_INSTALL);
  in->remove_button = gtk_dialog_get_widget_for_response (GTK_DIALOG (in->dialog),
                                                          RESPONSE_REMOVE);
  gtk_widget_set_tooltip_text (in->install_button,
                               _("Download the voice from the sherpa-onnx "
                                 "project and install it for you"));
  gtk_window_set_default_size (GTK_WINDOW (in->dialog), 640, 480);

  content = gtk_dialog_get_content_area (GTK_DIALOG (in->dialog));
  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 6);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  label = gtk_label_new_with_mnemonic (_("_Language:"));
  in->language_combo = gtk_combo_box_text_new ();
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), in->language_combo);
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), in->language_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (content), box, FALSE, FALSE, 0);

  in->store = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                  G_TYPE_STRING, G_TYPE_STRING);
  sortable = GTK_TREE_SORTABLE (in->store);
  gtk_tree_sortable_set_sort_column_id (sortable, COL_KEY, GTK_SORT_ASCENDING);
  in->filter = gtk_tree_model_filter_new (GTK_TREE_MODEL (in->store), NULL);
  gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (in->filter),
                                          installer_visible, in, NULL);
  in->view = gtk_tree_view_new_with_model (in->filter);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Language"), COL_LANGUAGE, TRUE);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Voice"), COL_NAME, TRUE);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Quality"), COL_QUALITY, FALSE);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Size"), COL_SIZE, FALSE);
  installer_add_column (GTK_TREE_VIEW (in->view), _("State"), COL_STATE_TEXT, FALSE);
  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), in->view);
  gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);

  in->licence_label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (in->licence_label), 0.0);
  gtk_label_set_selectable (GTK_LABEL (in->licence_label), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (in->licence_label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (content), in->licence_label, FALSE, FALSE, 0);

  in->progress = gtk_progress_bar_new ();
  gtk_widget_set_no_show_all (in->progress, TRUE);
  gtk_box_pack_start (GTK_BOX (content), in->progress, FALSE, FALSE, 0);

  in->status_label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (in->status_label), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (in->status_label), TRUE);
  gtk_box_pack_start (GTK_BOX (content), in->status_label, FALSE, FALSE, 0);

  g_signal_connect (in->language_combo, "changed",
                    G_CALLBACK (installer_language_changed), in);
  g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (in->view)), "changed",
                    G_CALLBACK (installer_selection_changed), in);
  g_signal_connect (in->dialog, "response", G_CALLBACK (installer_response), in);
  g_signal_connect (in->dialog, "destroy", G_CALLBACK (installer_destroyed), in);

  installer_update_buttons (in);
  gtk_widget_show_all (in->dialog);
  installer_fetch_catalogue (in);
}
