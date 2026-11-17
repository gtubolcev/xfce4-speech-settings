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
 * What the voice window shares with the sources of voices: one source per
 * synthesizer, which knows its list of voices, where a voice goes and how
 * it is installed and removed. The window does the rest.
 */

#ifndef __VOICE_INSTALLER_PRIVATE_H__
#define __VOICE_INSTALLER_PRIVATE_H__

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "voice-installer.h"

G_BEGIN_DECLS

typedef enum
{
  STATE_NONE,
  STATE_USER,      /* in the home directory: can be removed */
  STATE_SYSTEM     /* from a package */
} VoiceState;

typedef struct _Installer   Installer;
typedef struct _VoiceSource VoiceSource;

struct _VoiceSource
{
  /* the speechd module, also with a suffix ("sherpa-onnx-vulkan-rs") */
  const gchar  *module;
  /* its program, which must be there */
  const gchar  *module_path;
  /* marked with N_() */
  const gchar  *title;
  const gchar  *install_tooltip;
  const gchar  *quality_title;    /* NULL: no such column */
  gboolean      no_size;          /* the catalogue gives no sizes */
  const gchar  *catalogue_url;
  const gchar  *cache_name;       /* in ~/.cache/PACKAGE */

  gpointer    (*new_data)   (void);
  void        (*free_data)  (gpointer     data);
  void        (*free_voice) (gpointer     voice);
  /* the catalogue into installer_add_voice(); FALSE: it is not usable */
  gboolean    (*load)       (Installer   *in,
                             JsonNode    *root,
                             GError     **error);
  VoiceState  (*state)      (Installer   *in,
                             gpointer     voice);
  /* ends with installer_finish() */
  void        (*install)    (Installer   *in,
                             gpointer     voice);
  gboolean    (*remove)     (Installer   *in,
                             gpointer     voice,
                             GError     **error);
  /* with installer_set_licence() */
  void        (*licence)    (Installer   *in,
                             gpointer     voice);
};

typedef void (*InstallerDownloadDone) (Installer   *in,
                                       const gchar *md5,     /* NULL: failed */
                                       GError      *error,
                                       gpointer     data);

struct _Installer
{
  const VoiceSource     *source;
  gpointer               data;       /* of the source */

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
  GPtrArray             *voices;     /* owns what the rows point to */

  SoupSession           *session;
  GCancellable          *cancellable;
  gchar                 *language;
  VoiceInstallerChanged  changed;
  gpointer               changed_data;

  /* the voice being installed */
  gboolean               busy;
  gpointer               voice;
  gchar                 *unpack;     /* removed when done */

  /* the file being downloaded */
  GInputStream          *in;
  GOutputStream         *out;
  GChecksum             *md5;
  goffset                total;
  goffset                done;
  goffset                max;
  gdouble                base;       /* its part of the progress bar */
  gdouble                span;
  InstallerDownloadDone  download_done;
  gpointer               download_data;
};

Installer   *installer_ref          (Installer             *in);
void         installer_unref        (Installer             *in);
void         installer_add_voice    (Installer             *in,
                                     gpointer               voice,
                                     const gchar           *family,
                                     const gchar           *language,
                                     const gchar           *name,
                                     const gchar           *quality,
                                     gint64                 bytes);
gpointer     installer_selected     (Installer             *in);
void         installer_set_status   (Installer             *in,
                                     const gchar           *text);
void         installer_set_licence  (Installer             *in,
                                     const gchar           *licence);
void         installer_set_progress (Installer             *in,
                                     gdouble                fraction);
void         installer_update_rows  (Installer             *in);
void         installer_finish       (Installer             *in,
                                     GError                *error);
void         installer_download     (Installer             *in,
                                     const gchar           *url,
                                     const gchar           *path,
                                     goffset                max,
                                     gdouble                base,
                                     gdouble                span,
                                     InstallerDownloadDone  done,
                                     gpointer               data);
gboolean     installer_http_ok      (SoupMessage           *msg,
                                     GError               **error);

gboolean     vi_remove_tree         (const gchar           *path);
gchar       *vi_licence_line        (const gchar           *text,
                                     gsize                  length);
const gchar *vi_json_string         (JsonObject            *object,
                                     const gchar           *member);
JsonObject  *vi_json_object         (JsonObject            *object,
                                     const gchar           *member);
JsonArray   *vi_json_array          (JsonObject            *object,
                                     const gchar           *member);
gint64       vi_json_int            (JsonObject            *object,
                                     const gchar           *member,
                                     gint64                 fallback);

/* voice-unzip.c: a zip archive into @dest, in a thread */
gboolean     vi_unzip               (const gchar           *zip,
                                     const gchar           *dest,
                                     GCancellable          *cancellable,
                                     GError               **error);
void         vi_unzip_async         (const gchar           *zip,
                                     const gchar           *dest,
                                     GCancellable          *cancellable,
                                     GAsyncReadyCallback    callback,
                                     gpointer               data);
gboolean     vi_unzip_finish        (GAsyncResult          *result,
                                     GError               **error);

/* voice-packagekit.c: a package of the distribution through PackageKit */
typedef enum
{
  VI_PACKAGE_UNKNOWN,      /* no such package, or no PackageKit */
  VI_PACKAGE_INSTALLED     /* it was there or is now */
} ViPackageResult;

void            vi_package_install_async  (const gchar         *name,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             data);
ViPackageResult vi_package_install_finish (GAsyncResult        *result,
                                           GError             **error);

extern const VoiceSource voice_source_piper;
extern const VoiceSource voice_source_rhvoice;

G_END_DECLS

#endif /* !__VOICE_INSTALLER_PRIVATE_H__ */
