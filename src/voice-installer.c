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
 * The window that installs more voices for a synthesizer. What the voices
 * are and how one is installed is the business of the source of that
 * synthesizer (voice-source-*.c); here are the list, the buttons, the
 * catalogue (kept a week in ~/.cache) and downloading.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#ifdef HAVE_XFCE
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#endif
#ifdef HAVE_GTK_X11
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#endif

#include "voice-installer-private.h"

/* the list is fetched again after a week */
#define CATALOGUE_MAX_AGE (7 * G_TIME_SPAN_DAY)
#define CATALOGUE_MAX_SIZE (16 * 1024 * 1024)
#define CHUNK_SIZE (64 * 1024)

enum
{
  COL_VOICE,       /* of the source, owned by in->voices */
  COL_FAMILY,      /* "ru": the language without the region */
  COL_LANGUAGE,    /* "Russian (Russia)" */
  COL_NAME,
  COL_QUALITY,
  COL_SIZE,
  COL_STATE,
  COL_STATE_TEXT,
  N_COLS
};

static const VoiceSource *sources[] =
{
  &voice_source_piper,
  &voice_source_rhvoice,
};



/* The source of @module: "sherpa-onnx" also for "sherpa-onnx-vulkan-rs" */
static const VoiceSource *
find_source (const gchar *module)
{
  guint i;

  if (module == NULL)
    return NULL;

  for (i = 0; i < G_N_ELEMENTS (sources); i++)
    {
      gsize len = strlen (sources[i]->module);

      if (strncmp (module, sources[i]->module, len) == 0
          && (module[len] == '\0' || module[len] == '-')
          && g_file_test (sources[i]->module_path, G_FILE_TEST_EXISTS))
        return sources[i];
    }

  return NULL;
}



gboolean
voice_installer_has_source (const gchar *module)
{
  return find_source (module) != NULL;
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
      return _("Available");
    }
}



/* ------------------------------------------------------------- helpers */

/* Removes @path and everything below it; links are removed, not followed */
gboolean
vi_remove_tree (const gchar *path)
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

          vi_remove_tree (child);
          g_free (child);
        }
      if (dir != NULL)
        g_dir_close (dir);

      return g_rmdir (path) == 0;
    }

  return g_unlink (path) == 0;
}



/* "[LGPL v2.1](https://…)" -> "LGPL v2.1", at most 120 characters */
static gchar *
licence_clean (const gchar *line)
{
  GRegex *link = g_regex_new ("\\[([^]]*)\\]\\([^)]*\\)", 0, 0, NULL);
  gchar  *text = g_regex_replace (link, line, -1, 0, "\\1", 0, NULL);

  g_regex_unref (link);
  if (text == NULL)
    return NULL;

  g_strstrip (g_strdelimit (text, "\t\r", ' '));
  /* list marks and headings */
  while (*text == '*' || *text == '-' || *text == '#')
    {
      memmove (text, text + 1, strlen (text));
      g_strchug (text);
    }
  if (*text == '\0')
    {
      g_free (text);
      return NULL;
    }
  if (g_utf8_strlen (text, -1) > 120)
    {
      gchar *cut = g_utf8_substring (text, 0, 120);

      g_free (text);
      text = cut;
    }

  return text;
}



/* The licence a model card or README states: "License: CC0", the line after
 * a "License" heading, or "This voice is distributed under …" */
gchar *
vi_licence_line (const gchar *data,
                 gsize        length)
{
  gchar   *text = g_strndup (data, length);
  gchar  **lines;
  gchar   *licence = NULL;
  gboolean next = FALSE;
  guint    i;

  if (!g_utf8_validate (text, -1, NULL))
    {
      g_free (text);
      return NULL;
    }

  lines = g_strsplit (text, "\n", -1);
  for (i = 0; licence == NULL && lines[i] != NULL; i++)
    {
      gchar       *lower = g_utf8_strdown (lines[i], -1);
      const gchar *at = strstr (lower, "license:");

      if (at == NULL)
        at = strstr (lower, "licence:");

      if (next)
        licence = licence_clean (lines[i]);
      else if (at != NULL)
        {
          licence = licence_clean (lines[i] + (at - lower) + strlen ("license:"));
          next = licence == NULL;
        }
      else if (g_str_has_prefix (g_strchug (lower), "#") && strstr (lower, "licen") != NULL)
        next = TRUE;
      else if (strstr (lower, "distributed under") != NULL
               || strstr (lower, "distributed in the") != NULL)
        licence = licence_clean (lines[i]);
      g_free (lower);
    }

  g_strfreev (lines);
  g_free (text);

  return licence;
}



const gchar *
vi_json_string (JsonObject  *object,
                const gchar *member)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node)
      || json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return json_node_get_string (node);
}



JsonObject *
vi_json_object (JsonObject  *object,
                const gchar *member)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  return node != NULL && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}



JsonArray *
vi_json_array (JsonObject  *object,
               const gchar *member)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  return node != NULL && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}



gint64
vi_json_int (JsonObject  *object,
             const gchar *member,
             gint64       fallback)
{
  JsonNode *node = object != NULL ? json_object_get_member (object, member) : NULL;

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node)
      || json_node_get_value_type (node) != G_TYPE_INT64)
    return fallback;

  return json_node_get_int (node);
}



gboolean
installer_http_ok (SoupMessage *msg,
                   GError     **error)
{
  if (soup_message_get_status (msg) == SOUP_STATUS_OK)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
               soup_message_get_status (msg), soup_message_get_reason_phrase (msg));
  return FALSE;
}



/* ----------------------------------------------------------- the window */

Installer *
installer_ref (Installer *in)
{
  in->refs++;
  return in;
}



void
installer_unref (Installer *in)
{
  if (--in->refs > 0)
    return;

  g_clear_object (&in->session);
  g_clear_object (&in->cancellable);
  g_clear_object (&in->filter);
  g_clear_object (&in->store);
  g_ptr_array_unref (in->voices);
  if (in->data != NULL)
    in->source->free_data (in->data);
  g_free (in->language);
  g_free (in);
}



void
installer_set_status (Installer   *in,
                      const gchar *text)
{
  if (!in->closed)
    gtk_label_set_text (GTK_LABEL (in->status_label), text != NULL ? text : "");
}



void
installer_set_licence (Installer   *in,
                       const gchar *licence)
{
  gchar *text;

  if (in->closed)
    return;

  text = licence != NULL ? g_strdup_printf (_("Licence: %s"), licence) : g_strdup ("");
  gtk_label_set_text (GTK_LABEL (in->licence_label), text);
  g_free (text);
}



/* @fraction < 0: no idea how far */
void
installer_set_progress (Installer *in,
                        gdouble    fraction)
{
  if (in->closed)
    return;

  if (fraction < 0.0)
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (in->progress));
  else
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (in->progress), MIN (fraction, 1.0));
}



void
installer_add_voice (Installer   *in,
                     gpointer     voice,
                     const gchar *family,
                     const gchar *language,
                     const gchar *name,
                     const gchar *quality,
                     gint64       bytes)
{
  VoiceState state = in->source->state (in, voice);
  gchar     *size = bytes > 0 ? g_format_size (bytes) : g_strdup ("");

  g_ptr_array_add (in->voices, voice);
  gtk_list_store_insert_with_values (in->store, NULL, -1,
                                     COL_VOICE, voice,
                                     COL_FAMILY, family,
                                     COL_LANGUAGE, language,
                                     COL_NAME, name,
                                     COL_QUALITY, quality,
                                     COL_SIZE, size,
                                     COL_STATE, state,
                                     COL_STATE_TEXT, state_text (state),
                                     -1);
  g_free (size);
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



gpointer
installer_selected (Installer *in)
{
  GtkTreeIter iter;
  gpointer    voice = NULL;

  if (installer_get_selected (in, &iter))
    gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_VOICE, &voice, -1);

  return voice;
}



/* The state of every voice again: a package brings several */
void
installer_update_rows (Installer *in)
{
  GtkTreeModel *model = GTK_TREE_MODEL (in->store);
  GtkTreeIter   iter;
  gboolean      valid;

  for (valid = gtk_tree_model_get_iter_first (model, &iter); valid;
       valid = gtk_tree_model_iter_next (model, &iter))
    {
      gpointer   voice;
      VoiceState state;

      gtk_tree_model_get (model, &iter, COL_VOICE, &voice, -1);
      state = in->source->state (in, voice);
      gtk_list_store_set (in->store, &iter, COL_STATE, state,
                          COL_STATE_TEXT, state_text (state), -1);
    }
}



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



static void
installer_show_licence (Installer *in)
{
  if (!in->closed)
    in->source->licence (in, installer_selected (in));
}



/* ------------------------------------------------------------ catalogue */

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
  JsonParser *parser;
  JsonNode   *root;
  GError     *local = NULL;
  gboolean    ok;

  /* the rows point to the voices, one of them is being installed */
  if (in->busy)
    return TRUE;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, length, error))
    {
      g_object_unref (parser);
      return FALSE;
    }

  root = json_parser_get_root (parser);
  if (root == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of voices is damaged"));
      g_object_unref (parser);
      return FALSE;
    }

  gtk_list_store_clear (in->store);
  g_ptr_array_set_size (in->voices, 0);
  ok = in->source->load (in, root, &local);
  g_object_unref (parser);
  if (!ok || in->voices->len == 0)
    {
      gtk_list_store_clear (in->store);
      g_ptr_array_set_size (in->voices, 0);
      if (local == NULL)
        local = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                     _("The list of voices is empty"));
      g_propagate_error (error, local);
      return FALSE;
    }

  installer_fill_languages (in);

  return TRUE;
}



static gchar *
catalogue_cache_path (Installer *in)
{
  return g_build_filename (g_get_user_cache_dir (), PACKAGE, in->source->cache_name, NULL);
}



/* The cached list, if any; @fresh: younger than a week */
static gboolean
installer_load_cached (Installer *in,
                       gboolean   fresh)
{
  gchar    *path = catalogue_cache_path (in);
  gchar    *data = NULL;
  gsize     length;
  GStatBuf  st;
  gboolean  loaded = FALSE;

  if (g_stat (path, &st) == 0
      && st.st_size <= CATALOGUE_MAX_SIZE
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
  Installer   *in = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  GBytes      *bytes;
  GError      *error = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (in->closed)
    goto out;

  if (bytes != NULL)
    installer_http_ok (msg, &error);
  if (error == NULL && g_bytes_get_size (bytes) > CATALOGUE_MAX_SIZE)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("The list of voices is too big"));

  if (error == NULL
      && installer_load_catalogue (in, g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes), &error))
    {
      gchar *path = catalogue_cache_path (in);
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
  installer_update_buttons (in);

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
  msg = soup_message_new (SOUP_METHOD_GET, in->source->catalogue_url);
  soup_session_send_and_read_async (in->session, msg, G_PRIORITY_DEFAULT,
                                    in->cancellable, installer_catalogue_done,
                                    installer_ref (in));
  g_object_unref (msg);
}



/* ------------------------------------------------------------- download */

static void
installer_download_end (Installer *in,
                        GError    *error)
{
  InstallerDownloadDone  done = in->download_done;
  gpointer               data = in->download_data;
  GError                *local = NULL;
  gchar                 *md5 = NULL;

  if (error == NULL && in->out != NULL)
    {
      if (g_output_stream_close (in->out, in->cancellable, &local))
        md5 = g_strdup (g_checksum_get_string (in->md5));
      else
        error = local;
    }

  g_clear_object (&in->in);
  g_clear_object (&in->out);
  g_clear_pointer (&in->md5, g_checksum_free);
  in->download_done = NULL;
  in->download_data = NULL;

  done (in, md5, error, data);
  g_free (md5);
  g_clear_error (&local);
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
  gsize      size;

  if (bytes == NULL)
    goto failed;

  size = g_bytes_get_size (bytes);
  if (size == 0)
    {
      g_bytes_unref (bytes);
      if (in->total > 0 && in->done != in->total)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                               _("the download was cut short"));
          goto failed;
        }
      installer_download_end (in, NULL);
      return;
    }

  in->done += size;
  if (in->done > in->max)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("the download is too big"));
  else
    {
      g_checksum_update (in->md5, g_bytes_get_data (bytes, NULL), size);
      g_output_stream_write_all (in->out, g_bytes_get_data (bytes, NULL), size,
                                 NULL, in->cancellable, &error);
    }
  g_bytes_unref (bytes);
  if (error != NULL)
    goto failed;

  installer_set_progress (in, in->total > 0
                              ? in->base + in->span * in->done / in->total : -1.0);
  installer_read_next (in);
  return;

failed:
  installer_download_end (in, error);
  g_error_free (error);
}



static void
installer_download_sent (GObject      *source,
                         GAsyncResult *result,
                         gpointer      data)
{
  Installer   *in = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), result);
  GError      *error = NULL;
  GFile       *file;
  const gchar *path = g_object_get_data (G_OBJECT (msg), "path");

  in->in = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  if (in->in == NULL || !installer_http_ok (msg, &error))
    goto failed;

  in->total = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));
  if (in->total > in->max)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("the download is too big"));
      goto failed;
    }

  file = g_file_new_for_path (path);
  in->out = G_OUTPUT_STREAM (g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE,
                                             in->cancellable, &error));
  g_object_unref (file);
  if (in->out == NULL)
    goto failed;

  installer_set_status (in, _("Downloading…"));
  installer_read_next (in);
  return;

failed:
  installer_download_end (in, error);
  g_error_free (error);
}



/* @url into the file @path, at most @max bytes, the bar going from @base to
 * @base + @span; @done gets the MD5 sum of what came */
void
installer_download (Installer             *in,
                    const gchar           *url,
                    const gchar           *path,
                    goffset                max,
                    gdouble                base,
                    gdouble                span,
                    InstallerDownloadDone  done,
                    gpointer               data)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);

  in->download_done = done;
  in->download_data = data;
  in->total = in->done = 0;
  in->max = max;
  in->base = base;
  in->span = span;
  in->md5 = g_checksum_new (G_CHECKSUM_MD5);

  if (msg == NULL)
    {
      GError *error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s", url);

      installer_download_end (in, error);
      g_error_free (error);
      return;
    }

  installer_set_progress (in, base);
  installer_set_status (in, _("Connecting…"));
  g_object_set_data_full (G_OBJECT (msg), "path", g_strdup (path), g_free);
  soup_session_send_async (in->session, msg, G_PRIORITY_DEFAULT, in->cancellable,
                           installer_download_sent, in);
  g_object_unref (msg);
}



/* --------------------------------------------------------------- install */

/* Ends an installation; @error NULL: done */
void
installer_finish (Installer *in,
                  GError    *error)
{
  if (in->unpack != NULL)
    vi_remove_tree (in->unpack);
  g_clear_pointer (&in->unpack, g_free);
  in->busy = FALSE;
  in->voice = NULL;

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
      installer_update_rows (in);
      installer_update_buttons (in);
    }

  if (error == NULL && in->changed != NULL)
    in->changed (in->changed_data);

  installer_unref (in);
}



static void
installer_install (Installer *in)
{
  GtkTreeIter iter;
  gpointer    voice = NULL;
  gint        state = -1;

  if (in->busy || !installer_get_selected (in, &iter))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_VOICE, &voice,
                      COL_STATE, &state, -1);
  if (voice == NULL || state != STATE_NONE)
    return;

  in->busy = TRUE;
  in->voice = voice;
  installer_ref (in);

  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (in->progress), 0.0);
  gtk_widget_show (in->progress);
  installer_set_status (in, _("Connecting…"));
  installer_update_buttons (in);

  in->source->install (in, voice);
}



static void
installer_remove (Installer *in)
{
  GtkTreeIter iter;
  gpointer    voice = NULL;
  gint        state = -1;
  GError     *error = NULL;

  if (in->busy || !installer_get_selected (in, &iter))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (in->store), &iter, COL_VOICE, &voice,
                      COL_STATE, &state, -1);
  if (voice == NULL || state != STATE_USER)
    return;

  if (in->source->remove (in, voice, &error))
    {
      installer_set_status (in, NULL);
      if (in->changed != NULL)
        in->changed (in->changed_data);
    }
  else
    {
      gchar *text = g_strdup_printf (_("The voice could not be removed: %s"),
                                     error->message);

      installer_set_status (in, text);
      g_free (text);
      g_error_free (error);
    }
  installer_update_rows (in);
  installer_update_buttons (in);
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



/* by language, then by name */
static gint
installer_compare (GtkTreeModel *model,
                   GtkTreeIter  *a,
                   GtkTreeIter  *b,
                   gpointer      data)
{
  gchar *la, *lb, *na, *nb;
  gint   result;

  gtk_tree_model_get (model, a, COL_LANGUAGE, &la, COL_NAME, &na, -1);
  gtk_tree_model_get (model, b, COL_LANGUAGE, &lb, COL_NAME, &nb, -1);
  result = g_utf8_collate (la != NULL ? la : "", lb != NULL ? lb : "");
  if (result == 0)
    result = g_utf8_collate (na != NULL ? na : "", nb != NULL ? nb : "");
  g_free (la);
  g_free (lb);
  g_free (na);
  g_free (nb);

  return result;
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

/* Closing the dialog stops a download: ask first. FALSE: keep it open. */
static gboolean
installer_may_close (Installer *in)
{
  GtkWidget *ask;
  gint       answer;
  gboolean   closed;

  if (!in->busy)
    return TRUE;

  ask = gtk_message_dialog_new (GTK_WINDOW (in->dialog),
                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                _("Stop installing the voice?"));
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (ask),
                                            _("The voice is still being downloaded. "
                                              "If the window is closed, the download "
                                              "stops and the voice is not installed."));
  gtk_dialog_add_buttons (GTK_DIALOG (ask),
                          _("_Continue Downloading"), GTK_RESPONSE_CANCEL,
                          _("_Stop and Close"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (ask), GTK_RESPONSE_CANCEL);

  /* the download goes on meanwhile; the dialog may go with its parent */
  installer_ref (in);
  answer = gtk_dialog_run (GTK_DIALOG (ask));
  closed = in->closed;
  if (!closed)
    gtk_widget_destroy (ask);
  installer_unref (in);

  return !closed && answer == GTK_RESPONSE_ACCEPT;
}



static void
installer_response (GtkDialog *dialog,
                    gint       response,
                    Installer *in)
{
  if (response == RESPONSE_INSTALL)
    installer_install (in);
  else if (response == RESPONSE_REMOVE)
    installer_remove (in);
  else if (installer_may_close (in))
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



/* In the settings manager the page sits in a GtkPlug, which the window
 * manager does not know as a window: the dialog would be a window of its
 * own and could end up under the manager.  Make it transient for the
 * top-level window that holds the socket. */
static void
voice_installer_set_parent (GtkWindow *dialog,
                            GtkWindow *parent)
{
#ifdef HAVE_GTK_X11
  GdkWindow  *socket, *top;
  GdkDisplay *display;
  Display    *xdisplay;
  Window      xid, root, up, *children;
  guint       n;

  if (!GTK_IS_PLUG (parent))
    return;
  socket = gtk_plug_get_socket_window (GTK_PLUG (parent));
  display = gtk_widget_get_display (GTK_WIDGET (dialog));
  if (socket == NULL || !GDK_IS_X11_DISPLAY (display))
    return;
  xdisplay = GDK_DISPLAY_XDISPLAY (display);
  xid = GDK_WINDOW_XID (socket);
  gdk_x11_display_error_trap_push (display);
  for (;;)
    {
      if (!XQueryTree (xdisplay, xid, &root, &up, &children, &n))
        {
          xid = None;
          break;
        }
      if (children != NULL)
        XFree (children);
      if (up == root || up == None)
        break;
      xid = up;
    }
  gdk_x11_display_error_trap_pop_ignored (display);
  if (xid == None)
    return;

  top = gdk_x11_window_foreign_new_for_display (display, xid);
  if (top == NULL)
    return;
  gtk_widget_realize (GTK_WIDGET (dialog));
  gdk_window_set_transient_for (gtk_widget_get_window (GTK_WIDGET (dialog)), top);
  g_object_unref (top);
  gtk_window_set_position (dialog, GTK_WIN_POS_CENTER_ON_PARENT);
#endif
}

void
voice_installer_show (GtkWindow             *parent,
                      const gchar           *module,
                      const gchar           *language,
                      VoiceInstallerChanged  changed,
                      gpointer               data)
{
  const VoiceSource *source = find_source (module);
  Installer         *in;
  GtkWidget         *content, *box, *label, *scrolled;
  GtkTreeSortable   *sortable;

  if (source == NULL)
    return;

  in = g_new0 (Installer, 1);
  in->refs = 1;
  in->source = source;
  in->data = source->new_data != NULL ? source->new_data () : NULL;
  in->voices = g_ptr_array_new_with_free_func (source->free_voice);
  in->language = g_strdup (language);
  in->changed = changed;
  in->changed_data = data;
  in->cancellable = g_cancellable_new ();
  in->session = soup_session_new_with_options ("user-agent", PACKAGE "/" PACKAGE_VERSION,
                                               "timeout", 60, NULL);

  in->dialog = gtk_dialog_new_with_buttons (_(source->title), parent,
                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            _("_Remove"), RESPONSE_REMOVE,
                                            _("_Install"), RESPONSE_INSTALL,
                                            _("_Close"), GTK_RESPONSE_CLOSE,
                                            NULL);
  in->install_button = gtk_dialog_get_widget_for_response (GTK_DIALOG (in->dialog),
                                                           RESPONSE_INSTALL);
  in->remove_button = gtk_dialog_get_widget_for_response (GTK_DIALOG (in->dialog),
                                                          RESPONSE_REMOVE);
  gtk_widget_set_tooltip_text (in->install_button, _(source->install_tooltip));
  gtk_window_set_default_size (GTK_WINDOW (in->dialog), 640, 480);
  voice_installer_set_parent (GTK_WINDOW (in->dialog), parent);

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
  if (source->note != NULL)
    {
      label = gtk_label_new (_(source->note));
      gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);
    }

  in->store = gtk_list_store_new (N_COLS, G_TYPE_POINTER, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_INT, G_TYPE_STRING);
  sortable = GTK_TREE_SORTABLE (in->store);
  gtk_tree_sortable_set_default_sort_func (sortable, installer_compare, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id (sortable, GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                        GTK_SORT_ASCENDING);
  in->filter = gtk_tree_model_filter_new (GTK_TREE_MODEL (in->store), NULL);
  gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (in->filter),
                                          installer_visible, in, NULL);
  in->view = gtk_tree_view_new_with_model (in->filter);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Language"), COL_LANGUAGE, TRUE);
  installer_add_column (GTK_TREE_VIEW (in->view), _("Voice"), COL_NAME, TRUE);
  if (source->quality_title != NULL)
    installer_add_column (GTK_TREE_VIEW (in->view), _(source->quality_title),
                          COL_QUALITY, FALSE);
  if (!source->no_size)
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
