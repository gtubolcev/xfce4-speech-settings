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

/* The modules of the server, the languages they speak and their voices. */

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

/* a module still starting lists no voices: ask again this often, this many
 * times (sherpa-onnx with a preloaded model needs several seconds) */
#define RETRY_DELAY_MS 1000
#define RETRY_MAX 20

/* the order matches SPDVoiceType, starting at SPD_MALE1 */
const Choice speech_page_voice_types[N_VOICE_TYPES] =
{
  { "MALE1", N_("Male 1") },
  { "MALE2", N_("Male 2") },
  { "MALE3", N_("Male 3") },
  { "FEMALE1", N_("Female 1") },
  { "FEMALE2", N_("Female 2") },
  { "FEMALE3", N_("Female 3") },
  { "CHILD_MALE", N_("Child, male") },
  { "CHILD_FEMALE", N_("Child, female") },
};

/* modules Speech Dispatcher always adds itself, not a real choice, and the
 * legacy engines, whose voices are no choice next to the others: eSpeak
 * (user, 2026-11-11), Festival, Flite and Pico (user, 2026-11-12) */
static const gchar *hidden_modules[] = { "dummy", "espeak-ng-fallback",
                                         "espeak", "espeak-ng", "festival",
                                         "flite", "pico", NULL };

static gboolean
speech_page_same_language (const gchar *a,
                           const gchar *b);
static gint
speech_page_compare_strings (gconstpointer a,
                             gconstpointer b);



/* Whether @module has a voice for @language; a module that does not list
 * its voices may have one */
static gboolean
speech_page_module_speaks (SpeechPage  *page,
                           const gchar *module,
                           const gchar *language)
{
  GPtrArray *languages;
  guint      i;

  languages = g_hash_table_lookup (page->module_languages, module);
  if (languages == NULL || languages->len == 0)
    return TRUE;

  for (i = 0; i < languages->len; i++)
    if (speech_page_same_language (g_ptr_array_index (languages, i), language))
      return TRUE;

  return FALSE;
}



gboolean
speech_page_array_has (GPtrArray   *array,
                       const gchar *text)
{
  guint i;

  for (i = 0; i < array->len; i++)
    if (strcmp (g_ptr_array_index (array, i), text) == 0)
      return TRUE;

  return FALSE;
}



/* The socket of the server; a new one means the server was restarted (for
 * example to pick up a newly installed module). Only the default address. */
gboolean
speech_page_server_stat (struct stat *st)
{
  gchar   *path;
  gboolean ok;

  if (g_getenv ("SPEECHD_ADDRESS") != NULL)
    return FALSE;
  path = g_build_filename (g_get_user_runtime_dir (), "speech-dispatcher",
                           "speechd.sock", NULL);
  ok = stat (path, st) == 0;
  g_free (path);
  return ok;
}



/* Asks every module for its voices, to know the languages it speaks. The
 * connection is opened anew afterwards, so it uses the default module again. */
void
speech_page_scan_modules (SpeechPage *page)
{
  guint i;
  guint j;

  g_clear_pointer (&page->modules, g_strfreev);
  g_hash_table_remove_all (page->module_languages);
  g_ptr_array_set_size (page->languages, 0);

  if (!speech_page_connect (page))
    return;

  {
    struct stat st;

    if (speech_page_server_stat (&st))
      {
        page->server_dev = st.st_dev;
        page->server_ino = st.st_ino;
      }
  }

  page->modules = spd_list_modules (page->connection);

  for (i = 0; page->modules != NULL && page->modules[i] != NULL; i++)
    {
      const gchar *module = page->modules[i];
      GPtrArray   *languages;
      SPDVoice   **voices;
      gchar       *base;

      if (g_strv_contains (hidden_modules, module)
          || spd_set_output_module (page->connection, module) != 0)
        continue;

      voices = spd_list_synthesis_voices (page->connection);
      languages = g_ptr_array_new_with_free_func (g_free);
      for (j = 0; voices != NULL && voices[j] != NULL; j++)
        {
          const gchar *language = voices[j]->language;

          if (language == NULL || *language == '\0'
              || speech_page_array_has (languages, language))
            continue;
          g_ptr_array_add (languages, g_strdup (language));

          /* one entry per language: "ru" for "ru", "ru-RU" and "ru_RU";
           * the voices of a language are matched the same way */
          base = g_ascii_strdown (language, strcspn (language, "-_"));
          if (!speech_page_array_has (page->languages, base))
            g_ptr_array_add (page->languages, base);
          else
            g_free (base);
        }
      if (voices != NULL)
        free_spd_voices (voices);

      g_hash_table_insert (page->module_languages, g_strdup (module),
                           languages);
    }

  g_ptr_array_sort (page->languages, speech_page_compare_strings);

  spd_close (page->connection);
  page->connection = NULL;
  speech_page_connect (page);
}



/* Writes @module ("" for none) as the synthesizer of the selected language:
 * its LanguageDefaultModule line, and DefaultModule when it is the default
 * language (texts and clients without a language) */
void
speech_page_store_module (SpeechPage  *page,
                          const gchar *module)
{
  gchar *language = speech_page_get_language (page);
  gchar *default_language = speechd_config_get (page->config, "DefaultLanguage");

  if (language == NULL || *language == '\0'
      || speech_page_same_language (language, default_language != NULL
                                               ? default_language
                                               : DEFAULT_LANGUAGE))
    {
      if (*module == '\0')
        speechd_config_unset (page->config, "DefaultModule");
      else
        speechd_config_set (page->config, "DefaultModule", module);
    }

  if (language != NULL && *language != '\0')
    speechd_config_set_language_module (page->config, language,
                                        *module != '\0' ? module : NULL);

  g_free (default_language);
  g_free (language);

  speech_page_park_preloads (page);
}



/* Offers the modules that speak the selected language, and the selected one */
void
speech_page_fill_modules (SpeechPage *page)
{
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT (page->module_combo);
  gchar           *current;
  gchar           *language;
  gchar           *label;
  const gchar     *first = NULL;
  gboolean         found = FALSE;
  guint            i;

  language = speech_page_get_language (page);
  current = NULL;
  if (language != NULL && *language != '\0')
    current = speechd_config_get_language_module (page->config, language);
  if (current == NULL)
    current = speechd_config_get (page->config, "DefaultModule");

  gtk_combo_box_text_remove_all (combo);

  for (i = 0; page->modules != NULL && page->modules[i] != NULL; i++)
    {
      const gchar *module = page->modules[i];
      gchar       *cpu = speechd_module_cpu_variant (module);
      gchar       *gpu;
      const gchar *name = module;

      if (g_strv_contains (hidden_modules, module))
        {
          g_free (cpu);
          continue;
        }
      /* a graphics card variant is no entry of its own: the check box under
       * its synthesizer picks it, and the entry of that one stands for it */
      if (cpu != NULL && g_strv_contains ((const gchar *const *) page->modules, cpu))
        {
          g_free (cpu);
          continue;
        }
      g_free (cpu);
      gpu = speech_page_gpu_twin (page, module);
      if (gpu != NULL && g_strcmp0 (gpu, current) == 0)
        module = gpu;
      /* sherpa-onnx, the module of the linux-tts-* packages, before the
       * others, which are heavier or need their model installed by hand */
      if ((first == NULL || g_strcmp0 (name, "sherpa-onnx") == 0)
          && speech_page_module_speaks (page, module, language))
        first = module;
      if (g_strcmp0 (module, current) == 0)
        found = TRUE;
      else if (!speech_page_module_speaks (page, module, language))
        {
          g_free (gpu);
          continue;
        }

      if (speech_page_module_speaks (page, module, language))
        gtk_combo_box_text_append (combo, module, name);
      else
        {
          /* TRANSLATORS: a synthesizer without a voice for the language */
          label = g_strdup_printf (_("%s (no voice)"), name);
          gtk_combo_box_text_append (combo, module, label);
          g_free (label);
        }
      g_free (gpu);
    }

  /* keep a configured module that is not loaded at the moment */
  if (current != NULL && *current != '\0' && !found)
    gtk_combo_box_text_append (combo, current, current);

  /* Without a module for the language Speech Dispatcher would use
   * espeak-ng, which is hidden here, or a module without a voice for it,
   * which reads the text with a voice of another language. Take the first
   * synthesizer that speaks the language instead, also in the
   * configuration, so the dialog shows what is used. */
  if (first != NULL
      && (current == NULL || *current == '\0'
          || !speech_page_module_speaks (page, current, language)))
    {
      g_free (current);
      current = g_strdup (first);
      speech_page_store_module (page, current);
      if (speechd_config_is_dirty (page->config))
        speech_page_queue_save (page);
    }

  if (current != NULL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), current);

  g_free (language);
  g_free (current);
}



static gint
speech_page_compare_strings (gconstpointer a,
                             gconstpointer b)
{
  return g_strcmp0 (*(const gchar **) a, *(const gchar **) b);
}



/* Offers the languages of all modules */
void
speech_page_fill_languages (SpeechPage *page)
{
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT (page->language_combo);
  guint            i;

  gtk_combo_box_text_remove_all (combo);
  for (i = 0; i < page->languages->len; i++)
    gtk_combo_box_text_append_text (combo,
                                    g_ptr_array_index (page->languages, i));
}



/* Asks the selected module for its voices */
void
speech_page_load_voices (SpeechPage *page)
{
  const gchar *module;

  if (page->voices != NULL)
    free_spd_voices (page->voices);
  page->voices = NULL;

  if (page->connection == NULL)
    return;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    spd_set_output_module (page->connection, module);

  page->voices = spd_list_synthesis_voices (page->connection);
}


/* No voices from the selected module: it is most likely still starting after
 * a reload (ours or anyone's). Read everything again in a moment, as the
 * module list and languages came from the same half-started server. */
void
speech_page_retry_if_empty (SpeechPage *page)
{
  if (page->connection == NULL
      || (page->voices != NULL && page->voices[0] != NULL))
    {
      page->refill_tries = 0;
      return;
    }
  if (page->refill_id != 0 || page->refill_tries >= RETRY_MAX)
    return;
  page->refill_tries++;
  page->refill_id = g_timeout_add (RETRY_DELAY_MS,
                                   speech_page_refill_modules, page);
}



static gboolean
speech_page_same_language (const gchar *a,
                           const gchar *b)
{
  gsize len = strcspn (a, "-_");

  return len == strcspn (b, "-_") && g_ascii_strncasecmp (a, b, len) == 0;
}



/* The voice of @voices that a voice type like "FEMALE2" selects for
 * @language, or NULL when the module does not tell.  Modules report either
 * the gender ("male"/"female", sd_zonos3: the n-th voice of that gender,
 * else the n-th voice) or the voice type a voice serves (the generic
 * modules' AddVoice: that type, else MALE1, else the first voice). */
static const gchar *
speech_page_resolve_voice (SPDVoice   **voices,
                           const gchar *language,
                           const gchar *type)
{
  GPtrArray   *candidates;
  const gchar *gender = NULL;
  const gchar *rest = type;
  const gchar *found = NULL;
  gboolean     by_gender = FALSE;
  gboolean     by_type = FALSE;
  guint        index = 0;
  guint        i, t;

  if (voices == NULL)
    return NULL;

  candidates = g_ptr_array_new ();
  for (i = 0; voices[i] != NULL; i++)
    {
      const gchar *variant = voices[i]->variant;

      if (voices[i]->language == NULL
          || !speech_page_same_language (voices[i]->language, language))
        continue;
      g_ptr_array_add (candidates, voices[i]);
      if (variant == NULL)
        continue;
      if (g_ascii_strcasecmp (variant, "male") == 0
          || g_ascii_strcasecmp (variant, "female") == 0)
        by_gender = TRUE;
      for (t = 0; t < G_N_ELEMENTS (speech_page_voice_types); t++)
        if (g_ascii_strcasecmp (variant, speech_page_voice_types[t].id) == 0)
          by_type = TRUE;
    }

  if (g_ascii_strncasecmp (rest, "CHILD_", 6) == 0)
    rest += 6;
  if (g_ascii_strncasecmp (rest, "FEMALE", 6) == 0)
    gender = "female", rest += 6;
  else if (g_ascii_strncasecmp (rest, "MALE", 4) == 0)
    gender = "male", rest += 4;
  if (g_ascii_isdigit (*rest) && atoi (rest) > 0)
    index = atoi (rest) - 1;

  /* sd_zonos3 without genders: the n-th voice of the language */
  if (candidates->len > 0 && !by_gender && !by_type)
    found = ((SPDVoice *) g_ptr_array_index (candidates,
                                             index % candidates->len))->name;
  else if (candidates->len > 0 && by_gender)
    {
      GPtrArray *same = g_ptr_array_new ();

      for (i = 0; i < candidates->len; i++)
        {
          SPDVoice *voice = g_ptr_array_index (candidates, i);

          if (voice->variant != NULL && gender != NULL
              && g_ascii_strcasecmp (voice->variant, gender) == 0)
            g_ptr_array_add (same, voice);
        }
      if (same->len == 0)
        g_ptr_array_extend (same, candidates, NULL, NULL);
      found = ((SPDVoice *) g_ptr_array_index (same, index % same->len))->name;
      g_ptr_array_free (same, TRUE);
    }
  else if (candidates->len > 0 && by_type)
    {
      const gchar *tries[] = { type, "MALE1" };

      for (t = 0; t < G_N_ELEMENTS (tries) && found == NULL; t++)
        for (i = 0; i < candidates->len && found == NULL; i++)
          {
            SPDVoice *voice = g_ptr_array_index (candidates, i);

            if (voice->variant != NULL
                && g_ascii_strcasecmp (voice->variant, tries[t]) == 0)
              found = voice->name;
          }
      if (found == NULL)
        found = ((SPDVoice *) g_ptr_array_index (candidates, 0))->name;
    }

  g_ptr_array_free (candidates, TRUE);

  return found;
}



/* The graphics card variant of @module when the server has it, else NULL */
gchar *
speech_page_gpu_twin (SpeechPage  *page,
                      const gchar *module)
{
  gchar *gpu = speechd_module_gpu_variant (module);

  if (gpu != NULL && page->modules != NULL
      && g_strv_contains ((const gchar *const *) page->modules, gpu))
    return gpu;

  g_free (gpu);
  return NULL;
}

/* Whether the computer has a graphics card a Vulkan driver may run on: a
 * render node, or the device of the NVIDIA driver. Asking Vulkan itself
 * would load every driver into the dialog; a card without Vulkan makes the
 * variant fail to load, which the dialog shows. */
static gboolean
speech_page_has_gpu (void)
{
  static gint  cached = -1;
  GDir        *dir;
  const gchar *name;

  if (cached >= 0)
    return cached;
  cached = g_file_test ("/dev/nvidia0", G_FILE_TEST_EXISTS);
  dir = g_dir_open ("/dev/dri", 0, NULL);
  while (!cached && dir != NULL && (name = g_dir_read_name (dir)) != NULL)
    cached = g_str_has_prefix (name, "renderD");
  if (dir != NULL)
    g_dir_close (dir);
  if (g_getenv ("SPEECH_SETTINGS_FAKE_GPU") != NULL)
    cached = TRUE;

  return cached;
}

/* Shows the check box "Use the graphics card" when the selected synthesizer
 * has a variant for it with a voice for the language, on while it is used */
void
speech_page_sync_gpu (SpeechPage *page)
{
  gchar   *module, *cpu, *gpu, *language;
  gboolean visible = FALSE, on = FALSE, updating = page->updating;

  if (page->gpu_check == NULL)
    return;
  module = speech_page_get_module (page);
  cpu = speechd_module_cpu_variant (module);
  language = speech_page_get_language (page);
  if (cpu != NULL && g_strv_contains ((const gchar *const *) page->modules, cpu))
    visible = on = TRUE;
  else if (speech_page_has_gpu ()
           && (gpu = speech_page_gpu_twin (page, module)) != NULL)
    {
      visible = speech_page_module_speaks (page, gpu, language);
      g_free (gpu);
    }

  page->updating = TRUE;
  gtk_widget_set_visible (page->gpu_check, visible);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->gpu_check), on);
  page->updating = updating;
  g_free (language);
  g_free (cpu);
  g_free (module);
}



/* The module the dialog configures: the chosen one, else the default */
gchar *
speech_page_get_module (SpeechPage *page)
{
  const gchar *module;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    return g_strdup (module);

  return speechd_config_get (page->config, "DefaultModule");
}



gchar *
speech_page_get_language (SpeechPage *page)
{
  GtkWidget   *entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  const gchar *language = gtk_entry_get_text (GTK_ENTRY (entry));

  if (language == NULL || *language == '\0')
    language = DEFAULT_LANGUAGE;

  return g_strstrip (g_strdup (language));
}



/* The voice type the server asks the modules for */
gchar *
speech_page_get_voice_type (SpeechPage *page)
{
  gchar *type = speechd_config_get (page->config, "DefaultVoiceType");

  if (type == NULL || *type == '\0')
    {
      g_free (type);
      type = g_strdup ("MALE1");
    }

  return type;
}



/* The voice of @name for @language in the module's voice list, or NULL */
SPDVoice *
speech_page_find_voice (SpeechPage  *page,
                        const gchar *name,
                        const gchar *language)
{
  guint i;

  for (i = 0; page->voices != NULL && page->voices[i] != NULL; i++)
    if (g_strcmp0 (page->voices[i]->name, name) == 0
        && page->voices[i]->language != NULL
        && speech_page_same_language (page->voices[i]->language, language))
      return page->voices[i];

  return NULL;
}



/* Lists the voices the selected synthesizer has for the selected language
 * and selects the one it speaks with: the voice set in its configuration,
 * else the one the voice type selects. Synthesizers without a voice setting
 * keep the voice type list. */
void
speech_page_update_voices (SpeechPage *page)
{
  GtkComboBoxText    *combo;
  SpeechdVoiceSetting setting;
  GHashTable         *seen;
  gchar              *module;
  gchar              *language;
  gchar              *type;
  gchar              *active = NULL;
  const gchar        *voice_language = NULL;
  gboolean            updating = page->updating;
  guint               n = 0;
  guint               i;

  if (page->voice_combo == NULL)
    return;
  combo = GTK_COMBO_BOX_TEXT (page->voice_combo);

  module = speech_page_get_module (page);
  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  setting = speechd_module_voice_setting (module);

  page->updating = TRUE;
  gtk_combo_box_text_remove_all (combo);

  seen = g_hash_table_new (g_str_hash, g_str_equal);
  for (i = 0; page->voices != NULL && page->voices[i] != NULL; i++)
    {
      SPDVoice *voice = page->voices[i];

      if (voice->name == NULL || voice->language == NULL
          || !speech_page_same_language (voice->language, language)
          || g_hash_table_contains (seen, voice->name))
        continue;
      g_hash_table_add (seen, voice->name);
      gtk_combo_box_text_append (combo, voice->name, voice->name);
      /* an exact match of the language wins, "en" for "en-US" otherwise */
      if (voice_language == NULL
          || g_ascii_strcasecmp (voice->language, language) == 0)
        voice_language = voice->language;
      n++;
    }
  g_hash_table_destroy (seen);

  if (voice_language != NULL)
    active = speechd_module_get_voice (module, voice_language, type);
  /* a configured voice that is gone: the module falls back to the type */
  if (active != NULL && speech_page_find_voice (page, active, language) == NULL)
    g_clear_pointer (&active, g_free);
  if (active == NULL && setting != SPEECHD_VOICE_SETTING_NONE)
    active = g_strdup (speech_page_resolve_voice (page->voices, language,
                                                  type));
  if (active != NULL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), active);

  gtk_widget_set_sensitive (page->voice_combo,
                            n > 0 && setting != SPEECHD_VOICE_SETTING_NONE);
  if (n == 0)
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("The synthesizer has no voice for this "
                                   "language"));
  else if (setting == SPEECHD_VOICE_SETTING_NONE)
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("This synthesizer chooses its voice by "
                                   "the voice type"));
  else
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("The voice for this language, kept in the "
                                   "synthesizer's settings"));

  /* only the synthesizers without a voice setting need the voice type */
  gtk_widget_set_visible (page->voice_type_label,
                          setting == SPEECHD_VOICE_SETTING_NONE);
  gtk_widget_set_visible (page->voice_type_combo,
                          setting == SPEECHD_VOICE_SETTING_NONE);

  page->updating = updating;
  g_free (active);
  g_free (type);
  g_free (language);
  g_free (module);
}



/* TRUE for a language code like "ru", "pt-BR" or "zh_Hant": two or three
 * letters, then parts of letters and digits joined by "-" or "_" */
gboolean
speech_page_is_language_code (const gchar *text)
{
  return g_regex_match_simple ("^[A-Za-z]{2,3}([-_][A-Za-z0-9]{1,8})*$",
                               text, 0, 0);
}



/* The language of the desktop as a speechd code ("ru", "de"), or NULL
 * for C/POSIX */
gchar *
speech_page_locale_language (void)
{
  const gchar * const *names = g_get_language_names ();
  gchar               *code;
  gsize                n;

  if (names == NULL || names[0] == NULL)
    return NULL;
  /* the language alone: clients send "ru" more often than "ru-RU", and
   * a LanguageDefaultModule line for "ru" is found for both */
  n = strcspn (names[0], "_.@");
  code = g_strndup (names[0], n);
  if (g_strcmp0 (code, "C") == 0 || g_strcmp0 (code, "POSIX") == 0 || *code == '\0')
    {
      g_free (code);
      return NULL;
    }
  return code;
}



/* The modules restart after the reload; ask for the list once they are up. */
gboolean
speech_page_refill_modules (gpointer data)
{
  SpeechPage *page = data;

  page->refill_id = 0;

  if (page->connection != NULL)
    {
      spd_close (page->connection);
      page->connection = NULL;
    }

  page->updating = TRUE;
  speech_page_scan_modules (page);
  speech_page_fill_modules (page);
  speech_page_load_voices (page);
  {
    /* the voices of the new connection; refilling must not clear the language */
    GtkWidget *entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
    gchar     *language = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));

    speech_page_fill_languages (page);
    gtk_entry_set_text (GTK_ENTRY (entry), language);
    g_free (language);
  }
  page->updating = FALSE;
  speech_page_update_voices (page);
  speech_page_retry_if_empty (page);

  return G_SOURCE_REMOVE;
}



/* The list of modules is read when the window opens; when the server was
 * restarted meanwhile (a module installed or removed), read it again once the
 * window is used */
/* A module that starts slowly (sherpa-onnx-vulkan-rs sets up the GPU) joins
 * the server's list after the others; a scan made in between, after a
 * reload or a package update, misses it. The list costs one short request:
 * compare it every few seconds and read everything again when it changed,
 * not while the synthesizer list is open. */
gboolean
speech_page_watch_modules (gpointer data)
{
  SpeechPage *page = data;
  gboolean    shown = FALSE;
  gchar     **modules;
  guint       i;

  if (page->connection == NULL || page->refill_id != 0 || page->updating)
    return G_SOURCE_CONTINUE;

  g_object_get (page->module_combo, "popup-shown", &shown, NULL);
  if (shown)
    return G_SOURCE_CONTINUE;

  modules = spd_list_modules (page->connection);
  if (modules == NULL)
    return G_SOURCE_CONTINUE;

  for (i = 0; modules[i] != NULL && page->modules != NULL
              && page->modules[i] != NULL; i++)
    if (strcmp (modules[i], page->modules[i]) != 0)
      break;

  if (page->modules == NULL || modules[i] != NULL || page->modules[i] != NULL)
    page->refill_id = g_idle_add (speech_page_refill_modules, page);

  g_strfreev (modules);
  return G_SOURCE_CONTINUE;
}
