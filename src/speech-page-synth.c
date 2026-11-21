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

/* The options of the selected synthesizer: keeping its voice in memory,
 * loading a voice when it is chosen, a copy of what it speaks. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

#ifdef HAVE_XFCE
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#include <unistd.h>
#include <gtk/gtk.h>
#endif
#include <speech-dispatcher/libspeechd.h>

#include "speech-page-private.h"

/* "SherpaPreload" for "sherpa-onnx" and "sherpa-onnx-vulkan-rs" */
gchar *
speech_page_module_key (const gchar *module,
                        const gchar *suffix)
{
  return speechd_module_option_key (module, suffix);
}



/* Sets option @key of @module to @value (NULL removes it) unless it has it
 * already; the server rereads module configurations on the next save. */
void
speech_page_set_module_option (SpeechPage  *page,
                               const gchar *module,
                               const gchar *key,
                               const gchar *value)
{
  gchar  *old = speechd_module_get_option (module, key);
  GError *error = NULL;
  gchar  *message;

  if (g_strcmp0 (old, value) != 0)
    {
      if (speechd_module_set_option (module, key, value, &error))
        {
          if (!speechd_module_reads_changes (module))
            page->reload_pending = TRUE;
          speech_page_queue_save (page);
        }
      else
        {
          message = g_strdup_printf (_("Failed to save the setting: %s"),
                                     error->message);
          speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
          g_free (message);
          g_error_free (error);
        }
    }
  g_free (old);
}



/* Our modules load their model with the first message and free it after
 * some minutes without speech. "<Module>Preload" loads it when the module
 * starts, "<Module>IdleTimeout 0" keeps it. The check box shows and sets
 * them for the selected synthesizer. A synthesizer that no language uses
 * any more has them parked in the dialog's own file, so its model takes no
 * memory, and gets them back when it is chosen again. */

#define PRELOAD_FILE "preload.ini"

static gchar *
speech_page_preload_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "xfce4-speech-settings",
                           PRELOAD_FILE, NULL);
}

static gboolean
speech_page_option_on (const gchar *value)
{
  return value != NULL && *value != '\0' && strcmp (value, "0") != 0;
}

/* The value of "<Module>Preload": sd_sherpa-onnx has many models and
 * preloads the one of a voice, the others take "1" */
gchar *
speech_page_preload_value (SpeechPage  *page,
                           const gchar *module)
{
  gchar *value;
  gchar *language;
  gchar *type;

  if (strcmp (speechd_module_base (module), "sherpa") != 0
      && strcmp (speechd_module_base (module), "piper") != 0)
    return g_strdup ("1");

  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  value = speechd_module_get_voice (module, language, type);
  if (value == NULL)
    value = g_strdup (gtk_combo_box_get_active_id (
                        GTK_COMBO_BOX (page->voice_combo)));
  g_free (type);
  g_free (language);

  return value;
}

/* Moves the preload options of the modules no language uses into the
 * dialog's file, and back for the modules in use again */
void
speech_page_park_preloads (SpeechPage *page)
{
  GPtrArray *used;
  GKeyFile  *parked;
  gchar     *path;
  gboolean   changed = FALSE;
  guint      i;

  if (page->modules == NULL || page->config == NULL)
    return;

  used = speechd_config_used_modules (page->config);
  parked = g_key_file_new ();
  path = speech_page_preload_path ();
  g_key_file_load_from_file (parked, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  for (i = 0; page->modules[i] != NULL; i++)
    {
      const gchar *module = page->modules[i];
      gchar       *key = speech_page_module_key (module, "Preload");
      gchar       *idle_key = speech_page_module_key (module, "IdleTimeout");
      gchar       *value = NULL;
      gboolean     in_use = speech_page_array_has (used, module);

      if (speechd_module_has_option (module, key))
        value = speechd_module_get_option (module, key);

      if (!in_use && speech_page_option_on (value))
        {
          gchar *idle = speechd_module_get_option (module, idle_key);

          g_key_file_set_string (parked, module, "Preload", value);
          if (idle != NULL)
            g_key_file_set_string (parked, module, "IdleTimeout", idle);
          speech_page_set_module_option (page, module, key, NULL);
          speech_page_set_module_option (page, module, idle_key, NULL);
          changed = TRUE;
          g_free (idle);
        }
      else if (in_use && !speech_page_option_on (value)
               && g_key_file_has_group (parked, module))
        {
          gchar *saved = g_key_file_get_string (parked, module, "Preload", NULL);
          gchar *idle = g_key_file_get_string (parked, module, "IdleTimeout",
                                               NULL);

          if (speech_page_option_on (saved))
            {
              speech_page_set_module_option (page, module, key, saved);
              speech_page_set_module_option (page, module, idle_key, idle);
            }
          g_key_file_remove_group (parked, module, NULL);
          changed = TRUE;
          g_free (idle);
          g_free (saved);
        }

      g_free (value);
      g_free (idle_key);
      g_free (key);
    }

  if (changed)
    {
      gchar *dir = g_path_get_dirname (path);

      if (g_mkdir_with_parents (dir, 0700) == 0)
        g_key_file_save_to_file (parked, path, NULL);
      g_free (dir);
    }

  g_free (path);
  g_key_file_free (parked);
  g_ptr_array_unref (used);
}

/* Forgets a parked state of @module: the user just decided */
static void
speech_page_unpark (const gchar *module)
{
  GKeyFile *parked = g_key_file_new ();
  gchar    *path = speech_page_preload_path ();

  if (g_key_file_load_from_file (parked, path, G_KEY_FILE_KEEP_COMMENTS, NULL)
      && g_key_file_remove_group (parked, module, NULL))
    g_key_file_save_to_file (parked, path, NULL);
  g_free (path);
  g_key_file_free (parked);
}

/* Load requests. The synthesizers of linux-tts load a voice when the user
 * chooses it here, not with the first sentence: the dialog writes the voice
 * to $XDG_RUNTIME_DIR/linux-tts/<module>.load. The module tells what it does
 * in <module>.state ("loading", "ready", "failed <why>", "idle"), shown next
 * to the check box. A synthesizer the user switches away from, and that no
 * language uses any more, gets its file removed after RELEASE_DELAY_S, and
 * the module frees its models (it looks at the file every 2 s); the same for
 * all of them when the dialog closes. Switching back within the delay keeps
 * the model; switching back later loads it again, and a module that is still
 * loading drops the model when the load ends. */

#define RELEASE_DELAY_S 2

static gchar *
speech_page_runtime_path (const gchar *module,
                          const gchar *suffix)
{
  gchar *name = g_strconcat (module, suffix, NULL);
  gchar *path = g_build_filename (g_get_user_runtime_dir (), "linux-tts",
                                  name, NULL);

  g_free (name);
  return path;
}

void
speech_page_show_load_state (SpeechPage *page)
{
  gchar   *module = speech_page_get_module (page);
  gchar   *state = NULL;
  gboolean loading = FALSE;
  gchar   *text = NULL;

  if (page->load_box == NULL)
    {
      g_free (module);
      return;
    }

  if (module != NULL && speechd_module_reads_changes (module))
    {
      gchar *path = speech_page_runtime_path (module, ".state");

      if (g_file_get_contents (path, &state, NULL, NULL))
        {
          g_strstrip (state);
          if (g_str_has_prefix (state, "loading"))
            {
              loading = TRUE;
              text = g_strdup (_("Loading the voice…"));
            }
          else if (g_str_has_prefix (state, "failed"))
            text = g_strdup_printf (_("The voice could not be loaded: %s"),
                                    g_strstrip (state + strlen ("failed")));
        }
      g_free (path);
    }

  if (text != NULL)
    {
      gtk_label_set_text (GTK_LABEL (page->load_label), text);
      gtk_widget_set_visible (page->load_spinner, loading);
      if (loading)
        gtk_spinner_start (GTK_SPINNER (page->load_spinner));
      else
        gtk_spinner_stop (GTK_SPINNER (page->load_spinner));
      gtk_widget_show (page->load_label);
      gtk_widget_show (page->load_box);
    }
  else
    {
      gtk_spinner_stop (GTK_SPINNER (page->load_spinner));
      gtk_widget_hide (page->load_box);
    }

  g_free (text);
  g_free (state);
  g_free (module);
}

static void
speech_page_load_state_changed (GFileMonitor     *monitor,
                                GFile            *file,
                                GFile            *other,
                                GFileMonitorEvent event,
                                SpeechPage       *page)
{
  speech_page_show_load_state (page);
}

void
speech_page_watch_load_state (SpeechPage *page)
{
  gchar *dir = g_build_filename (g_get_user_runtime_dir (), "linux-tts", NULL);

  if (g_mkdir_with_parents (dir, 0700) == 0)
    {
      GFile *file = g_file_new_for_path (dir);

      page->load_monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE,
                                                     NULL, NULL);
      if (page->load_monitor != NULL)
        g_signal_connect (page->load_monitor, "changed",
                          G_CALLBACK (speech_page_load_state_changed), page);
      g_object_unref (file);
    }
  g_free (dir);
}

/* the user chose the synthesizer or its voice: load it now */
void
speech_page_request_load (SpeechPage *page)
{
  gchar *module = speech_page_get_module (page);

  if (module != NULL && speechd_module_reads_changes (module))
    {
      gchar *value = speech_page_preload_value (page, module);
      gchar *path = speech_page_runtime_path (module, ".load");
      gchar *dir = g_path_get_dirname (path);

      if (value != NULL && g_mkdir_with_parents (dir, 0700) == 0)
        {
          gchar *text = g_strconcat (value, "\n", NULL);

          /* a new file each time: the module sees it even if the voice is
           * the same as last time */
          if (g_file_set_contents (path, text, -1, NULL))
            g_hash_table_add (page->load_requested, g_strdup (module));
          g_free (text);
        }
      g_free (dir);
      g_free (path);
      g_free (value);
    }
  g_free (module);
  speech_page_show_load_state (page);
  speech_page_schedule_release (page);
}

/* removes the load requests of the synthesizers no language uses; they
 * free their models */
static void
speech_page_release_unused (SpeechPage *page)
{
  GPtrArray     *used = speechd_config_used_modules (page->config);
  gchar         *current = speech_page_get_module (page);
  GHashTableIter iter;
  gpointer       module;

  g_hash_table_iter_init (&iter, page->load_requested);
  while (g_hash_table_iter_next (&iter, &module, NULL))
    if (!speech_page_array_has (used, module)
        && g_strcmp0 (module, current) != 0)
      {
        gchar *path = speech_page_runtime_path (module, ".load");

        unlink (path);
        g_free (path);
        g_hash_table_iter_remove (&iter);
      }
  g_free (current);
  g_ptr_array_unref (used);
}

static gboolean
speech_page_release_timeout (gpointer data)
{
  SpeechPage *page = data;

  page->release_id = 0;
  speech_page_release_unused (page);
  return G_SOURCE_REMOVE;
}

/* a synthesizer or voice was chosen: in a moment, free the ones switched
 * away from; every choice starts the delay again, so clicking back and
 * forth releases nothing until the user settles */
void
speech_page_schedule_release (SpeechPage *page)
{
  if (page->release_id != 0)
    g_source_remove (page->release_id);
  page->release_id = g_timeout_add_seconds (RELEASE_DELAY_S,
                                            speech_page_release_timeout, page);
}

/* the dialog closes: the synthesizers no language uses free their models */
void
speech_page_release_loads (SpeechPage *page)
{
  GPtrArray     *used = speechd_config_used_modules (page->config);
  GHashTableIter iter;
  gpointer       module;

  if (page->release_id != 0)
    {
      g_source_remove (page->release_id);
      page->release_id = 0;
    }
  g_hash_table_iter_init (&iter, page->load_requested);
  while (g_hash_table_iter_next (&iter, &module, NULL))
    if (!speech_page_array_has (used, module))
      {
        gchar *path = speech_page_runtime_path (module, ".load");

        unlink (path);
        g_free (path);
      }
  g_ptr_array_unref (used);
  g_hash_table_remove_all (page->load_requested);
}


/* Shows the selected synthesizer's state in the check box; with
 * @follow_voice, a preloaded sd_sherpa-onnx preloads the voice just chosen */
void
speech_page_sync_preload_full (SpeechPage *page,
                               gboolean    follow_voice)
{
  gchar   *module = speech_page_get_module (page);
  gchar   *key = NULL;
  gboolean supported = FALSE;
  gboolean on = FALSE;

  if (module != NULL && *module != '\0')
    {
      key = speech_page_module_key (module, "Preload");
      supported = speechd_module_has_option (module, key);
    }

  if (supported)
    {
      gchar *value = speechd_module_get_option (module, key);

      on = speech_page_option_on (value);
      if (on && follow_voice)
        {
          gchar *wanted = speech_page_preload_value (page, module);

          if (wanted != NULL)
            speech_page_set_module_option (page, module, key, wanted);
          g_free (wanted);
        }
      g_free (value);
    }

  if (page->preload_check != NULL)
    {
      gboolean updating = page->updating;

      page->updating = TRUE;
      /* the others load fast enough: nothing to offer */
      gtk_widget_set_visible (page->preload_check, supported);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->preload_check),
                                    on);
      page->updating = updating;
    }

  g_free (key);
  g_free (module);

  speech_page_sync_gpu (page);
  speech_page_sync_text_options (page);
}

void
speech_page_sync_preload (SpeechPage *page)
{
  speech_page_sync_preload_full (page, FALSE);
}

/* "AudioCopyDir" of our modules: a copy of what they speak, outputN.wav
 * and outputN.txt in a directory; no line means no copy */
#define COPY_KEY "AudioCopyDir"

static void
speech_page_sync_copy (SpeechPage *page)
{
  gchar   *module, *value = NULL;
  gboolean supported = FALSE;
  gboolean updating = page->updating;

  if (page->copy_check == NULL)
    return;
  module = speech_page_get_module (page);
  if (module != NULL && *module != '\0')
    supported = speechd_module_has_option (module, COPY_KEY);
  if (supported)
    value = speechd_module_get_option (module, COPY_KEY);

  page->updating = TRUE;
  gtk_widget_set_visible (page->copy_check, supported);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->copy_check),
                                value != NULL && *value != '\0');
  /* the field keeps the last directory while off */
  if (value != NULL && *value != '\0')
    gtk_entry_set_text (GTK_ENTRY (page->copy_entry), value);
  gtk_widget_set_visible (page->copy_box, supported && value != NULL && *value != '\0');
  page->updating = updating;
  g_free (value);
  g_free (module);
}

void
speech_page_sync_text_options (SpeechPage *page)
{
  speech_page_sync_copy (page);
}

/* The directory of the field, as the module takes it: absolute or ~/...;
 * NULL (and a message) for what it would refuse */
static gchar *
speech_page_copy_dir (SpeechPage *page)
{
  gchar *dir = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (page->copy_entry))));

  if (*dir == '\0')
    {
      g_free (dir);
      return g_strdup (COPY_DEFAULT_DIR);
    }
  if (strchr (dir, '"') != NULL || strchr (dir, '\n') != NULL
      || !(g_path_is_absolute (dir) || g_strcmp0 (dir, "~") == 0
           || g_str_has_prefix (dir, "~/")))
    {
      speech_page_show_message (page, GTK_MESSAGE_ERROR,
                                _("The folder for the copy must be a full path, "
                                  "such as ~/speech-copy."));
      g_free (dir);
      return NULL;
    }
  return dir;
}

static void
speech_page_copy_apply (SpeechPage *page)
{
  gchar *module, *dir = NULL;
  gboolean on;

  if (page->updating)
    return;
  module = speech_page_get_module (page);
  if (module == NULL || *module == '\0')
    {
      g_free (module);
      return;
    }
  on = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->copy_check));
  if (on)
    {
      dir = speech_page_copy_dir (page);
      if (dir == NULL)
        {
          g_free (module);
          return;
        }
      page->updating = TRUE;
      gtk_entry_set_text (GTK_ENTRY (page->copy_entry), dir);
      page->updating = FALSE;
    }
  gtk_widget_set_visible (page->copy_box, on);
  speech_page_set_module_option (page, module, COPY_KEY, dir);
  g_free (dir);
  g_free (module);
}

void
speech_page_copy_toggled (GtkToggleButton *button,
                          SpeechPage      *page)
{
  speech_page_copy_apply (page);
}

void
speech_page_copy_entry_activate (GtkEntry   *entry,
                                 SpeechPage *page)
{
  speech_page_copy_apply (page);
}

gboolean
speech_page_copy_entry_focus_out (GtkWidget  *entry,
                                  GdkEvent   *event,
                                  SpeechPage *page)
{
  speech_page_copy_apply (page);
  return FALSE;
}

void
speech_page_copy_browse (GtkButton  *button,
                         SpeechPage *page)
{
  GtkFileChooserNative *chooser;
  gchar *dir;

  chooser = gtk_file_chooser_native_new (_("Folder for the Copy"),
                                         GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                         _("_Select"), _("_Cancel"));
  gtk_file_chooser_set_create_folders (GTK_FILE_CHOOSER (chooser), TRUE);
  dir = speech_page_copy_dir (page);
  if (dir != NULL)
    {
      gchar *path = g_str_has_prefix (dir, "~")
                    ? g_build_filename (g_get_home_dir (), dir + 1, NULL) : g_strdup (dir);

      if (g_file_test (path, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser), path);
      else
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser), g_get_home_dir ());
      g_free (path);
      g_free (dir);
    }
  if (gtk_native_dialog_run (GTK_NATIVE_DIALOG (chooser)) == GTK_RESPONSE_ACCEPT)
    {
      gchar *chosen = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));

      if (chosen != NULL)
        {
          const gchar *home = g_get_home_dir ();
          gsize        len = strlen (home);
          gchar       *shown;

          /* ~/ keeps the setting valid if the home moves */
          if (g_str_has_prefix (chosen, home) && chosen[len] == G_DIR_SEPARATOR)
            shown = g_strconcat ("~", chosen + len, NULL);
          else
            shown = g_strdup (chosen);
          gtk_entry_set_text (GTK_ENTRY (page->copy_entry), shown);
          speech_page_copy_apply (page);
          g_free (shown);
          g_free (chosen);
        }
    }
  g_object_unref (chooser);
}

void
speech_page_preload_toggled (GtkToggleButton *button,
                             SpeechPage      *page)
{
  gchar *module;
  gchar *key, *idle_key;

  if (page->updating)
    return;

  module = speech_page_get_module (page);
  if (module == NULL || *module == '\0')
    {
      g_free (module);
      return;
    }

  key = speech_page_module_key (module, "Preload");
  idle_key = speech_page_module_key (module, "IdleTimeout");
  if (gtk_toggle_button_get_active (button))
    {
      gchar *value = speech_page_preload_value (page, module);

      if (value != NULL)
        {
          speech_page_set_module_option (page, module, key, value);
          speech_page_set_module_option (page, module, idle_key, "0");
        }
      g_free (value);
    }
  else
    {
      speech_page_set_module_option (page, module, key, NULL);
      speech_page_set_module_option (page, module, idle_key, NULL);
    }
  speech_page_unpark (module);

  g_free (idle_key);
  g_free (key);
  g_free (module);

  speech_page_sync_preload (page);
  speech_page_sync_voices_button (page);
}
