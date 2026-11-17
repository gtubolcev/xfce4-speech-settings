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
 * Installs a package of the distribution through PackageKit on the system
 * bus, without libpackagekit-glib2: Resolve the name, then InstallPackages.
 * When there is no PackageKit, no such package, or the user may not install
 * packages, the result is VI_PACKAGE_UNKNOWN and the caller downloads the
 * voice itself.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#ifdef HAVE_XFCE
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#include "voice-installer-private.h"

#define PK_NAME             "org.freedesktop.PackageKit"
#define PK_PATH             "/org/freedesktop/PackageKit"
#define PK_INTERFACE        "org.freedesktop.PackageKit"
#define PK_TRANSACTION      "org.freedesktop.PackageKit.Transaction"

/* enums of PackageKit; filters and flags are bit fields of them */
#define PK_FILTER_NONE      (G_GUINT64_CONSTANT (1) << 1)
#define PK_FLAG_TRUSTED     (G_GUINT64_CONSTANT (1) << 1)
#define PK_INFO_INSTALLED   1
#define PK_INFO_AVAILABLE   2
#define PK_EXIT_SUCCESS     1
#define PK_EXIT_CANCELLED   3
#define PK_ERROR_NOT_AUTHORIZED 48

/* a Resolve that hangs is given up */
#define RESOLVE_TIMEOUT_S   60

typedef struct
{
  GTask           *task;           /* NULL: returned */
  GDBusConnection *bus;
  gchar           *name;
  gboolean         installing;
  gchar           *transaction;
  guint            signal_id;
  guint            watch_id;
  guint            timeout_id;
  gulong           cancel_id;

  gboolean         installed;
  gchar           *available;      /* package id */
  guint            error_code;
  gchar           *error_details;
} PkJob;



static void
pk_job_free (gpointer data)
{
  PkJob *job = data;

  g_free (job->name);
  g_free (job->transaction);
  g_free (job->available);
  g_free (job->error_details);
  g_clear_object (&job->bus);
  g_free (job);
}



static void
pk_job_stop_transaction (PkJob *job)
{
  if (job->signal_id != 0)
    g_dbus_connection_signal_unsubscribe (job->bus, job->signal_id);
  job->signal_id = 0;
  if (job->timeout_id != 0)
    g_source_remove (job->timeout_id);
  job->timeout_id = 0;
  g_clear_pointer (&job->transaction, g_free);
}



/* Returns the task once; @error NULL: @result */
static void
pk_job_return (PkJob           *job,
               ViPackageResult  result,
               GError          *error)
{
  GTask *task = job->task;

  if (task == NULL)
    return;
  job->task = NULL;

  pk_job_stop_transaction (job);
  if (job->watch_id != 0)
    g_bus_unwatch_name (job->watch_id);
  job->watch_id = 0;
  if (job->cancel_id != 0)
    g_cancellable_disconnect (g_task_get_cancellable (task), job->cancel_id);
  job->cancel_id = 0;

  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_int (task, result);
  g_object_unref (task);
}



static void pk_job_start (PkJob *job);

static void
pk_job_finished (PkJob   *job,
                 guint32  exit_code)
{
  pk_job_stop_transaction (job);

  if (!job->installing)
    {
      if (job->installed)
        pk_job_return (job, VI_PACKAGE_INSTALLED, NULL);
      else if (job->available != NULL)
        {
          job->installing = TRUE;
          pk_job_start (job);
        }
      else
        pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
      return;
    }

  if (exit_code == PK_EXIT_SUCCESS)
    pk_job_return (job, VI_PACKAGE_INSTALLED, NULL);
  else if (exit_code == PK_EXIT_CANCELLED
           || g_cancellable_is_cancelled (g_task_get_cancellable (job->task)))
    pk_job_return (job, 0, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                _("Cancelled")));
  else if (job->error_code == PK_ERROR_NOT_AUTHORIZED)
    /* no administrator: the voice goes to the home directory instead */
    pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
  else
    pk_job_return (job, 0, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                        _("the package %s could not be installed: %s"),
                                        job->name,
                                        job->error_details != NULL
                                        ? job->error_details : _("unknown error")));
}



static void
pk_job_signal (GDBusConnection *bus,
               const gchar     *sender,
               const gchar     *path,
               const gchar     *interface,
               const gchar     *signal,
               GVariant        *parameters,
               gpointer         data)
{
  PkJob *job = data;

  if (job->task == NULL || g_strcmp0 (path, job->transaction) != 0)
    return;

  if (strcmp (signal, "Package") == 0
      && g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uss)")))
    {
      guint32      info;
      const gchar *id;
      gsize        len = strlen (job->name);

      g_variant_get (parameters, "(u&s&s)", &info, &id, NULL);
      /* "rhvoice-ukrainian;1.26-1;all;ubuntu" */
      if (strncmp (id, job->name, len) != 0 || id[len] != ';')
        return;
      if (info == PK_INFO_INSTALLED)
        job->installed = TRUE;
      else if (info == PK_INFO_AVAILABLE && job->available == NULL)
        job->available = g_strdup (id);
    }
  else if (strcmp (signal, "ErrorCode") == 0
           && g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(us)")))
    {
      const gchar *details;

      g_variant_get (parameters, "(u&s)", &job->error_code, &details);
      g_free (job->error_details);
      job->error_details = g_strdup (details);
    }
  else if (strcmp (signal, "Finished") == 0
           && g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    {
      guint32 exit_code;

      g_variant_get (parameters, "(uu)", &exit_code, NULL);
      pk_job_finished (job, exit_code);
    }
}



static void
pk_job_called (GObject      *source,
               GAsyncResult *result,
               gpointer      data)
{
  GTask    *task = data;
  PkJob    *job = g_task_get_task_data (task);
  GError   *error = NULL;
  GVariant *reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                   result, &error);

  if (reply != NULL)
    g_variant_unref (reply);
  else if (job->task != NULL)
    {
      if (job->installing && !g_dbus_error_is_remote_error (error))
        pk_job_return (job, 0, error);
      else
        {
          /* refused by PackageKit: download instead */
          g_error_free (error);
          pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
        }
      error = NULL;
    }
  g_clear_error (&error);
  g_object_unref (task);
}



static void
pk_job_call (PkJob       *job,
             const gchar *method,
             GVariant    *parameters)
{
  g_dbus_connection_call (job->bus, PK_NAME, job->transaction, PK_TRANSACTION,
                          method, parameters, NULL, G_DBUS_CALL_FLAGS_NONE,
                          /* InstallPackages returns at once, the work follows */
                          30000, NULL, pk_job_called, g_object_ref (job->task));
}



static gboolean
pk_job_timeout (gpointer data)
{
  PkJob *job = data;

  job->timeout_id = 0;
  pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);

  return G_SOURCE_REMOVE;
}



static void
pk_job_created (GObject      *source,
                GAsyncResult *result,
                gpointer      data)
{
  GTask        *task = data;
  PkJob        *job = g_task_get_task_data (task);
  GVariant     *reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                       result, NULL);
  const gchar  *hints[] = { "interactive=true", NULL };

  if (job->task == NULL)
    goto out;

  if (reply == NULL)
    {
      /* no PackageKit */
      if (job->installing)
        pk_job_return (job, 0, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                    _("PackageKit is not available")));
      else
        pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
      goto out;
    }

  g_variant_get (reply, "(o)", &job->transaction);
  job->error_code = 0;
  g_clear_pointer (&job->error_details, g_free);
  job->signal_id = g_dbus_connection_signal_subscribe (job->bus, PK_NAME, PK_TRANSACTION,
                                                       NULL, job->transaction, NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       pk_job_signal, job, NULL);
  pk_job_call (job, "SetHints", g_variant_new ("(^as)", hints));

  if (job->installing)
    {
      const gchar *ids[] = { job->available, NULL };

      pk_job_call (job, "InstallPackages", g_variant_new ("(t^as)", PK_FLAG_TRUSTED, ids));
    }
  else
    {
      const gchar *names[] = { job->name, NULL };

      pk_job_call (job, "Resolve", g_variant_new ("(t^as)", PK_FILTER_NONE, names));
      job->timeout_id = g_timeout_add_seconds (RESOLVE_TIMEOUT_S, pk_job_timeout, job);
    }

out:
  if (reply != NULL)
    g_variant_unref (reply);
  g_object_unref (task);
}



static void
pk_job_start (PkJob *job)
{
  g_dbus_connection_call (job->bus, PK_NAME, PK_PATH, PK_INTERFACE,
                          "CreateTransaction", NULL, G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
                          pk_job_created, g_object_ref (job->task));
}



static void
pk_job_vanished (GDBusConnection *bus,
                 const gchar     *name,
                 gpointer         data)
{
  PkJob *job = data;

  /* only while a transaction runs: PackageKit starts on demand */
  if (job->transaction == NULL)
    return;
  if (job->installing)
    pk_job_return (job, 0, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                _("PackageKit stopped")));
  else
    pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
}



static void
pk_job_cancelled (GCancellable *cancellable,
                  gpointer      data)
{
  PkJob *job = data;

  /* the Finished signal follows */
  if (job->transaction != NULL)
    g_dbus_connection_call (job->bus, PK_NAME, job->transaction, PK_TRANSACTION,
                            "Cancel", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, NULL, NULL);
}



static gboolean
pk_job_cancel_idle (gpointer data)
{
  GTask *task = data;
  PkJob *job = g_task_get_task_data (task);

  if (job->transaction != NULL)
    pk_job_cancelled (NULL, job);
  else
    pk_job_return (job, 0, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                _("Cancelled")));
  g_object_unref (task);

  return G_SOURCE_REMOVE;
}



static void
pk_job_cancelled_cb (GCancellable *cancellable,
                     gpointer      data)
{
  /* not from within g_cancellable_connect() or cancel(): later */
  g_idle_add (pk_job_cancel_idle, g_object_ref (data));
}



static void
pk_job_got_bus (GObject      *source,
                GAsyncResult *result,
                gpointer      data)
{
  GTask *task = data;
  PkJob *job = g_task_get_task_data (task);

  job->bus = g_bus_get_finish (result, NULL);
  if (job->bus == NULL)
    pk_job_return (job, VI_PACKAGE_UNKNOWN, NULL);
  else if (job->task != NULL)
    {
      job->watch_id = g_bus_watch_name_on_connection (job->bus, PK_NAME,
                                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                      NULL, pk_job_vanished, job, NULL);
      pk_job_start (job);
    }
  g_object_unref (task);
}



/* Installs the package @name if the distribution has it and it is not
 * there yet. PackageKit asks for the password itself. */
void
vi_package_install_async (const gchar         *name,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, data);
  PkJob *job = g_new0 (PkJob, 1);

  job->task = g_object_ref (task);
  job->name = g_strdup (name);
  g_task_set_task_data (task, job, pk_job_free);

  if (cancellable != NULL)
    job->cancel_id = g_cancellable_connect (cancellable, G_CALLBACK (pk_job_cancelled_cb),
                                            task, NULL);
  g_bus_get (G_BUS_TYPE_SYSTEM, cancellable, pk_job_got_bus, g_object_ref (task));
  g_object_unref (task);
}



ViPackageResult
vi_package_install_finish (GAsyncResult *result,
                           GError      **error)
{
  GError *local = NULL;
  gssize  value = g_task_propagate_int (G_TASK (result), &local);

  if (local != NULL)
    {
      g_propagate_error (error, local);
      return VI_PACKAGE_UNKNOWN;
    }

  return (ViPackageResult) value;
}
