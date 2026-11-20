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
#include <gtk/gtk.h>
#endif
#include <speech-dispatcher/libspeechd.h>

#include "speech-page.h"
#include "speechd-config.h"
#include "voice-installer.h"

#define SAVE_DELAY_MS 400
#define LANGUAGE_DELAY_MS 500
/* time the server needs to restart its modules after a reload */
#define REFILL_DELAY_MS 1500
/* a module still starting lists no voices: ask again this often, this many
 * times (sherpa-onnx with a preloaded model needs several seconds) */
#define RETRY_DELAY_MS 1000
#define RETRY_MAX 20
/* how often the module list is compared with the server's */
#define WATCH_DELAY_MS 2000
/* Built-in defaults of Speech Dispatcher, used when a key is not set */
#define DEFAULT_LANGUAGE "en-US"
/* the test text grows up to this many lines, then scrolls */
#define TEST_MAX_LINES 8

typedef struct
{
  const gchar *id;
  const gchar *label;
} Choice;

/* the order matches SPDVoiceType, starting at SPD_MALE1 */
static const Choice voice_types[] =
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

static const Choice punctuation_modes[] =
{
  { "none", N_("None") },
  { "some", N_("Some") },
  { "most", N_("Most") },
  { "all", N_("All") },
};

static const SPDPunctuation punctuation_values[] =
{
  SPD_PUNCT_NONE, SPD_PUNCT_SOME, SPD_PUNCT_MOST, SPD_PUNCT_ALL,
};

static const Choice capital_modes[] =
{
  { "none", N_("Do nothing") },
  { "spell", N_("Say “capital”") },
  { "icon", N_("Play a sound") },
};

static const SPDCapitalLetters capital_values[] =
{
  SPD_CAP_NONE, SPD_CAP_SPELL, SPD_CAP_ICON,
};

/* an empty id leaves the choice to Speech Dispatcher */
static const Choice audio_methods[] =
{
  { "", N_("Automatic") },
  { "pipewire", "PipeWire" },
  { "pulse", "PulseAudio" },
  { "alsa", "ALSA" },
  { "oss", "OSS" },
  { "nas", "NAS" },
  { "libao", "libao" },
};

/* modules Speech Dispatcher always adds itself, not a real choice, and the
 * legacy engines, whose voices are no choice next to the others: eSpeak
 * (user, 2026-11-11), Festival, Flite and Pico (user, 2026-11-12) */
static const gchar *hidden_modules[] = { "dummy", "espeak-ng-fallback",
                                         "espeak", "espeak-ng", "festival",
                                         "flite", "pico", NULL };

/* The test sentence in the language that is spoken, not in the language of
 * the dialog, so these are not translated. */
static const struct
{
  const gchar *language;
  const gchar *text;
}
test_texts[] =
{
  { "ar", "هكذا يتكلم حاسوبك." },
  { "be", "Так гаворыць ваш камп’ютар." },
  { "bg", "Ето как говори вашият компютър." },
  { "ca", "Així parla el vostre ordinador." },
  { "cs", "Takhle mluví váš počítač." },
  { "da", "Sådan taler din computer." },
  { "de", "So spricht Ihr Computer." },
  { "el", "Έτσι μιλάει ο υπολογιστής σας." },
  { "en", "This is how your computer speaks." },
  { "es", "Así habla tu ordenador." },
  { "et", "Nii räägib teie arvuti." },
  { "fi", "Näin tietokoneesi puhuu." },
  { "fr", "Voici comment parle votre ordinateur." },
  { "hi", "आपका कंप्यूटर ऐसे बोलता है।" },
  { "hr", "Ovako govori vaše računalo." },
  { "hu", "Így beszél a számítógéped." },
  { "id", "Beginilah komputer Anda berbicara." },
  { "it", "Così parla il tuo computer." },
  { "ja", "これがあなたのコンピューターの声です。" },
  { "kk", "Компьютеріңіз осылай сөйлейді." },
  { "ko", "컴퓨터가 이렇게 말합니다." },
  { "lt", "Taip kalba jūsų kompiuteris." },
  { "lv", "Tā runā jūsu dators." },
  { "nb", "Slik snakker datamaskinen din." },
  { "nl", "Zo spreekt je computer." },
  { "pl", "Tak mówi twój komputer." },
  { "pt", "É assim que o seu computador fala." },
  { "ro", "Așa vorbește computerul dumneavoastră." },
  { "ru", "Так говорит ваш компьютер." },
  { "sk", "Takto hovorí váš počítač." },
  { "sl", "Tako govori vaš računalnik." },
  { "sr", "Овако говори ваш рачунар." },
  { "sv", "Så här talar din dator." },
  { "tr", "Bilgisayarınız böyle konuşur." },
  { "uk", "Так говорить ваш комп’ютер." },
  { "vi", "Đây là cách máy tính của bạn nói." },
  { "zh", "这就是你的电脑说话的声音。" },
};

typedef struct
{
  SpeechdConfig *config;
  SPDConnection *connection;

  GtkWidget     *info_bar;
  GtkWidget     *info_label;
  GtkWidget     *modules_bar;

  GtkWidget     *module_combo;
  GtkWidget     *language_combo;
  GtkWidget     *voice_combo;
  GtkWidget     *voice_type_label;
  GtkWidget     *voice_type_combo;
  GtkWidget     *preload_check;
  GtkWidget     *voices_button;
  /* the user wants the default synthesizer kept in memory */
  /* the module whose configuration has our Preload line, or NULL */
  /* the voices of the selected module, NULL-terminated, or NULL */
  SPDVoice     **voices;
  /* the modules of the server, and the languages each one has voices for */
  gchar        **modules;
  GHashTable    *module_languages;
  /* the languages of all modules, sorted */
  GPtrArray     *languages;
  GtkWidget     *rate_spin;
  GtkWidget     *pitch_spin;
  GtkWidget     *volume_spin;
  GtkWidget     *punctuation_combo;
  GtkWidget     *capital_combo;
  GtkWidget     *audio_combo;
  GtkWidget     *normalize_check;   /* TextNormalization of the synthesizer */
  GtkWidget     *stress_check;      /* TextStress of the synthesizer */
  GtkWidget     *copy_check;        /* AudioCopyDir of the synthesizer */
  GtkWidget     *copy_box;          /* the directory, shown when on */
  GtkWidget     *copy_entry;
  GtkWidget     *test_view;     /* GtkTextView, grows with its text */
  GtkWidget     *test_grid;
  /* the sentence put into test_view for a language, while the user keeps it */
  const gchar   *test_text;

  guint          save_id;
  guint          language_id;
  gchar         *typed_language;
  guint          refill_id;
  guint          refill_tries;  /* refills in a row that found no voices */
  guint          watch_id;
  guint          fit_id;        /* fits test_view's height to its text */
  /* the server the module list came from (its socket), see
   * speech_page_server_changed */
  dev_t          server_dev;
  ino_t          server_ino;
  /* a module configuration was written, the server has to read it */
  gboolean       reload_pending;
  gboolean       updating;
} SpeechPage;



static void
speech_page_show_message (SpeechPage     *page,
                          GtkMessageType  type,
                          const gchar    *message)
{
  gtk_info_bar_set_message_type (GTK_INFO_BAR (page->info_bar), type);
  gtk_label_set_text (GTK_LABEL (page->info_label), message);
  gtk_widget_show (page->info_bar);
}



static gboolean
speech_page_save (gpointer data)
{
  SpeechPage *page = data;
  GError     *error = NULL;
  gchar      *message;

  page->save_id = 0;

  if (!speechd_config_is_dirty (page->config) && !page->reload_pending)
    return G_SOURCE_REMOVE;
  page->reload_pending = FALSE;

  if (!speechd_config_save (page->config, &error)
      || !speechd_server_reload (&error))
    {
      message = g_strdup_printf (_("Failed to apply the settings: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
    }

  return G_SOURCE_REMOVE;
}



static void
speech_page_queue_save (SpeechPage *page)
{
  if (page->save_id != 0)
    g_source_remove (page->save_id);
  page->save_id = g_timeout_add (SAVE_DELAY_MS, speech_page_save, page);
}



static GtkWidget *
speech_page_combo_new (const Choice *choices,
                       guint         n_choices)
{
  GtkWidget *combo;
  guint      i;

  combo = gtk_combo_box_text_new ();
  for (i = 0; i < n_choices; i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), choices[i].id,
                               _(choices[i].label));

  return combo;
}



/* Selects the entry with @value (case-insensitive like Speech Dispatcher),
 * or @fallback when there is none. */
static void
speech_page_combo_select (GtkWidget    *combo,
                          const Choice *choices,
                          guint         n_choices,
                          const gchar  *value,
                          const gchar  *fallback)
{
  guint i;

  for (i = 0; value != NULL && i < n_choices; i++)
    if (g_ascii_strcasecmp (choices[i].id, value) == 0)
      {
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), choices[i].id);
        return;
      }

  gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), fallback);
}



/* speechd's -100 … +100 shown as a signed percentage: "+20 %" */
static gboolean
speech_page_percent_output (GtkSpinButton *spin,
                            gpointer       user_data)
{
  gint   value = gtk_spin_button_get_value_as_int (spin);
  gchar *text;

  text = g_strdup_printf (value > 0 ? "+%d %%" : "%d %%", value);
  if (g_strcmp0 (text, gtk_entry_get_text (GTK_ENTRY (spin))) != 0)
    gtk_entry_set_text (GTK_ENTRY (spin), text);
  g_free (text);

  return TRUE;
}



static gint
speech_page_percent_input (GtkSpinButton *spin,
                           gdouble       *new_value,
                           gpointer       user_data)
{
  const gchar *text = gtk_entry_get_text (GTK_ENTRY (spin));
  gchar       *end;
  gdouble      value;

  while (g_ascii_isspace (*text) || *text == '+')
    text++;
  value = g_ascii_strtod (text, &end);
  while (g_ascii_isspace (*end) || *end == '%')
    end++;
  if (end == text || *end != '\0')
    return GTK_INPUT_ERROR;

  *new_value = value;
  return TRUE;
}



static GtkWidget *
speech_page_percent_new (gint value)
{
  GtkWidget *spin;

  spin = gtk_spin_button_new_with_range (-100, 100, 5);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
  /* new_with_range makes it numeric, which drops the "%" of the output */
  gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (spin), FALSE);
  gtk_spin_button_set_increments (GTK_SPIN_BUTTON (spin), 5, 20);
  gtk_entry_set_width_chars (GTK_ENTRY (spin), 5);
  gtk_entry_set_max_width_chars (GTK_ENTRY (spin), 5);
  gtk_entry_set_alignment (GTK_ENTRY (spin), 1.0);
  g_signal_connect (spin, "output",
                    G_CALLBACK (speech_page_percent_output), NULL);
  g_signal_connect (spin, "input",
                    G_CALLBACK (speech_page_percent_input), NULL);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), CLAMP (value, -100, 100));
  /* set_value of the initial 0 emits nothing, so "output" never ran */
  speech_page_percent_output (GTK_SPIN_BUTTON (spin), NULL);

  return spin;
}



/* labels of all frames share one width, so the controls line up */
static GtkSizeGroup *label_size_group = NULL;
/* the combos share one width: the widest one's, not the window's */
static GtkSizeGroup *combo_size_group = NULL;
/* the labels of rate, pitch and volume, so their spins line up */
static GtkSizeGroup *percent_label_size_group = NULL;
/* the "Label: [spin]" rows and the test buttons: one column right of the
 * combos */
static GtkSizeGroup *right_size_group = NULL;

static void
speech_page_grid_add (GtkWidget   *grid,
                      gint         row,
                      const gchar *text,
                      GtkWidget   *widget)
{
  GtkWidget *label;

  label = gtk_label_new_with_mnemonic (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_size_group_add_widget (label_size_group, label);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), widget);
  gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);

  /* natural width, left-aligned: a full-width combo looks huge */
  gtk_widget_set_halign (widget, GTK_IS_COMBO_BOX (widget)
                                 ? GTK_ALIGN_FILL : GTK_ALIGN_START);
  if (GTK_IS_COMBO_BOX (widget))
    gtk_size_group_add_widget (combo_size_group, widget);
  gtk_grid_attach (GTK_GRID (grid), widget, 1, row, 1, 1);
}



/* "Label: [spin]" in column 2 of a row, right of the combos; the
 * tooltip explains the range, 0 is what the synthesizer does by default,
 * not silence */
static void
speech_page_grid_add_percent (GtkWidget   *grid,
                              gint         row,
                              const gchar *text,
                              GtkWidget   *spin,
                              const gchar *low,
                              const gchar *high)
{
  GtkWidget *hbox;
  GtkWidget *label;
  gchar     *tip;

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  /* a wider gap to the combos than between a label and its widget */
  gtk_widget_set_margin_start (hbox, 12);
  gtk_size_group_add_widget (right_size_group, hbox);
  gtk_grid_attach (GTK_GRID (grid), hbox, 2, row, 1, 1);

  label = gtk_label_new_with_mnemonic (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), spin);
  gtk_size_group_add_widget (percent_label_size_group, label);
  gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
  /* at the right edge, where the test buttons end */
  gtk_box_pack_end (GTK_BOX (hbox), spin, FALSE, FALSE, 0);

  tip = g_strdup_printf ("−100 %%: %s\n0 %%: %s\n+100 %%: %s",
                         low, _("Normal"), high);
  gtk_widget_set_tooltip_text (label, tip);
  gtk_widget_set_tooltip_text (spin, tip);
  g_free (tip);
}



static GtkWidget *
speech_page_frame_new (GtkWidget   *box,
                       const gchar *title)
{
  GtkWidget *frame;
  GtkWidget *grid;

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);

#ifdef HAVE_XFCE
  {
    GtkWidget *bin;

    frame = xfce_gtk_frame_box_new (title, &bin);
    gtk_container_add (GTK_CONTAINER (bin), grid);
  }
#else
  {
    /* what xfce_gtk_frame_box_new() draws: a bold title, no border, the
     * contents indented under it */
    GtkWidget *label = gtk_label_new (NULL);
    gchar *markup = g_markup_printf_escaped ("<b>%s</b>", title);

    gtk_label_set_markup (GTK_LABEL (label), markup);
    g_free (markup);
    frame = gtk_frame_new (NULL);
    gtk_frame_set_label_widget (GTK_FRAME (frame), label);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);
    gtk_widget_set_margin_start (grid, 18);
    gtk_widget_set_margin_top (grid, 6);
    gtk_widget_set_margin_bottom (grid, 6);
    gtk_container_add (GTK_CONTAINER (frame), grid);
  }
#endif
  gtk_box_pack_start (GTK_BOX (box), frame, FALSE, TRUE, 0);

  return grid;
}



static gboolean
speech_page_connect (SpeechPage *page)
{
  gchar *error = NULL;
  gchar *message;

  if (page->connection != NULL)
    return TRUE;

  page->connection = spd_open2 (PACKAGE, "main", NULL,
                                SPD_MODE_SINGLE, NULL, TRUE, &error);
  if (page->connection == NULL)
    {
      message = g_strdup_printf (_("Cannot connect to Speech Dispatcher: %s"),
                                 error != NULL ? error : _("unknown error"));
      speech_page_show_message (page, GTK_MESSAGE_WARNING, message);
      g_free (message);
      g_free (error);
      return FALSE;
    }

  return TRUE;
}



static gboolean
speech_page_same_language (const gchar *a,
                           const gchar *b);
static gchar *
speech_page_get_language (SpeechPage *page);
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



static gboolean
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
static gboolean
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
static void
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
static void speech_page_park_preloads (SpeechPage *page);

static void
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
static void
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

      if (g_strv_contains (hidden_modules, module))
        continue;
      if (first == NULL && speech_page_module_speaks (page, module, language))
        first = module;
      if (g_strcmp0 (module, current) == 0)
        found = TRUE;
      else if (!speech_page_module_speaks (page, module, language))
        continue;

      if (speech_page_module_speaks (page, module, language))
        gtk_combo_box_text_append (combo, module, module);
      else
        {
          /* TRANSLATORS: a synthesizer without a voice for the language */
          label = g_strdup_printf (_("%s (no voice)"), module);
          gtk_combo_box_text_append (combo, module, label);
          g_free (label);
        }
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
static void
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
static void
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

static gboolean speech_page_refill_modules (gpointer data);

/* No voices from the selected module: it is most likely still starting after
 * a reload (ours or anyone's). Read everything again in a moment, as the
 * module list and languages came from the same half-started server. */
static void
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
      for (t = 0; t < G_N_ELEMENTS (voice_types); t++)
        if (g_ascii_strcasecmp (variant, voice_types[t].id) == 0)
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



/* The module the dialog configures: the chosen one, else the default */
static gchar *
speech_page_get_module (SpeechPage *page)
{
  const gchar *module;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    return g_strdup (module);

  return speechd_config_get (page->config, "DefaultModule");
}



static gchar *
speech_page_get_language (SpeechPage *page)
{
  GtkWidget   *entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  const gchar *language = gtk_entry_get_text (GTK_ENTRY (entry));

  if (language == NULL || *language == '\0')
    language = DEFAULT_LANGUAGE;

  return g_strstrip (g_strdup (language));
}



/* The voice type the server asks the modules for */
static gchar *
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
static SPDVoice *
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
static void
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



/* "SherpaPreload" for "sherpa-onnx" and "sherpa-onnx-vulkan-rs" */
static gchar *
speech_page_module_key (const gchar *module,
                        const gchar *suffix)
{
  return speechd_module_option_key (module, suffix);
}



/* Sets option @key of @module to @value (NULL removes it) unless it has it
 * already; the server rereads module configurations on the next save. */
static void
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



/* The voices button only for a synthesizer whose voices can be downloaded:
 * the chosen one, else the default */
static void
speech_page_sync_voices_button (SpeechPage *page)
{
  gchar    *module;

  if (page->voices_button == NULL)
    return;

  module = speech_page_get_module (page);
  gtk_widget_set_visible (page->voices_button, voice_installer_has_source (module));
  g_free (module);
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
static gchar *
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
static void
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

static void speech_page_sync_text_options (SpeechPage *page);

/* Shows the selected synthesizer's state in the check box; with
 * @follow_voice, a preloaded sd_sherpa-onnx preloads the voice just chosen */
static void
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

  speech_page_sync_text_options (page);
}

static void
speech_page_sync_preload (SpeechPage *page)
{
  speech_page_sync_preload_full (page, FALSE);
}



/* The text stages of our modules, set per synthesizer in its configuration:
 * "TextNormalization" (numbers, dates, abbreviations as words; on unless
 * "no") and "TextStress" (stress marks from a dictionary; off unless
 * "yes"). A check box shows only for a synthesizer whose configuration
 * names the option, active or commented out. */
typedef struct
{
  const gchar *key;
  gboolean     default_on;
} TextOption;

static const TextOption text_normalization = { "TextNormalization", TRUE };
static const TextOption text_stress = { "TextStress", FALSE };

static gboolean
speech_page_text_option_on (const TextOption *option,
                            const gchar      *value)
{
  if (value == NULL || *value == '\0')
    return option->default_on;
  return g_ascii_strcasecmp (value, "yes") == 0 || g_ascii_strcasecmp (value, "1") == 0
         || g_ascii_strcasecmp (value, "true") == 0 || g_ascii_strcasecmp (value, "on") == 0;
}

static void
speech_page_sync_text_option (SpeechPage       *page,
                              GtkWidget        *check,
                              const TextOption *option)
{
  gchar   *module;
  gboolean supported = FALSE;
  gboolean on = option->default_on;
  gboolean updating = page->updating;

  if (check == NULL)
    return;
  module = speech_page_get_module (page);
  if (module != NULL && *module != '\0')
    supported = speechd_module_has_option (module, option->key);
  if (supported)
    {
      /* both work on Russian text only */
      gchar *language = speech_page_get_language (page);

      supported = language != NULL && g_ascii_strncasecmp (language, "ru", 2) == 0
                  && (language[2] == '\0' || language[2] == '-' || language[2] == '_');
      g_free (language);
    }
  if (supported)
    {
      gchar *value = speechd_module_get_option (module, option->key);

      on = speech_page_text_option_on (option, value);
      g_free (value);
    }

  page->updating = TRUE;
  gtk_widget_set_visible (check, supported);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), on);
  page->updating = updating;
  g_free (module);
}

/* "AudioCopyDir" of our modules: a copy of what they speak, outputN.wav
 * and outputN.txt in a directory; no line means no copy */
#define COPY_KEY "AudioCopyDir"
#define COPY_DEFAULT_DIR "~/speech-copy"

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

static void
speech_page_sync_text_options (SpeechPage *page)
{
  speech_page_sync_text_option (page, page->normalize_check, &text_normalization);
  speech_page_sync_text_option (page, page->stress_check, &text_stress);
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

static void
speech_page_copy_toggled (GtkToggleButton *button,
                          SpeechPage      *page)
{
  speech_page_copy_apply (page);
}

static void
speech_page_copy_entry_activate (GtkEntry   *entry,
                                 SpeechPage *page)
{
  speech_page_copy_apply (page);
}

static gboolean
speech_page_copy_entry_focus_out (GtkWidget  *entry,
                                  GdkEvent   *event,
                                  SpeechPage *page)
{
  speech_page_copy_apply (page);
  return FALSE;
}

static void
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

static void
speech_page_text_option_toggled (GtkToggleButton  *button,
                                 SpeechPage       *page,
                                 const TextOption *option)
{
  gchar   *module;
  gboolean on;

  if (page->updating)
    return;
  module = speech_page_get_module (page);
  if (module != NULL && *module != '\0')
    {
      on = gtk_toggle_button_get_active (button);
      /* the default is no line at all: a later default follows the package */
      speech_page_set_module_option (page, module, option->key,
                                     on == option->default_on ? NULL
                                     : on ? "yes" : "no");
    }
  g_free (module);
}

static void
speech_page_normalize_toggled (GtkToggleButton *button,
                               SpeechPage      *page)
{
  speech_page_text_option_toggled (button, page, &text_normalization);
}

static void
speech_page_stress_toggled (GtkToggleButton *button,
                            SpeechPage      *page)
{
  speech_page_text_option_toggled (button, page, &text_stress);
}



static void
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



static void
speech_page_voice_changed (GtkComboBox *combo,
                           SpeechPage  *page)
{
  const gchar *name = gtk_combo_box_get_active_id (combo);
  SPDVoice    *voice;
  GError      *error = NULL;
  gchar       *module;
  gchar       *language;
  gchar       *type;
  gchar       *message;

  if (page->updating || name == NULL)
    return;

  module = speech_page_get_module (page);
  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  voice = speech_page_find_voice (page, name, language);

  /* the module knows the language as its voice says, e.g. "ru", not "ru-RU" */
  if (voice != NULL && module != NULL
      && !speechd_module_set_voice (module, voice->language, type, name,
                                    &error))
    {
      message = g_strdup_printf (_("Failed to save the voice: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
    }
  else if (voice != NULL && module != NULL)
    {
      page->reload_pending = TRUE;
      speech_page_queue_save (page);
      speech_page_sync_preload_full (page, TRUE);
      speech_page_sync_voices_button (page);
    }

  g_free (type);
  g_free (language);
  g_free (module);
}



static void
speech_page_module_changed (GtkComboBox *combo,
                            SpeechPage  *page)
{
  const gchar *module = gtk_combo_box_get_active_id (combo);

  if (page->updating || module == NULL)
    return;

  speech_page_store_module (page, module);
  speech_page_queue_save (page);

  speech_page_load_voices (page);
  speech_page_update_voices (page);
  speech_page_retry_if_empty (page);
  speech_page_sync_preload (page);
  speech_page_sync_voices_button (page);
}



/* The test sentence for a language code like "ru" or "en-US", or NULL */
static const gchar *
speech_page_get_test_text (const gchar *language)
{
  gsize len;
  guint i;

  if (language == NULL)
    return NULL;

  len = strcspn (language, "-_");
  for (i = 0; i < G_N_ELEMENTS (test_texts); i++)
    if (strlen (test_texts[i].language) == len
        && g_ascii_strncasecmp (test_texts[i].language, language, len) == 0)
      return test_texts[i].text;

  return NULL;
}



/* the whole text of the test field */
static gchar *
speech_page_get_view_text (SpeechPage *page)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (page->test_view));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}



/* Shows the sentence of the language, unless the user has typed another text */
static void
speech_page_update_test_text (SpeechPage  *page,
                              const gchar *language)
{
  const gchar *text = speech_page_get_test_text (language);
  GtkTextBuffer *buffer;
  gchar *current;
  gboolean kept;

  /* no sentence for this language: English rather than the sentence of the
   * language chosen before */
  if (text == NULL)
    text = speech_page_get_test_text ("en");

  if (page->test_view == NULL)
    return;

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (page->test_view));
  current = speech_page_get_view_text (page);
  kept = page->test_text != NULL && g_strcmp0 (current, page->test_text) != 0;
  g_free (current);
  if (kept)
    return;

  gtk_text_buffer_set_text (buffer, text, -1);
  page->test_text = text;
}



/* TRUE for a language code like "ru", "pt-BR" or "zh_Hant": two or three
 * letters, then parts of letters and digits joined by "-" or "_" */
static gboolean
speech_page_is_language_code (const gchar *text)
{
  return g_regex_match_simple ("^[A-Za-z]{2,3}([-_][A-Za-z0-9]{1,8})*$",
                               text, 0, 0);
}



/* Writes the typed language to the configuration. Only an empty entry or
 * a code like "ru" or "pt-BR" is written; mistyped text ("и") keeps the
 * saved language. */
static void
speech_page_store_language (SpeechPage  *page,
                            const gchar *language)
{
  if (*language == '\0')
    {
      speechd_config_unset (page->config, "DefaultLanguage");
      speech_page_queue_save (page);
    }
  else if (speech_page_is_language_code (language))
    {
      speechd_config_set (page->config, "DefaultLanguage", language);
      speech_page_queue_save (page);
    }
}



static gboolean
speech_page_apply_language (gpointer data)
{
  SpeechPage *page = data;
  gchar      *language;

  page->language_id = 0;

  language = page->typed_language;
  page->typed_language = NULL;
  speech_page_store_language (page, language);
  speech_page_update_test_text (page, *language != '\0' ? language : DEFAULT_LANGUAGE);

  /* the synthesizer may change with the language: its "changed" handler is
   * blocked here, so read the voices of the one now selected */
  page->updating = TRUE;
  speech_page_fill_modules (page);
  page->updating = FALSE;
  speech_page_load_voices (page);
  speech_page_update_voices (page);
  speech_page_retry_if_empty (page);
  speech_page_sync_preload (page);
  speech_page_sync_voices_button (page);

  g_free (language);

  return G_SOURCE_REMOVE;
}



/* Applied once typing pauses: replacing the selected text empties the
 * entry for a moment, which must not reset the language */
static void
speech_page_language_changed (GtkComboBox *combo,
                              SpeechPage  *page)
{
  GtkWidget *entry = gtk_bin_get_child (GTK_BIN (combo));

  if (page->updating)
    return;

  g_free (page->typed_language);
  page->typed_language =
    g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))));

  if (page->language_id != 0)
    g_source_remove (page->language_id);
  page->language_id = g_timeout_add (LANGUAGE_DELAY_MS,
                                     speech_page_apply_language, page);
}



static void
speech_page_choice_changed (GtkComboBox *combo,
                            SpeechPage  *page)
{
  const gchar *key = g_object_get_data (G_OBJECT (combo), "speechd-key");
  const gchar *id = gtk_combo_box_get_active_id (combo);

  if (page->updating || id == NULL)
    return;

  if (*id == '\0')
    speechd_config_unset (page->config, key);
  else
    speechd_config_set (page->config, key, id);
  speech_page_queue_save (page);
}



static void
speech_page_percent_changed (GtkSpinButton *spin,
                             SpeechPage    *page)
{
  const gchar *key = g_object_get_data (G_OBJECT (spin), "speechd-key");

  if (page->updating)
    return;

  speechd_config_set_int (page->config, key,
                          gtk_spin_button_get_value_as_int (spin));
  speech_page_queue_save (page);
}



static gint
speech_page_choice_index (GtkWidget    *combo,
                          const Choice *choices,
                          guint         n_choices)
{
  const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
  guint        i;

  for (i = 0; id != NULL && i < n_choices; i++)
    if (strcmp (choices[i].id, id) == 0)
      return i;

  return 0;
}



/* The modules restart after the reload; ask for the list once they are up. */
static gboolean
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
static gboolean
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

static void
speech_page_window_active (GObject    *window,
                           GParamSpec *pspec,
                           gpointer    box)
{
  SpeechPage *page = g_object_get_data (G_OBJECT (box), "speech-page");
  struct stat st;

  if (page == NULL || page->config == NULL || page->refill_id != 0
      || !gtk_window_is_active (GTK_WINDOW (window))
      || !speech_page_server_stat (&st)
      || (st.st_dev == page->server_dev && st.st_ino == page->server_ino))
    return;

  page->server_dev = st.st_dev;
  page->server_ino = st.st_ino;
  page->refill_id = g_idle_add (speech_page_refill_modules, page);
}



static void
speech_page_hierarchy_changed (GtkWidget *box,
                               GtkWidget *previous,
                               gpointer   data)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel (box);

  if (!GTK_IS_WINDOW (toplevel))
    return;
  g_signal_connect_object (toplevel, "notify::is-active",
                           G_CALLBACK (speech_page_window_active), box, 0);
  g_signal_handlers_disconnect_by_func (box, speech_page_hierarchy_changed, data);
}



/* Voices were installed or removed: the server reads them on a reload */
static void
speech_page_voices_changed (gpointer data)
{
  SpeechPage *page = data;

  page->reload_pending = TRUE;
  speech_page_queue_save (page);
  if (page->refill_id != 0)
    g_source_remove (page->refill_id);
  page->refill_id = g_timeout_add (SAVE_DELAY_MS + REFILL_DELAY_MS,
                                   speech_page_refill_modules, page);
}



static void
speech_page_voices_clicked (GtkButton  *button,
                            SpeechPage *page)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (button));
  gchar     *language = speech_page_get_language (page);
  gchar     *module = speech_page_get_module (page);

  voice_installer_show (GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
                        module, language, speech_page_voices_changed, page);
  g_free (module);
  g_free (language);
}



/* A window does not get smaller by itself when a child is hidden */
static void
speech_page_hide_modules_bar (SpeechPage *page)
{
  GtkWidget *toplevel;
  gint       width;

  gtk_widget_hide (page->modules_bar);

  toplevel = gtk_widget_get_toplevel (page->modules_bar);
  if (GTK_IS_WINDOW (toplevel))
    {
      gtk_window_get_size (GTK_WINDOW (toplevel), &width, NULL);
      gtk_window_resize (GTK_WINDOW (toplevel), width, 1);
    }
}



static void
speech_page_modules_response (GtkInfoBar *bar,
                              gint        response,
                              SpeechPage *page)
{
  GError *error = NULL;
  gchar  *message;

  if (response != GTK_RESPONSE_ACCEPT)
    {
      speech_page_hide_modules_bar (page);
      return;
    }

  if (page->save_id != 0)
    {
      g_source_remove (page->save_id);
      page->save_id = 0;
    }

  if (!speechd_config_use_detected_modules (page->config, &error)
      || !speechd_config_save (page->config, &error)
      || !speechd_server_reload (&error))
    {
      message = g_strdup_printf (_("Failed to apply the settings: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
      return;
    }

  speech_page_hide_modules_bar (page);

  if (page->refill_id != 0)
    g_source_remove (page->refill_id);
  page->refill_id = g_timeout_add (REFILL_DELAY_MS,
                                   speech_page_refill_modules, page);
}



/* Speaks with the values shown in the dialog, set explicitly on our own
 * connection, so the test does not wait for the server reload. */
static void
speech_page_test_clicked (GtkButton  *button,
                          SpeechPage *page)
{
  const gchar *module;
  const gchar *language;
  gchar       *text;
  GtkWidget   *entry;
  gint         i;

  if (!speech_page_connect (page))
    return;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    spd_set_output_module (page->connection, module);

  entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  language = gtk_entry_get_text (GTK_ENTRY (entry));
  spd_set_language (page->connection,
                    *language != '\0' ? language : DEFAULT_LANGUAGE);

  i = speech_page_choice_index (page->voice_type_combo, voice_types,
                                G_N_ELEMENTS (voice_types));
  spd_set_voice_type (page->connection, SPD_MALE1 + i);
  if (gtk_widget_get_sensitive (page->voice_combo)
      && gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->voice_combo)) != NULL)
    spd_set_synthesis_voice (page->connection,
                             gtk_combo_box_get_active_id (
                               GTK_COMBO_BOX (page->voice_combo)));

  spd_set_voice_rate (page->connection,
                      gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (page->rate_spin)));
  spd_set_voice_pitch (page->connection,
                       gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (page->pitch_spin)));
  spd_set_volume (page->connection,
                  gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (page->volume_spin)));

  i = speech_page_choice_index (page->punctuation_combo, punctuation_modes,
                                G_N_ELEMENTS (punctuation_modes));
  spd_set_punctuation (page->connection, punctuation_values[i]);

  i = speech_page_choice_index (page->capital_combo, capital_modes,
                                G_N_ELEMENTS (capital_modes));
  spd_set_capital_letters (page->connection, capital_values[i]);

  text = speech_page_get_view_text (page);
  g_strstrip (text);
  if (*text != '\0')
    {
      spd_cancel (page->connection);
      if (spd_say (page->connection, SPD_TEXT, text) < 0)
        speech_page_show_message (page, GTK_MESSAGE_WARNING,
                                  _("Speech Dispatcher did not accept the text."));
    }
  g_free (text);
}



static void
speech_page_stop_clicked (GtkButton  *button,
                          SpeechPage *page)
{
  if (page->connection != NULL)
    spd_cancel (page->connection);
}



static void
speech_page_free (gpointer data)
{
  SpeechPage *page = data;

  /* a language typed just before closing is still saved */
  if (page->language_id != 0)
    {
      g_source_remove (page->language_id);
      speech_page_store_language (page, page->typed_language);
    }
  g_free (page->typed_language);

  if (page->save_id != 0)
    {
      g_source_remove (page->save_id);
      speech_page_save (page);
    }

  if (page->refill_id != 0)
    g_source_remove (page->refill_id);
  if (page->watch_id != 0)
    g_source_remove (page->watch_id);
  if (page->fit_id != 0)
    g_source_remove (page->fit_id);

  if (page->connection != NULL)
    spd_close (page->connection);

  if (page->voices != NULL)
    free_spd_voices (page->voices);
  g_strfreev (page->modules);
  g_hash_table_destroy (page->module_languages);
  g_ptr_array_free (page->languages, TRUE);

  speechd_config_free (page->config);
  g_free (page);
}



void
speech_page_flush (GtkWidget *widget)
{
  SpeechPage *page = g_object_get_data (G_OBJECT (widget), "speech-page");

  if (page != NULL && page->save_id != 0)
    {
      g_source_remove (page->save_id);
      speech_page_save (page);
    }
}



void
speech_page_add_close (GtkWidget *widget,
                       GtkWidget *button)
{
  SpeechPage *page = g_object_get_data (G_OBJECT (widget), "speech-page");
  GtkWidget  *hbox;

  g_return_if_fail (page != NULL);

  /* the same row as Speak and Stop, with Close in the place of Stop */
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (hbox), TRUE);
  gtk_widget_set_margin_start (hbox, 12);
  gtk_size_group_add_widget (right_size_group, hbox);
  gtk_box_pack_start (GTK_BOX (hbox), gtk_label_new (NULL), TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);

  /* at the bottom of a taller window; the gap between frames, not the one
   * between rows */
  gtk_widget_set_valign (hbox, GTK_ALIGN_END);
  gtk_widget_set_vexpand (hbox, TRUE);
  gtk_widget_set_margin_top (hbox, 12);
  gtk_grid_attach (GTK_GRID (page->test_grid), hbox, 2, 1, 1, 1);
}



static void
speech_page_bind_choice (SpeechPage   *page,
                         GtkWidget    *combo,
                         const gchar  *key,
                         const Choice *choices,
                         guint         n_choices,
                         const gchar  *fallback)
{
  gchar *value;

  value = speechd_config_get (page->config, key);
  speech_page_combo_select (combo, choices, n_choices, value, fallback);
  g_free (value);

  g_object_set_data (G_OBJECT (combo), "speechd-key", (gpointer) key);
  g_signal_connect (combo, "changed",
                    G_CALLBACK (speech_page_choice_changed), page);
}



static GtkWidget *
speech_page_bind_percent (SpeechPage  *page,
                          const gchar *key,
                          gint         fallback)
{
  GtkWidget *spin;

  spin = speech_page_percent_new (speechd_config_get_int (page->config, key,
                                                          fallback));
  g_object_set_data (G_OBJECT (spin), "speechd-key", (gpointer) key);
  g_signal_connect (spin, "value-changed",
                    G_CALLBACK (speech_page_percent_changed), page);

  return spin;
}



/* the height of one line of text in widget, in pixels */
static gint
speech_page_line_height (GtkWidget *widget)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, "Ag");
  gint height;

  pango_layout_get_pixel_size (layout, NULL, &height);
  g_object_unref (layout);
  return height;
}



/* Sets the height of the test text to its lines, at most TEST_MAX_LINES:
 * a dialog does not grow by itself when only the natural height grows,
 * so the minimum height follows the text. */
static gboolean
speech_page_fit_test_text (gpointer data)
{
  SpeechPage  *page = data;
  GtkTextView *view = GTK_TEXT_VIEW (page->test_view);
  GtkWidget   *scrolled = gtk_widget_get_parent (page->test_view);
  GtkWidget   *toplevel;
  GtkTextIter  end;
  gint         y, height, line, old;

  page->fit_id = 0;
  gtk_text_buffer_get_end_iter (gtk_text_view_get_buffer (view), &end);
  gtk_text_view_get_line_yrange (view, &end, &y, &height);
  line = speech_page_line_height (page->test_view);
  /* a scroll bar only when needed: even hidden it keeps the field high */
  if (y + height > line * TEST_MAX_LINES)
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  else
    {
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                      GTK_POLICY_NEVER, GTK_POLICY_NEVER);
      /* the view scrolled to the cursor while it was lower: back to the top */
      gtk_adjustment_set_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (view)), 0);
    }
  height = CLAMP (y + height, line, line * TEST_MAX_LINES)
           + gtk_text_view_get_top_margin (view) + gtk_text_view_get_bottom_margin (view);
  old = gtk_scrolled_window_get_min_content_height (GTK_SCROLLED_WINDOW (scrolled));
  if (height == old)
    return G_SOURCE_REMOVE;

  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled), height);
  /* a window keeps its size when its content gets smaller: shrink it */
  toplevel = gtk_widget_get_toplevel (page->test_view);
  if (height < old && GTK_IS_WINDOW (toplevel))
    gtk_window_resize (GTK_WINDOW (toplevel),
                       gtk_widget_get_allocated_width (toplevel), 1);
  return G_SOURCE_REMOVE;
}

/* after a change of the text or of the width (the lines wrap anew), once
 * the view has laid the text out */
static void
speech_page_queue_fit (SpeechPage *page)
{
  if (page->fit_id == 0)
    page->fit_id = g_idle_add_full (GTK_TEXT_VIEW_PRIORITY_VALIDATE + 1,
                                    speech_page_fit_test_text, page, NULL);
}

static void
speech_page_test_view_allocated (GtkWidget     *view,
                                 GdkRectangle  *allocation,
                                 SpeechPage    *page)
{
  speech_page_queue_fit (page);
}



/* Ctrl+Enter in the test text speaks it; Enter alone starts a new line */
static gboolean
speech_page_test_key_press (GtkWidget   *view,
                            GdkEventKey *event,
                            SpeechPage  *page)
{
  if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
      && (event->state & gtk_accelerator_get_default_mod_mask ()) == GDK_CONTROL_MASK)
    {
      speech_page_test_clicked (NULL, page);
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}



GtkWidget *
speech_page_new (void)
{
  SpeechPage *page;
  GtkWidget  *box;
  GtkWidget  *grid;
  GtkWidget  *entry;
  GtkWidget  *button;
  GtkWidget  *hbox;
  GtkWidget  *label;
  GtkWidget  *scrolled;
  GError     *error = NULL;
  gchar      *value;
  gchar      *text;

  page = g_new0 (SpeechPage, 1);
  page->module_languages =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify) g_ptr_array_unref);
  page->languages = g_ptr_array_new_with_free_func (g_free);

  if (label_size_group == NULL)
    label_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  if (combo_size_group == NULL)
    combo_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  if (percent_label_size_group == NULL)
    percent_label_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  if (right_size_group == NULL)
    right_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  g_object_set_data_full (G_OBJECT (box), "speech-page", page,
                          speech_page_free);
  g_signal_connect (box, "hierarchy-changed",
                    G_CALLBACK (speech_page_hierarchy_changed), NULL);

  page->info_bar = gtk_info_bar_new ();
  page->info_label = gtk_label_new (NULL);
  gtk_label_set_line_wrap (GTK_LABEL (page->info_label), TRUE);
  gtk_label_set_width_chars (GTK_LABEL (page->info_label), 40);
  gtk_label_set_xalign (GTK_LABEL (page->info_label), 0.0);
  /* errors are copied into bug reports and searches */
  gtk_label_set_selectable (GTK_LABEL (page->info_label), TRUE);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (
                       GTK_INFO_BAR (page->info_bar))), page->info_label);
  gtk_widget_show (page->info_label);
  gtk_widget_set_no_show_all (page->info_bar, TRUE);
  gtk_box_pack_start (GTK_BOX (box), page->info_bar, FALSE, TRUE, 0);

  page->config = speechd_config_load (&error);
  if (page->config == NULL)
    {
      text = g_strdup_printf (_("Failed to read the settings: %s"),
                              error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, text);
      g_free (text);
      g_error_free (error);
      return box;
    }

  /* full copies of our modules' files, made by older versions, become the
   * options that differ from the system files; the modules reread them */
  if (speechd_modules_migrate ())
    speech_page_voices_changed (page);

  /* AddModule lines hide every module that is not listed */
  page->modules_bar = gtk_info_bar_new_with_buttons (
    _("_Use All"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_info_bar_set_message_type (GTK_INFO_BAR (page->modules_bar),
                                 GTK_MESSAGE_QUESTION);
  gtk_info_bar_set_show_close_button (GTK_INFO_BAR (page->modules_bar), TRUE);
  label = gtk_label_new (_("Your settings list the synthesizers to load, so "
                           "synthesizers installed later are not offered. "
                           "Use all installed synthesizers?"));
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  /* the window height is computed for the narrowest wrapping */
  gtk_label_set_width_chars (GTK_LABEL (label), 40);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (
                       GTK_INFO_BAR (page->modules_bar))), label);
  gtk_widget_show (label);
  gtk_widget_set_no_show_all (page->modules_bar, TRUE);
  gtk_box_pack_start (GTK_BOX (box), page->modules_bar, FALSE, TRUE, 0);
  g_signal_connect (page->modules_bar, "response",
                    G_CALLBACK (speech_page_modules_response), page);
  if (speechd_config_has_module_list (page->config))
    gtk_widget_show (page->modules_bar);

  page->updating = TRUE;

  /* Voice */
  grid = speech_page_frame_new (box, _("Voice"));

  /* the language first: it decides which synthesizers can speak */
  speech_page_scan_modules (page);
  page->watch_id = g_timeout_add (WATCH_DELAY_MS, speech_page_watch_modules,
                                  page);

  page->language_combo = gtk_combo_box_text_new_with_entry ();
  entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), DEFAULT_LANGUAGE);
  gtk_widget_set_tooltip_text (page->language_combo,
                               _("Language code like “en-US” or “ru”. Most "
                                 "applications set the language themselves."));
  speech_page_grid_add (grid, 0, _("_Language:"), page->language_combo);
  speech_page_fill_languages (page);
  value = speechd_config_get (page->config, "DefaultLanguage");
  gtk_entry_set_text (GTK_ENTRY (entry), value != NULL ? value : "");
  g_free (value);

  page->module_combo = gtk_combo_box_text_new ();
  gtk_widget_set_tooltip_text (page->module_combo,
                               _("The speech synthesizer used when an "
                                 "application does not ask for another one. "
                                 "Only the ones with a voice for the "
                                 "language are offered."));
  speech_page_grid_add (grid, 1, _("_Synthesizer:"), page->module_combo);
  speech_page_fill_modules (page);
  speech_page_load_voices (page);
  g_signal_connect (page->module_combo, "changed",
                    G_CALLBACK (speech_page_module_changed), page);
  g_signal_connect (page->language_combo, "changed",
                    G_CALLBACK (speech_page_language_changed), page);

  page->voice_combo = gtk_combo_box_text_new ();
  {
    /* a list under the field: as a menu it puts the selected voice over
     * the field, and a voice near the end of a long list leaves the rest
     * of the menu empty */
    GtkCssProvider *css = gtk_css_provider_new ();

    gtk_css_provider_load_from_data (css,
                                     "combobox { -GtkComboBox-appears-as-list: 1; }",
                                     -1, NULL);
    gtk_style_context_add_provider (gtk_widget_get_style_context (page->voice_combo),
                                    GTK_STYLE_PROVIDER (css),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css);
  }
  speech_page_grid_add (grid, 2, _("_Voice:"), page->voice_combo);
  g_signal_connect (page->voice_combo, "changed",
                    G_CALLBACK (speech_page_voice_changed), page);

  page->voice_type_combo = speech_page_combo_new (voice_types,
                                                  G_N_ELEMENTS (voice_types));
  gtk_widget_set_tooltip_text (page->voice_type_combo,
                               _("The synthesizer maps these types to its "
                                 "own voices"));
  speech_page_grid_add (grid, 3, _("Voice _type:"), page->voice_type_combo);
  page->voice_type_label =
    gtk_grid_get_child_at (GTK_GRID (grid), 0, 3);
  gtk_widget_set_no_show_all (page->voice_type_label, TRUE);
  gtk_widget_set_no_show_all (page->voice_type_combo, TRUE);
  speech_page_bind_choice (page, page->voice_type_combo, "DefaultVoiceType",
                           voice_types, G_N_ELEMENTS (voice_types), "MALE1");
  speech_page_update_voices (page);

  page->preload_check =
    gtk_check_button_new_with_mnemonic (_("_Keep the synthesizer loaded"));
  gtk_widget_set_tooltip_text (page->preload_check,
                               _("Load the synthesizer when the speech "
                                 "server starts and keep it in memory: the "
                                 "first message is spoken sooner. Otherwise "
                                 "it is loaded with the first message and "
                                 "freed after five minutes without speech."));
  gtk_widget_set_no_show_all (page->preload_check, TRUE);
  gtk_grid_attach (GTK_GRID (grid), page->preload_check, 1, 4, 1, 1);
  /* a synthesizer left unused by an older version or by hand */
  speech_page_park_preloads (page);
  speech_page_sync_preload (page);
  g_signal_connect (page->preload_check, "toggled",
                    G_CALLBACK (speech_page_preload_toggled), page);

  page->voices_button = gtk_button_new_with_mnemonic (_("_Voices…"));
  gtk_widget_set_tooltip_text (page->voices_button,
                               _("Download more voices for this synthesizer"));
  gtk_widget_set_halign (page->voices_button, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), page->voices_button, 1, 5, 1, 1);
  gtk_widget_set_no_show_all (page->voices_button, TRUE);
  speech_page_sync_voices_button (page);
  g_signal_connect (page->voices_button, "clicked",
                    G_CALLBACK (speech_page_voices_clicked), page);

  /* rate, pitch, volume: a column right of language, synthesizer, voice */
  page->rate_spin = speech_page_bind_percent (page, "DefaultRate", 0);
  speech_page_grid_add_percent (grid, 0, _("_Rate:"), page->rate_spin,
                                _("Slower"), _("Faster"));
  page->pitch_spin = speech_page_bind_percent (page, "DefaultPitch", 0);
  speech_page_grid_add_percent (grid, 1, _("_Pitch:"), page->pitch_spin,
                                _("Lower"), _("Higher"));
  page->volume_spin = speech_page_bind_percent (page, "DefaultVolume", 0);
  speech_page_grid_add_percent (grid, 2, _("_Volume:"), page->volume_spin,
                                _("Quieter"), _("Louder"));

  /* Reading */
  grid = speech_page_frame_new (box, _("Reading"));

  page->punctuation_combo =
    speech_page_combo_new (punctuation_modes, G_N_ELEMENTS (punctuation_modes));
  speech_page_grid_add (grid, 0, _("P_unctuation:"), page->punctuation_combo);
  speech_page_bind_choice (page, page->punctuation_combo,
                           "DefaultPunctuationMode", punctuation_modes,
                           G_N_ELEMENTS (punctuation_modes), "none");

  page->capital_combo =
    speech_page_combo_new (capital_modes, G_N_ELEMENTS (capital_modes));
  speech_page_grid_add (grid, 1, _("_Capital letters:"), page->capital_combo);
  speech_page_bind_choice (page, page->capital_combo,
                           "DefaultCapLetRecognition", capital_modes,
                           G_N_ELEMENTS (capital_modes), "none");

  /* the audio output goes with the reading settings: one frame less */
  page->audio_combo =
    speech_page_combo_new (audio_methods, G_N_ELEMENTS (audio_methods));
  speech_page_grid_add (grid, 2, _("_Output:"), page->audio_combo);
  speech_page_bind_choice (page, page->audio_combo, "AudioOutputMethod",
                           audio_methods, G_N_ELEMENTS (audio_methods), "");

  /* the text stages of the selected synthesizer */
  page->normalize_check =
    gtk_check_button_new_with_mnemonic (_("Read _numbers, dates and abbreviations as words"));
  gtk_widget_set_tooltip_text (page->normalize_check,
                               _("The selected synthesizer writes out numbers, "
                                 "dates, units and abbreviations in full before "
                                 "reading (Russian for now). Without it the "
                                 "voice reads the text as it is."));
  gtk_widget_set_no_show_all (page->normalize_check, TRUE);
  gtk_grid_attach (GTK_GRID (grid), page->normalize_check, 1, 3, 1, 1);
  g_signal_connect (page->normalize_check, "toggled",
                    G_CALLBACK (speech_page_normalize_toggled), page);

  page->stress_check =
    gtk_check_button_new_with_mnemonic (_("Take stress and ё from a _dictionary"));
  gtk_widget_set_tooltip_text (page->stress_check,
                               _("Before reading, Russian text gets the stress "
                                 "and the letter ё of a pronunciation dictionary, "
                                 "as far as the voice can use them (Piper voices "
                                 "get only ё). Words with two possible stresses "
                                 "(зАмок, замОк) are not resolved yet: the "
                                 "dictionary picks one."));
  gtk_widget_set_no_show_all (page->stress_check, TRUE);
  gtk_grid_attach (GTK_GRID (grid), page->stress_check, 1, 4, 1, 1);
  g_signal_connect (page->stress_check, "toggled",
                    G_CALLBACK (speech_page_stress_toggled), page);

  page->copy_check =
    gtk_check_button_new_with_mnemonic (_("Save a cop_y of the speech to files"));
  gtk_widget_set_tooltip_text (page->copy_check,
                               _("For debugging: while it speaks, the selected "
                                 "synthesizer also writes each message to "
                                 "output1.wav, output2.wav and so on in the "
                                 "folder below, and the text the voice got, "
                                 "after numbers and stress, to output1.txt "
                                 "and so on."));
  gtk_widget_set_no_show_all (page->copy_check, TRUE);
  gtk_grid_attach (GTK_GRID (grid), page->copy_check, 1, 5, 1, 1);
  g_signal_connect (page->copy_check, "toggled",
                    G_CALLBACK (speech_page_copy_toggled), page);
  {
    GtkWidget *browse;

    page->copy_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (page->copy_box, 24);
    page->copy_entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (page->copy_entry), COPY_DEFAULT_DIR);
    gtk_widget_set_hexpand (page->copy_entry, TRUE);
    gtk_widget_set_tooltip_text (page->copy_entry,
                                 _("A full path or ~/...; the folder is created "
                                   "when needed. Enter or leaving the field saves it."));
    gtk_box_pack_start (GTK_BOX (page->copy_box), page->copy_entry, TRUE, TRUE, 0);
    browse = gtk_button_new_with_mnemonic (_("_Browse…"));
    gtk_box_pack_start (GTK_BOX (page->copy_box), browse, FALSE, FALSE, 0);
    gtk_widget_show_all (page->copy_box);
    gtk_widget_set_no_show_all (page->copy_box, TRUE);
    gtk_grid_attach (GTK_GRID (grid), page->copy_box, 1, 6, 1, 1);
    g_signal_connect (page->copy_entry, "activate",
                      G_CALLBACK (speech_page_copy_entry_activate), page);
    g_signal_connect (page->copy_entry, "focus-out-event",
                      G_CALLBACK (speech_page_copy_entry_focus_out), page);
    g_signal_connect (browse, "clicked", G_CALLBACK (speech_page_copy_browse), page);
  }
  speech_page_sync_text_options (page);

  /* Test */
  grid = speech_page_frame_new (box, _("Test"));

  page->test_grid = grid;

  /* the text where the combos are, the buttons below rate, pitch, volume;
   * one line that grows with the text up to TEST_MAX_LINES, then scrolls
   * (speech_page_fit_test_text) */
  page->test_view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (page->test_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (page->test_view), FALSE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (page->test_view), 6);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (page->test_view), 6);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (page->test_view), 4);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (page->test_view), 4);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (page->test_view)),
                            _("This is how your computer speaks."), -1);
  value = speechd_config_get (page->config, "DefaultLanguage");
  page->test_text = _("This is how your computer speaks.");
  if (value != NULL && *value != '\0')
    speech_page_update_test_text (page, value);
  g_free (value);
  gtk_widget_set_tooltip_text (page->test_view,
                               _("Ctrl+Enter speaks the text. Most Russian voices "
                                 "take \"+\" before a vowel as its stress: зам+ок."));
  g_signal_connect (page->test_view, "key-press-event",
                    G_CALLBACK (speech_page_test_key_press), page);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), page->test_view);
  g_signal_connect_swapped (gtk_text_view_get_buffer (GTK_TEXT_VIEW (page->test_view)),
                            "changed", G_CALLBACK (speech_page_queue_fit), page);
  g_signal_connect (page->test_view, "size-allocate",
                    G_CALLBACK (speech_page_test_view_allocated), page);

  label = gtk_label_new_with_mnemonic (_("Te_xt:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  /* level with the first line, not with the middle of a tall field */
  gtk_widget_set_valign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_top (label, 5);
  gtk_size_group_add_widget (label_size_group, label);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), page->test_view);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  /* the combos decide the width, not the text */
  gtk_widget_set_halign (scrolled, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (scrolled, FALSE);
  gtk_size_group_add_widget (combo_size_group, scrolled);
  gtk_grid_attach (GTK_GRID (grid), scrolled, 1, 0, 1, 1);

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (hbox), TRUE);
  gtk_widget_set_margin_start (hbox, 12);
  gtk_size_group_add_widget (right_size_group, hbox);
  gtk_widget_set_valign (hbox, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), hbox, 2, 0, 1, 1);

  button = gtk_button_new_with_mnemonic (_("_Speak"));
  g_signal_connect (button, "clicked",
                    G_CALLBACK (speech_page_test_clicked), page);
  gtk_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);

  button = gtk_button_new_with_mnemonic (_("S_top"));
  g_signal_connect (button, "clicked",
                    G_CALLBACK (speech_page_stop_clicked), page);
  gtk_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);

  page->updating = FALSE;

  return box;
}
