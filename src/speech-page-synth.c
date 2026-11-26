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
#include <json-glib/json-glib.h>
#include <signal.h>
#include <speech-dispatcher/libspeechd.h>

#include "speech-page-private.h"
#include "voice-installer.h"

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

  gboolean by_id = strcmp (speechd_module_base (module), "sherpa") == 0
                   || strcmp (speechd_module_base (module), "piper") == 0;

  /* the modules with their own voice line (KokoroVoice "en-US" "…") load the
   * model of the voice chosen for the language; "1" would load the model of
   * whichever voice the module finds first, often another language's */
  if (!by_id
      && speechd_module_voice_setting (module) != SPEECHD_VOICE_SETTING_MODULE)
    return g_strdup ("1");

  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  value = speechd_module_get_voice (module, language, type);
  if (value == NULL && by_id)
    value = g_strdup (gtk_combo_box_get_active_id (
                        GTK_COMBO_BOX (page->voice_combo)));
  if (value == NULL)
    value = g_strdup ("1");
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
#define PENDING_TIMEOUT_MS 2500

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

  /* a request the module has not taken yet counts as loading: the user sees
   * it at once */
  if (!loading && text == NULL && page->pending_id != 0)
    {
      loading = TRUE;
      text = g_strdup (_("Loading the voice…"));
    }

  /* the voice is not there yet: speaking now would wait for it or use the
   * old one */
  if (page->speak_button != NULL)
    gtk_widget_set_sensitive (page->speak_button,
                              !loading && page->pending_id == 0);

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
  gchar *base = g_file_get_basename (file);

  /* the module took the request: from now on its state tells */
  if (page->pending_id != 0 && g_str_has_suffix (base, ".state"))
    {
      g_source_remove (page->pending_id);
      page->pending_id = 0;
    }
  g_free (base);
  speech_page_show_load_state (page);
}

/* the module did not answer: the voice was loaded already */
static gboolean
speech_page_pending_timeout (gpointer data)
{
  SpeechPage *page = data;

  page->pending_id = 0;
  speech_page_show_load_state (page);
  return G_SOURCE_REMOVE;
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

/* The user is switching the engine, the synthesizer or the device: from this
 * moment, before the new module is known, Speak is insensitive and "Loading
 * the voice…" shows. speech_page_request_load () takes it from here (and
 * ends it when the module does not load voices). */
void
speech_page_begin_load (SpeechPage *page)
{
  if (page->pending_id != 0)
    g_source_remove (page->pending_id);
  page->pending_id = g_timeout_add (PENDING_TIMEOUT_MS,
                                    speech_page_pending_timeout, page);
  speech_page_show_load_state (page);
}

/* the user chose the synthesizer or its voice: load it now */
void
speech_page_request_load (SpeechPage *page)
{
  gchar *module = speech_page_get_module (page);

  if ((module == NULL || !speechd_module_reads_changes (module))
      && page->pending_id != 0)
    {
      g_source_remove (page->pending_id);
      page->pending_id = 0;
    }

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
            {
              g_hash_table_add (page->load_requested, g_strdup (module));
              /* the module looks at the request every 2 s (CONFIG_CHECK_MS
               * in linux-tts) and writes .state only when it loads */
              if (page->pending_id != 0)
                g_source_remove (page->pending_id);
              page->pending_id =
                g_timeout_add (PENDING_TIMEOUT_MS,
                               speech_page_pending_timeout, page);
            }
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

/* "TextStressHomographs" of our modules: the stress of Russian homographs
 * from the context, by rules or with a RUAccent model of the packages
 * ruaccent-model-tiny and -turbo (or one the user put in
 * ~/.local/share/linux-tts/ruaccent/<size>) */
#define HOMOGRAPHS_KEY "TextStressHomographs"

static const struct
{
  const gchar *id;
  const gchar *label;
  const gchar *tip;
} homograph_modes[] =
{
  { "none", N_("Most frequent reading"),
    N_("A homograph gets its most frequent stress: за́мок, все.") },
  { "rules", N_("Rules"),
    N_("Rules read the neighbouring words: висячий замо́к, у реки́. No download.") },
  { "tiny", N_("Rules and small model"),
    N_("The rules, then the small RUAccent model for the other homographs "
       "and for ё (все/всё): about 55 MB, fast.") },
  { "turbo", N_("Rules and large model"),
    N_("The rules, then the large RUAccent model: about 380 MB, a little more "
       "accurate, slower.") },
};

static gboolean
homograph_model_installed (const gchar *size)
{
  gchar   *path = g_build_filename (g_get_user_data_dir (), "linux-tts", "ruaccent",
                                    size, "model.onnx", NULL);
  gboolean found = g_file_test (path, G_FILE_TEST_IS_REGULAR);

  g_free (path);
  if (!found)
    {
      /* the ruaccent-model-* packages */
      path = g_build_filename ("/usr/share/linux-tts/ruaccent", size,
                               "model.onnx", NULL);
      found = g_file_test (path, G_FILE_TEST_IS_REGULAR);
      g_free (path);
    }
  return found;
}

static void
speech_page_sync_homographs (SpeechPage *page)
{
  gchar   *module, *language, *value = NULL;
  gboolean supported = FALSE;
  gboolean updating = page->updating;
  guint    i, active = 0;

  if (page->homographs_combo == NULL)
    return;
  module = speech_page_get_module (page);
  /* only for Russian, though the setting belongs to the synthesizer */
  language = speech_page_get_language (page);
  if (module != NULL && *module != '\0' && language != NULL
      && (strcmp (language, "ru") == 0 || g_str_has_prefix (language, "ru-")
          || g_str_has_prefix (language, "ru_")))
    supported = speechd_module_has_option (module, HOMOGRAPHS_KEY);
  if (supported)
    value = speechd_module_get_option (module, HOMOGRAPHS_KEY);

  page->updating = TRUE;
  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (page->homographs_combo));
  for (i = 0; i < G_N_ELEMENTS (homograph_modes); i++)
    {
      const gchar *id = homograph_modes[i].id;
      gchar       *text;

      if (i >= 2 && !homograph_model_installed (id))
        /* TRANSLATORS: a choice whose model is not downloaded yet; %s is the
         * choice, it downloads when chosen */
        text = g_strdup_printf (_("%s (downloads)"), _(homograph_modes[i].label));
      else
        text = g_strdup (_(homograph_modes[i].label));
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (page->homographs_combo), id, text);
      g_free (text);
      if (value != NULL && g_ascii_strcasecmp (value, id) == 0)
        active = i;
    }
  gtk_combo_box_set_active (GTK_COMBO_BOX (page->homographs_combo), active);
  gtk_widget_set_tooltip_text (page->homographs_combo, _(homograph_modes[active].tip));
  if (gtk_widget_get_visible (page->homographs_label) != supported)
    {
      gtk_widget_set_visible (page->homographs_label, supported);
      gtk_widget_set_visible (page->homographs_combo, supported);
      speech_page_resize_groups ();
    }
  page->updating = updating;
  g_free (value);
  g_free (language);
  g_free (module);
}


/* The model of a choice is downloaded by the helper of the package
 * linux-tts-ruaccent-installer when the choice is made; the option is
 * written only after the download is done. */
#define RUACCENT_HELPER "/usr/share/linux-tts-ruaccent-installer/ruaccent-models.py"

static void homographs_read_line (GObject *source, GAsyncResult *res, gpointer data);

static void
homographs_progress_hide (SpeechPage *page)
{
  gtk_widget_hide (page->homographs_box);
  gtk_widget_set_sensitive (page->homographs_combo, TRUE);
  g_clear_object (&page->homographs_proc);
  g_clear_object (&page->homographs_cancellable);
  g_clear_pointer (&page->homographs_pending, g_free);
  g_clear_pointer (&page->homographs_module, g_free);
}

void
speech_page_homographs_free (SpeechPage *page)
{
  if (page->homographs_cancellable != NULL)
    g_cancellable_cancel (page->homographs_cancellable);
  if (page->homographs_proc != NULL)
    g_subprocess_send_signal (page->homographs_proc, SIGTERM);
  g_clear_object (&page->homographs_proc);
  g_clear_object (&page->homographs_cancellable);
  g_clear_pointer (&page->homographs_pending, g_free);
  g_clear_pointer (&page->homographs_module, g_free);
}

void
speech_page_homographs_cancel (GtkButton  *button,
                               SpeechPage *page)
{
  if (page->homographs_proc != NULL)
    {
      page->homographs_cancelled = TRUE;
      gtk_widget_set_sensitive (page->homographs_cancel, FALSE);
      g_subprocess_send_signal (page->homographs_proc, SIGTERM);
    }
}

static void
homographs_failed (SpeechPage *page, const gchar *text)
{
  homographs_progress_hide (page);
  /* back to what the synthesizer has */
  speech_page_sync_homographs (page);
  if (text != NULL)
    speech_page_show_message (page, GTK_MESSAGE_ERROR, text);
}

static const gchar *
homographs_error_text (const gchar *code)
{
  if (g_strcmp0 (code, "space") == 0)
    return _("Not enough free disk space for the model.");
  if (g_strcmp0 (code, "checksum") == 0)
    return _("The downloaded model is damaged (checksum mismatch).");
  if (g_strcmp0 (code, "download") == 0)
    return _("The model could not be downloaded. Check the network connection.");
  if (g_strcmp0 (code, "busy") == 0)
    return _("Another installation is running. Try again when it is done.");
  return _("The model could not be installed.");
}

static void
homographs_handle_line (SpeechPage *page, const gchar *line)
{
  JsonParser *parser = json_parser_new ();
  JsonObject *obj;
  const gchar *stage;

  if (!json_parser_load_from_data (parser, line, -1, NULL)
      || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      g_object_unref (parser);
      return;
    }
  obj = json_node_get_object (json_parser_get_root (parser));
  stage = json_object_has_member (obj, "stage")
    ? json_object_get_string_member (obj, "stage") : NULL;
  if (g_strcmp0 (stage, "download") == 0)
    {
      gdouble done = json_object_has_member (obj, "done") ? json_object_get_double_member (obj, "done") : 0;
      gdouble total = json_object_has_member (obj, "total") ? json_object_get_double_member (obj, "total") : 0;
      gchar  *text;

      if (total > 0)
        {
          gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (page->homographs_bar),
                                         0.95 * MIN (done / total, 1.0));
          text = g_strdup_printf (_("Downloading… %.0f of %.0f MB"),
                                  done / 1048576.0, total / 1048576.0);
        }
      else
        text = g_strdup (_("Downloading…"));
      gtk_label_set_text (GTK_LABEL (page->homographs_status), text);
      g_free (text);
    }
  else if (g_strcmp0 (stage, "convert") == 0 || g_strcmp0 (stage, "check") == 0)
    {
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (page->homographs_bar), 0.97);
      gtk_label_set_text (GTK_LABEL (page->homographs_status), _("Checking…"));
    }
  else if (g_strcmp0 (stage, "error") == 0)
    {
      const gchar *code = json_object_has_member (obj, "code")
        ? json_object_get_string_member (obj, "code") : NULL;

      /* "exists" means the model is already there: not an error */
      if (g_strcmp0 (code, "exists") == 0)
        g_object_set_data (G_OBJECT (page->homographs_status), "exists", GINT_TO_POINTER (1));
      else if (g_strcmp0 (code, "cancelled") != 0)
        {
          g_object_set_data_full (G_OBJECT (page->homographs_status), "err",
                                  g_strdup (homographs_error_text (code)), g_free);
        }
    }
  g_object_unref (parser);
}

static void
homographs_finished (GObject *source, GAsyncResult *res, gpointer data)
{
  SpeechPage *page = data;
  GSubprocess *proc = G_SUBPROCESS (source);
  gboolean ok;
  const gchar *err;

  if (!g_subprocess_wait_finish (proc, res, NULL))
    return;               /* cancelled: the page is closing */
  ok = g_subprocess_get_if_exited (proc) && g_subprocess_get_exit_status (proc) == 0;
  if (page->homographs_proc != proc)
    return;
  err = g_object_get_data (G_OBJECT (page->homographs_status), "err");
  if (!ok && g_object_get_data (G_OBJECT (page->homographs_status), "exists") == NULL
      && !page->homographs_cancelled && err == NULL)
    err = _("The model could not be installed.");
  if (page->homographs_cancelled || (!ok && err != NULL))
    {
      gchar *copy = page->homographs_cancelled ? NULL : g_strdup (err);

      g_object_set_data (G_OBJECT (page->homographs_status), "err", NULL);
      homographs_failed (page, copy);
      g_free (copy);
      return;
    }
  if (homograph_model_installed (page->homographs_pending))
    {
      gchar *module = g_strdup (page->homographs_module);
      gchar *id = g_strdup (page->homographs_pending);

      homographs_progress_hide (page);
      speech_page_set_module_option (page, module, HOMOGRAPHS_KEY, id);
      speech_page_sync_homographs (page);
      g_free (module);
      g_free (id);
    }
  else
    homographs_failed (page, _("The model could not be installed."));
}

static void
homographs_read_line (GObject *source, GAsyncResult *res, gpointer data)
{
  SpeechPage *page = data;
  GDataInputStream *in = G_DATA_INPUT_STREAM (source);
  GError *error = NULL;
  gchar *line = g_data_input_stream_read_line_finish (in, res, NULL, &error);

  if (error != NULL)
    {
      gboolean gone = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

      g_error_free (error);
      g_object_unref (in);
      (void) gone;
      return;
    }
  if (line == NULL)
    {
      g_object_unref (in);
      return;
    }
  homographs_handle_line (page, line);
  g_free (line);
  g_data_input_stream_read_line_async (in, G_PRIORITY_DEFAULT,
                                       page->homographs_cancellable,
                                       homographs_read_line, page);
}

static void
homographs_start (SpeechPage *page, const gchar *module, const gchar *id)
{
  const gchar *helper = g_getenv ("SPEECH_RUACCENT_HELPER");
  GSubprocess *proc;
  GError      *error = NULL;
  GDataInputStream *out;

  if (helper == NULL || *helper == '\0')
    helper = RUACCENT_HELPER;
  if (page->homographs_proc != NULL)
    return;
  if (!g_file_test (helper, G_FILE_TEST_IS_REGULAR))
    {
      speech_page_sync_homographs (page);
      speech_page_show_message (page, GTK_MESSAGE_ERROR,
                                _("The model installer is missing. Install the "
                                  "package linux-tts-ruaccent-installer."));
      return;
    }
  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                           &error, "python3", "-I", helper, "install", id, NULL);
  if (proc == NULL)
    {
      speech_page_sync_homographs (page);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, error->message);
      g_error_free (error);
      return;
    }
  page->homographs_proc = proc;
  page->homographs_cancelled = FALSE;
  page->homographs_pending = g_strdup (id);
  page->homographs_module = g_strdup (module);
  page->homographs_cancellable = g_cancellable_new ();
  g_object_set_data (G_OBJECT (page->homographs_status), "err", NULL);
  g_object_set_data (G_OBJECT (page->homographs_status), "exists", NULL);
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (page->homographs_bar), 0.0);
  gtk_label_set_text (GTK_LABEL (page->homographs_status), _("Starting…"));
  gtk_widget_set_sensitive (page->homographs_cancel, TRUE);
  gtk_widget_set_sensitive (page->homographs_combo, FALSE);
  gtk_widget_show_all (page->homographs_box);
  out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
  g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (out), G_PRIORITY_DEFAULT,
                                       page->homographs_cancellable,
                                       homographs_read_line, page);
  g_object_unref (out);
  g_subprocess_wait_async (proc, page->homographs_cancellable, homographs_finished, page);
}

void
speech_page_homographs_changed (GtkComboBox *combo,
                                SpeechPage  *page)
{
  const gchar *id = gtk_combo_box_get_active_id (combo);
  gchar       *module;
  guint        i;

  if (page->updating || id == NULL)
    return;
  module = speech_page_get_module (page);
  if (module == NULL || *module == '\0')
    {
      g_free (module);
      return;
    }
  for (i = 0; i < G_N_ELEMENTS (homograph_modes); i++)
    if (strcmp (homograph_modes[i].id, id) == 0)
      gtk_widget_set_tooltip_text (GTK_WIDGET (combo), _(homograph_modes[i].tip));
  if ((strcmp (id, "tiny") == 0 || strcmp (id, "turbo") == 0)
      && !homograph_model_installed (id))
    {
      homographs_start (page, module, id);
      g_free (module);
      return;
    }
  /* "none" is the default: no line */
  speech_page_set_module_option (page, module, HOMOGRAPHS_KEY,
                                 strcmp (id, "none") == 0 ? NULL : id);
  g_free (module);
}

void
speech_page_sync_text_options (SpeechPage *page)
{
  speech_page_sync_copy (page);
  speech_page_sync_homographs (page);
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
}
