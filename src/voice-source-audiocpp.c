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
 * Models for the synthesizer audiocpp (audio.cpp, ggml) that the packages
 * cannot hold, as their licences restrict their use (OpenRAIL): Supertonic
 * 3, the Russian Kokoro, TeraTTS. They go into ~/.local/share/audiocpp-tts,
 * where sd_audiocpp finds them.
 *
 * All the work is done by audiocpp-models.py of speech-dispatcher-audiocpp,
 * which has the list (with every file pinned by its SHA-256), downloads,
 * converts and checks; it prints its progress as one JSON object a line.
 * Here it is only run: "list" for the catalogue, "install ID", "remove ID".
 *
 * The same helper with another catalogue, as ruaccent-models.py of
 * speech-dispatcher-text-normalizer, installs the RUAccent homograph models
 * (MIT) into ~/.local/share/linux-tts/ruaccent: voice_source_ruaccent, for
 * "TextStressHomographs tiny|turbo" of the modules.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <signal.h>
#include <string.h>

#include <gio/gio.h>
#ifdef HAVE_XFCE
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#include "voice-installer-private.h"

#define HELPER "/usr/share/speech-dispatcher-audiocpp/audiocpp-models.py"
#define SYSTEM_DIR "/usr/share/audiocpp-tts"
#ifndef RUACCENT_HELPER   /* -D for tests */
#define RUACCENT_HELPER "/usr/share/linux-tts/ruaccent/ruaccent-models.py"
#endif
#define LINE_MAX_SIZE 4096

typedef struct
{
  gchar  *id;          /* also the directory name */
  gchar  *label;
  gchar  *licence;     /* "OpenRAIL-M" */
  gchar  *licence_url;
  gchar  *missing;     /* packages it needs that are not installed, or NULL */
} AcppModel;

/* one installation */
typedef struct
{
  Installer        *in;
  AcppModel        *model;
  GSubprocess      *process;
  GDataInputStream *lines;
  gulong            cancel_id;
  GError           *error;      /* from an "error" line */
  gboolean          done;       /* a "done" line came */
} AcppInstall;



/* what differs between the two catalogues */
typedef struct
{
  const gchar *helper;
  const gchar *helper_env;   /* for a test with the source tree */
  const gchar *user_dir;     /* under ~/.local/share */
  const gchar *system_dir;   /* NULL: none */
  const gchar *marker;       /* the file that makes a model directory */
} AcppKind;

static const AcppKind kind_audiocpp =
{
  HELPER, "AUDIOCPP_MODELS_HELPER", "audiocpp-tts", SYSTEM_DIR, "audiocpp.ini"
};

static const AcppKind kind_ruaccent =
{
  RUACCENT_HELPER, "RUACCENT_MODELS_HELPER", "linux-tts/ruaccent", "/usr/share/linux-tts/ruaccent", "model.onnx"
};

static const AcppKind *
acpp_kind (Installer *in)
{
  return in->source == &voice_source_ruaccent ? &kind_ruaccent : &kind_audiocpp;
}

static const gchar *
acpp_helper (const AcppKind *k)
{
  const gchar *path = g_getenv (k->helper_env);

  return path != NULL && *path != '\0' ? path : k->helper;
}

static gchar **
acpp_catalogue_argv (const AcppKind *k)
{
  gchar *argv[] = { "python3", "-I", (gchar *) acpp_helper (k), "list", NULL };

  if (!g_file_test (acpp_helper (k), G_FILE_TEST_IS_REGULAR))
    return NULL;
  return g_strdupv (argv);
}

static gchar **
acpp_catalogue_command (void)
{
  return acpp_catalogue_argv (&kind_audiocpp);
}

static gchar **
ruaccent_catalogue_command (void)
{
  return acpp_catalogue_argv (&kind_ruaccent);
}

static gchar *
acpp_user_dir (const AcppKind *k)
{
  return g_build_filename (g_get_user_data_dir (), k->user_dir, NULL);
}

static void
acpp_free_voice (gpointer data)
{
  AcppModel *model = data;

  g_free (model->id);
  g_free (model->label);
  g_free (model->licence);
  g_free (model->licence_url);
  g_free (model->missing);
  g_free (model);
}



static gboolean
acpp_valid_id (const gchar *id)
{
  const gchar *p;

  if (id == NULL || !g_ascii_isalnum (*id) || strlen (id) > 64)
    return FALSE;
  for (p = id; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && strchr ("._-", *p) == NULL)
      return FALSE;
  return TRUE;
}

/* the output of "list": {"dir": ..., "models": [{id, label, languages,
 * download, licence: {spdx, url}, missing: [...]}, ...]} */
static gboolean
acpp_load (Installer *in,
           JsonNode  *root,
           GError   **error)
{
  JsonArray *models = JSON_NODE_HOLDS_OBJECT (root)
                      ? vi_json_array (json_node_get_object (root), "models") : NULL;
  guint      i;

  if (models == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           _("The list of models is damaged"));
      return FALSE;
    }

  for (i = 0; i < json_array_get_length (models); i++)
    {
      JsonNode    *node = json_array_get_element (models, i);
      JsonObject  *object, *licence;
      JsonArray   *languages, *missing;
      const gchar *id, *label;
      guint        k;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        continue;
      object = json_node_get_object (node);
      id = vi_json_string (object, "id");
      label = vi_json_string (object, "label");
      languages = vi_json_array (object, "languages");
      if (!acpp_valid_id (id) || label == NULL || languages == NULL)
        continue;
      licence = vi_json_object (object, "licence");
      missing = vi_json_array (object, "missing");

      /* a row per language, each with its own copy */
      for (k = 0; k < json_array_get_length (languages); k++)
        {
          JsonNode    *lnode = json_array_get_element (languages, k);
          const gchar *language = JSON_NODE_HOLDS_VALUE (lnode)
                                  ? json_node_get_string (lnode) : NULL;
          AcppModel   *model;
          GString     *needs = NULL;
          guint        m;

          if (language == NULL || *language == '\0')
            continue;
          model = g_new0 (AcppModel, 1);
          model->id = g_strdup (id);
          model->label = g_strdup (label);
          model->licence = g_strdup (vi_json_string (licence, "spdx"));
          model->licence_url = g_strdup (vi_json_string (licence, "url"));
          for (m = 0; missing != NULL && m < json_array_get_length (missing); m++)
            {
              const gchar *pkg = json_array_get_string_element (missing, m);

              if (pkg == NULL)
                continue;
              if (needs == NULL)
                needs = g_string_new (pkg);
              else
                g_string_append_printf (needs, ", %s", pkg);
            }
          model->missing = needs != NULL ? g_string_free (needs, FALSE) : NULL;
          installer_add_voice (in, model, language, language, label,
                               model->licence != NULL ? model->licence : "",
                               vi_json_int (object, "download", 0));
        }
    }

  return TRUE;
}



/* installed by the helper (it writes SOURCE), by a package, or by hand */
static VoiceState
acpp_state (Installer *in,
            gpointer   data)
{
  AcppModel      *model = data;
  const AcppKind *k = acpp_kind (in);
  gchar          *user_dir = acpp_user_dir (k);
  gchar          *ini = g_build_filename (user_dir, model->id, k->marker, NULL);
  gchar          *source = g_build_filename (user_dir, model->id, "SOURCE", NULL);
  gchar          *system_ini = k->system_dir != NULL
                               ? g_build_filename (k->system_dir, model->id, k->marker, NULL) : NULL;
  VoiceState      state = STATE_NONE;

  if (g_file_test (ini, G_FILE_TEST_IS_REGULAR))
    /* one put there by hand is not removed here */
    state = g_file_test (source, G_FILE_TEST_IS_REGULAR) ? STATE_USER : STATE_SYSTEM;
  else if (system_ini != NULL && g_file_test (system_ini, G_FILE_TEST_IS_REGULAR))
    state = STATE_SYSTEM;

  g_free (system_ini);
  g_free (source);
  g_free (ini);
  g_free (user_dir);

  return state;
}



static void
acpp_licence (Installer *in,
              gpointer   data)
{
  AcppModel *model = data;
  gchar     *text;

  if (model == NULL)
    {
      installer_set_licence (in, NULL);
      return;
    }
  if (model->missing != NULL)
    {
      /* TRANSLATORS: %s are the names of packages */
      text = g_strdup_printf (_("needs the packages %s"), model->missing);
      installer_set_status (in, text);
      g_free (text);
    }
  else if (!in->busy)
    installer_set_status (in, NULL);

  text = g_strdup_printf ("%s, %s",
                          model->licence != NULL ? model->licence : _("not stated"),
                          model->licence_url != NULL ? model->licence_url : "");
  installer_set_licence (in, text);
  g_free (text);
}



/* ------------------------------------------------------------- install */

static void acpp_read_line (AcppInstall *job);

static void
acpp_install_free (AcppInstall *job)
{
  if (job->cancel_id != 0)
    g_cancellable_disconnect (job->in->cancellable, job->cancel_id);
  g_clear_object (&job->lines);
  g_clear_object (&job->process);
  g_clear_error (&job->error);
  g_free (job);
}

static void
acpp_install_exited (GObject      *source,
                     GAsyncResult *result,
                     gpointer      data)
{
  AcppInstall *job = data;
  Installer   *in = job->in;
  GError      *error = NULL;

  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);
  if (g_cancellable_is_cancelled (in->cancellable))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  else if (job->error != NULL)
    error = g_error_copy (job->error);
  else if (!job->done || !g_subprocess_get_if_exited (job->process)
           || g_subprocess_get_exit_status (job->process) != 0)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 _("the installer stopped unexpectedly"));

  acpp_install_free (job);
  installer_finish (in, error);
  g_clear_error (&error);
}

static void
acpp_handle_line (AcppInstall *job,
                  const gchar *line)
{
  Installer   *in = job->in;
  JsonParser  *parser = json_parser_new ();
  JsonObject  *object;
  const gchar *stage;

  if (!json_parser_load_from_data (parser, line, -1, NULL)
      || json_parser_get_root (parser) == NULL
      || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      g_object_unref (parser);
      return;
    }
  object = json_node_get_object (json_parser_get_root (parser));
  stage = vi_json_string (object, "stage");

  if (g_strcmp0 (stage, "download") == 0)
    {
      gint64 done = vi_json_int (object, "done", 0);
      gint64 total = vi_json_int (object, "total", 0);

      if (total > 0 && done >= 0)
        {
          gchar *a = g_format_size (done);
          gchar *b = g_format_size (total);
          /* TRANSLATORS: "120 MB of 454 MB" */
          gchar *text = g_strdup_printf (_("Downloading… %s of %s"), a, b);

          /* the download is most of the time; the conversion the rest */
          installer_set_progress (in, 0.9 * done / total);
          installer_set_status (in, text);
          g_free (text);
          g_free (b);
          g_free (a);
        }
    }
  else if (g_strcmp0 (stage, "convert") == 0)
    {
      installer_set_progress (in, 0.9);
      installer_set_status (in, _("Converting the model…"));
    }
  else if (g_strcmp0 (stage, "check") == 0)
    {
      installer_set_progress (in, 0.97);
      installer_set_status (in, _("Checking…"));
    }
  else if (g_strcmp0 (stage, "done") == 0)
    {
      installer_set_progress (in, 1.0);
      job->done = TRUE;
    }
  else if (g_strcmp0 (stage, "error") == 0 && job->error == NULL)
    {
      const gchar *message = vi_json_string (object, "message");
      const gchar *code = vi_json_string (object, "code");

      if (g_strcmp0 (code, "space") == 0)
        job->error = g_error_new (G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("not enough free disk space (%s)"),
                                  message != NULL ? message : "");
      else if (g_strcmp0 (code, "checksum") == 0)
        job->error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                  _("a downloaded file is not the expected one (%s)"),
                                  message != NULL ? message : "");
      else
        job->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                          message != NULL ? message : "?");
    }

  g_object_unref (parser);
}

static void
acpp_line_read (GObject      *source,
                GAsyncResult *result,
                gpointer      data)
{
  AcppInstall *job = data;
  gsize        length = 0;
  gchar       *line;

  line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (source),
                                                    result, &length, NULL);
  if (line == NULL)
    {
      /* end of the output (or a broken line): wait for the exit */
      g_subprocess_wait_async (job->process, NULL, acpp_install_exited, job);
      return;
    }
  if (length <= LINE_MAX_SIZE)
    acpp_handle_line (job, line);
  g_free (line);
  acpp_read_line (job);
}

static void
acpp_read_line (AcppInstall *job)
{
  g_data_input_stream_read_line_async (job->lines, G_PRIORITY_DEFAULT, NULL,
                                       acpp_line_read, job);
}

/* Cancel or a closed dialog: the helper removes what it downloaded */
static void
acpp_cancelled (GCancellable *cancellable,
                gpointer      data)
{
  AcppInstall *job = data;

  if (job->process != NULL)
    g_subprocess_send_signal (job->process, SIGTERM);
}

static void
acpp_install (Installer *in,
              gpointer   data)
{
  AcppModel   *model = data;
  AcppInstall *job;
  GError      *error = NULL;

  if (model->missing != NULL)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("needs the packages %s"), model->missing);
      installer_finish (in, error);
      g_error_free (error);
      return;
    }

  job = g_new0 (AcppInstall, 1);
  job->in = in;
  job->model = model;
  job->process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                   | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
                                   "python3", "-I", acpp_helper (acpp_kind (in)), "install",
                                   model->id, NULL);
  if (job->process == NULL)
    {
      acpp_install_free (job);
      installer_finish (in, error);
      g_error_free (error);
      return;
    }
  job->lines = g_data_input_stream_new (g_subprocess_get_stdout_pipe (job->process));
  job->cancel_id = g_cancellable_connect (in->cancellable, G_CALLBACK (acpp_cancelled),
                                          job, NULL);
  installer_set_status (in, _("Connecting…"));
  acpp_read_line (job);
}



static gboolean
acpp_remove (Installer *in,
             gpointer   data,
             GError   **error)
{
  AcppModel *model = data;
  gchar     *argv[] = { "python3", "-I", (gchar *) acpp_helper (acpp_kind (in)), "remove",
                        model->id, NULL };
  gchar     *out = NULL;
  gint       status = 0;
  gboolean   ok;

  ok = g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &out, NULL, &status, error);
  if (ok && !g_spawn_check_wait_status (status, NULL))
    {
      JsonParser  *parser = json_parser_new ();
      const gchar *message = NULL;

      if (out != NULL && json_parser_load_from_data (parser, out, -1, NULL)
          && json_parser_get_root (parser) != NULL
          && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
        message = vi_json_string (json_node_get_object (json_parser_get_root (parser)),
                                  "message");
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           message != NULL ? message : "?");
      g_object_unref (parser);
      ok = FALSE;
    }
  g_free (out);

  return ok;
}



const VoiceSource voice_source_audiocpp =
{
  .module             = "audiocpp",
  .module_path        = "/usr/lib/speech-dispatcher-modules/sd_audiocpp",
  .title              = N_("Models for audio.cpp"),
  .install_tooltip    = N_("Download the model from Hugging Face, convert it for "
                           "audio.cpp and install it for you"),
  .quality_title      = N_("Licence"),
  .note               = N_("These synthesizers are not in the packages: their "
                           "licences (OpenRAIL) allow free use but forbid some "
                           "uses, such as harming people, impersonating them or "
                           "deceiving with the voice. By installing a model you "
                           "accept its licence. The model is downloaded from its "
                           "authors' page, checked and converted on this "
                           "computer."),
  .catalogue_command  = acpp_catalogue_command,
  .free_voice         = acpp_free_voice,
  .load               = acpp_load,
  .state              = acpp_state,
  .install            = acpp_install,
  .remove             = acpp_remove,
  .licence            = acpp_licence,
};

const VoiceSource voice_source_ruaccent =
{
  /* not a module: the models of TextStressHomographs, for every module */
  .module             = "ruaccent",
  .module_path        = RUACCENT_HELPER,
  .title              = N_("Models for Russian homographs"),
  .install_tooltip    = N_("Download the model from Hugging Face and install it for you"),
  .quality_title      = N_("Licence"),
  .note               = N_("RUAccent models (MIT licence) choose the stress of "
                           "Russian homographs from the sentence: за́мок or "
                           "замо́к, все or всё. The small one is fast enough "
                           "for any computer, the large one is a little more "
                           "accurate. They are downloaded from their authors' "
                           "page and checked."),
  .catalogue_command  = ruaccent_catalogue_command,
  .free_voice         = acpp_free_voice,
  .load               = acpp_load,
  .state              = acpp_state,
  .install            = acpp_install,
  .remove             = acpp_remove,
  .licence            = acpp_licence,
};
