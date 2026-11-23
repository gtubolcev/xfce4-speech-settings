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

#ifndef __SPEECH_PAGE_PRIVATE_H__
#define __SPEECH_PAGE_PRIVATE_H__

/* What the parts of the settings page share: speech-page.c builds the page
 * and handles its signals, speech-page-voices.c knows the modules, languages
 * and voices of the server, speech-page-synth.c the options of the selected
 * synthesizer (preload, load requests, audio copy). */

#include <sys/stat.h>

#include <gtk/gtk.h>
#include <speech-dispatcher/libspeechd.h>

#include "speechd-config.h"

G_BEGIN_DECLS

/* Built-in defaults of Speech Dispatcher, used when a key is not set */
#define DEFAULT_LANGUAGE "en-US"
#define COPY_DEFAULT_DIR "~/speech-copy"

typedef struct
{
  const gchar *id;
  const gchar *label;
} Choice;

/* the order matches SPDVoiceType, starting at SPD_MALE1 */
#define N_VOICE_TYPES (SPD_CHILD_FEMALE - SPD_MALE1 + 1)
extern const Choice speech_page_voice_types[N_VOICE_TYPES];

typedef struct
{
  SpeechdConfig *config;
  SPDConnection *connection;

  GtkWidget     *info_bar;
  GtkWidget     *info_label;
  GtkWidget     *modules_bar;

  GtkWidget     *model_combo;       /* "Synthesizer": Piper, Kokoro, ... */
  GtkWidget     *module_combo;      /* "Engine": the speechd module */
  GtkWidget     *module_label;
  GtkWidget     *language_combo;
  GtkWidget     *voice_combo;
  GtkWidget     *voice_type_label;
  GtkWidget     *voice_type_combo;
  GtkWidget     *preload_check;
  /* runs the synthesizer's "-vulkan-rs" variant, which the menu hides */
  GtkWidget     *gpu_box;           /* "CPU [switch] GPU" */
  GtkWidget     *gpu_switch;        /* on: the -vulkan-rs variant */
  GtkWidget     *voices_button;
  GtkWidget     *models_button;
  /* "Loading the voice…" or why it failed, next to preload_check */
  GtkWidget     *load_box;
  GtkWidget     *load_spinner;
  GtkWidget     *load_label;
  GtkWidget     *speak_button;      /* insensitive while a voice loads */
  GFileMonitor  *load_monitor;
  /* the modules asked to load a voice, to let go when the dialog closes */
  GHashTable    *load_requested;
  /* the voices of the selected module, NULL-terminated, or NULL */
  SPDVoice     **voices;
  /* the modules of the server, and the languages each one has voices for */
  gchar        **modules;
  GHashTable    *module_languages;
  GHashTable    *module_voices;     /* module -> SPDVoice ** of the scan */
  gchar         *model;             /* the synthesizer chosen in this run */
  /* the languages of all modules, sorted */
  GPtrArray     *languages;
  GtkWidget     *rate_spin;
  GtkWidget     *pitch_spin;
  GtkWidget     *volume_spin;
  GtkWidget     *punctuation_combo;
  GtkWidget     *capital_combo;
  GtkWidget     *audio_combo;
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
  guint          release_id;    /* frees the synthesizers switched away from */
  guint          pending_id;    /* a load request the module has not seen yet */
  /* the server the module list came from (its socket), see
   * speech_page_server_changed */
  dev_t          server_dev;
  ino_t          server_ino;
  /* a change the server has to read at once (a module that does not read
   * its file again itself, the audio output, the module list) */
  gboolean       reload_pending;
  /* written, the server reads it when the dialog closes: restarting all
   * modules for every slider step silenced the screen reader for seconds;
   * the test sentence gets the settings over its own connection */
  gboolean       server_stale;
  gboolean       updating;
} SpeechPage;


/* speech-page.c */
void speech_page_show_message (SpeechPage     *page,
                               GtkMessageType  type,
                               const gchar    *message);
void speech_page_queue_save (SpeechPage *page);
gboolean speech_page_connect (SpeechPage *page);
void speech_page_sync_voices_button (SpeechPage *page);

/* speech-page-voices.c */
gboolean speech_page_array_has (GPtrArray   *array,
                                const gchar *text);
gboolean speech_page_server_stat (struct stat *st);
void speech_page_scan_modules (SpeechPage *page);
void speech_page_store_module (SpeechPage  *page,
                               const gchar *module);
void speech_page_fill_modules (SpeechPage *page);
void speech_page_fill_languages (SpeechPage *page);
void speech_page_load_voices (SpeechPage *page);
void speech_page_retry_if_empty (SpeechPage *page);
gchar *speech_page_get_module (SpeechPage *page);
gchar *speech_page_get_language (SpeechPage *page);
gchar *speech_page_get_voice_type (SpeechPage *page);
SPDVoice *speech_page_find_voice (SpeechPage  *page,
                                  const gchar *name,
                                  const gchar *language);
void speech_page_update_voices (SpeechPage *page);
gchar *speech_page_gpu_twin (SpeechPage  *page,
                             const gchar *module);
void speech_page_sync_gpu (SpeechPage *page);
const gchar *speech_page_voice_model (const gchar *module,
                                      const gchar *voice);
void speech_page_fill_models (SpeechPage *page);
gboolean speech_page_ensure_voice (SpeechPage  *page,
                                   const gchar *prefer);
gboolean speech_page_is_language_code (const gchar *text);
gchar *speech_page_locale_language (void);
gboolean speech_page_refill_modules (gpointer data);
gboolean speech_page_watch_modules (gpointer data);

/* speech-page-synth.c */
gchar *speech_page_module_key (const gchar *module,
                               const gchar *suffix);
void speech_page_set_module_option (SpeechPage  *page,
                                    const gchar *module,
                                    const gchar *key,
                                    const gchar *value);
gchar *speech_page_preload_value (SpeechPage  *page,
                                  const gchar *module);
void speech_page_park_preloads (SpeechPage *page);
void speech_page_shrink_window (GtkWidget *toplevel);
void speech_page_show_load_state (SpeechPage *page);
void speech_page_watch_load_state (SpeechPage *page);
void speech_page_request_load (SpeechPage *page);
void speech_page_release_loads (SpeechPage *page);
void speech_page_schedule_release (SpeechPage *page);
void speech_page_sync_preload_full (SpeechPage *page,
                                    gboolean    follow_voice);
void speech_page_sync_preload (SpeechPage *page);
void speech_page_sync_text_options (SpeechPage *page);
void speech_page_copy_toggled (GtkToggleButton *button,
                               SpeechPage      *page);
void speech_page_copy_entry_activate (GtkEntry   *entry,
                                      SpeechPage *page);
gboolean speech_page_copy_entry_focus_out (GtkWidget  *entry,
                                           GdkEvent   *event,
                                           SpeechPage *page);
void speech_page_copy_browse (GtkButton  *button,
                              SpeechPage *page);
void speech_page_preload_toggled (GtkToggleButton *button,
                                  SpeechPage      *page);

G_END_DECLS

#endif /* !__SPEECH_PAGE_PRIVATE_H__ */
