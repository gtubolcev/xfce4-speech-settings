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

#include <signal.h>
#include <stdlib.h>

#include <gtk/gtk.h>
#include <glib-unix.h>
#ifdef HAVE_XFCE
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#ifdef HAVE_GTK_X11
#include <gdk/gdkx.h>
#include <gtk/gtkx.h>
#endif

#include "speech-page.h"

#ifdef HAVE_GTK_X11
static gint32 opt_socket_id = 0;
#endif
static gboolean opt_version = FALSE;

static GOptionEntry entries[] =
{
#ifdef HAVE_GTK_X11
  { "socket-id", 's', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &opt_socket_id,
    N_("Settings manager socket"), N_("SOCKET ID") },
#endif
  { "version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &opt_version,
    N_("Version information"), NULL },
  { NULL }
};



#ifdef HAVE_GTK_X11
/* seconds the settings manager has to embed the plug */
#define PLUG_EMBED_TIMEOUT 30

static gboolean plug_was_embedded = FALSE;

static void
plug_embedded_changed (GtkPlug    *plug,
                       GParamSpec *pspec,
                       gpointer    data)
{
  if (gtk_plug_get_embedded (plug))
    plug_was_embedded = TRUE;
  else if (plug_was_embedded)
    gtk_main_quit ();
}

static gboolean
plug_embed_timeout (gpointer data)
{
  if (!plug_was_embedded && !gtk_plug_get_embedded (GTK_PLUG (data)))
    {
      g_printerr ("xfce4-speech-settings: the settings manager did not take "
                  "the dialog; exiting\n");
      gtk_main_quit ();
    }
  return G_SOURCE_REMOVE;
}
#endif



/* One dialog at a time: two would show each other stale values, and the
 * one closed first reloads the server under the other and unloads the
 * voices only the other uses. The first one owns APP_ID on the session
 * bus; a second start asks it to show itself and leaves. */
#define APP_ID "org.xfce.SpeechSettings"

static GtkWidget *the_dialog = NULL;   /* NULL when embedded */

static void
app_activate (GApplication *app,
              gpointer      data)
{
  GtkWidget *msg;

  if (the_dialog != NULL)
    {
      gtk_window_present (GTK_WINDOW (the_dialog));
      return;
    }
  /* embedded: a plug cannot raise the settings manager's window */
  msg = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                _("The speech settings are already open in "
                                  "the Settings Manager."));
  gtk_window_set_title (GTK_WINDOW (msg), _("Speech"));
  gtk_window_set_icon_name (GTK_WINDOW (msg), PACKAGE);
  g_signal_connect (msg, "response", G_CALLBACK (gtk_widget_destroy), NULL);
  gtk_widget_show (msg);
}

/* TRUE when another dialog runs; it was asked to show itself. Without a
 * session bus each start is on its own, as before. */
static gboolean
another_instance (GApplication **app_out)
{
  GApplication *app = g_application_new (APP_ID, (GApplicationFlags) 0);
  GError       *error = NULL;

  *app_out = app;
  g_signal_connect (app, "activate", G_CALLBACK (app_activate), NULL);
  if (!g_application_register (app, NULL, &error))
    {
      g_printerr ("xfce4-speech-settings: %s\n", error->message);
      g_error_free (error);
      return FALSE;
    }
  if (!g_application_get_is_remote (app))
    return FALSE;
  g_application_activate (app);
  if (g_application_get_dbus_connection (app) != NULL)
    g_dbus_connection_flush_sync (g_application_get_dbus_connection (app), NULL, NULL);
  return TRUE;
}



/* logout or kill: save and let go of the voices as on Close */
static gboolean
quit_on_signal (gpointer data)
{
  gtk_main_quit ();
  return G_SOURCE_CONTINUE;
}



static void
dialog_response (GtkDialog *dialog,
                 gint       response,
                 GtkWidget *page)
{
  speech_page_flush (page);
  gtk_main_quit ();
}



static void
close_clicked (GtkButton *button,
               GtkDialog *dialog)
{
  gtk_dialog_response (dialog, GTK_RESPONSE_CLOSE);
}



gint
main (gint    argc,
      gchar **argv)
{
  GtkWidget    *page;
  GtkWidget    *dialog;
  GtkWidget    *button;
  GError       *error = NULL;
  GApplication *app;
  gboolean      other;

#ifdef HAVE_XFCE
  xfce_textdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");
#else
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
#endif

  if (!gtk_init_with_args (&argc, &argv, NULL, entries, PACKAGE, &error))
    {
      if (error != NULL)
        {
          g_print ("%s: %s.\n", PACKAGE, error->message);
          g_print (_("Type '%s --help' for usage."), PACKAGE);
          g_print ("\n");
          g_error_free (error);
        }
      else
        g_error ("Unable to open display.");

      return EXIT_FAILURE;
    }

  if (opt_version)
    {
#ifdef HAVE_XFCE
      g_print ("%s %s (Xfce %s)\n", PACKAGE, PACKAGE_VERSION,
               xfce_version_string ());
#else
      g_print ("%s %s\n", PACKAGE, PACKAGE_VERSION);
#endif
      return EXIT_SUCCESS;
    }

  other = another_instance (&app);

#ifdef HAVE_GTK_X11
  if (other && opt_socket_id != 0 && GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    {
      /* the settings manager waits for a plug: give it one that says why */
      GtkWidget *plug = gtk_plug_new (opt_socket_id);
      GtkWidget *label = gtk_label_new (_("The speech settings are already "
                                          "open in another window."));

      gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
      g_object_set (label, "margin", 24, NULL);
      g_signal_connect (plug, "delete-event", G_CALLBACK (gtk_main_quit), NULL);
      g_signal_connect (plug, "notify::embedded", G_CALLBACK (plug_embedded_changed), NULL);
      g_signal_connect (plug, "destroy", G_CALLBACK (gtk_main_quit), NULL);
      g_timeout_add_seconds (PLUG_EMBED_TIMEOUT, plug_embed_timeout, plug);
      gtk_container_add (GTK_CONTAINER (plug), label);
      gtk_widget_show_all (plug);
      gdk_notify_startup_complete ();
      gtk_main ();
      g_object_unref (app);
      return EXIT_SUCCESS;
    }
#endif
  if (other)
    {
      gdk_notify_startup_complete ();
      g_object_unref (app);
      return EXIT_SUCCESS;
    }

  page = speech_page_new ();
  g_unix_signal_add (SIGTERM, quit_on_signal, NULL);
  g_unix_signal_add (SIGINT, quit_on_signal, NULL);
  g_unix_signal_add (SIGHUP, quit_on_signal, NULL);

#ifdef HAVE_GTK_X11
  if (opt_socket_id != 0 && GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    {
      GtkWidget *plug;

      plug = gtk_plug_new (opt_socket_id);
      /* the plug is destroyed on delete-event, the page must outlive it
       * for the last save */
      g_object_ref (page);
      g_signal_connect (plug, "delete-event", G_CALLBACK (gtk_main_quit), NULL);
      /* a plug the settings manager dropped or never took gets no
       * delete-event and would stay as an invisible process */
      g_signal_connect (plug, "notify::embedded", G_CALLBACK (plug_embedded_changed), NULL);
      g_signal_connect (plug, "destroy", G_CALLBACK (gtk_main_quit), NULL);
      g_timeout_add_seconds (PLUG_EMBED_TIMEOUT, plug_embed_timeout, plug);
      gtk_container_add (GTK_CONTAINER (plug), page);
      gtk_widget_show_all (plug);

      gdk_notify_startup_complete ();

      /* keep the dialog out of the session */
      gdk_x11_set_sm_client_id ("FAKE ID");

      gtk_main ();

      speech_page_flush (page);
      g_object_unref (page);
      g_object_unref (app);
      return EXIT_SUCCESS;
    }
#endif

  /* no buttons in the action area: the theme gives it a margin of its own,
   * so Close would not end where the controls above it end */
#ifdef HAVE_XFCE
  dialog = xfce_titled_dialog_new ();
#else
  dialog = gtk_dialog_new ();
#endif
  gtk_window_set_title (GTK_WINDOW (dialog), _("Speech"));
  gtk_window_set_icon_name (GTK_WINDOW (dialog), PACKAGE);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 560, -1);
  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                      page, TRUE, TRUE, 0);

#ifdef HAVE_XFCE
  button = xfce_gtk_button_new_mixed ("window-close-symbolic", _("_Close"));
#else
  button = gtk_button_new_with_mnemonic (_("_Close"));
  gtk_button_set_image (GTK_BUTTON (button),
                        gtk_image_new_from_icon_name ("window-close-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
#endif
  g_signal_connect (button, "clicked", G_CALLBACK (close_clicked), dialog);
  speech_page_add_close (page, button);

  g_signal_connect (dialog, "response", G_CALLBACK (dialog_response), page);
  /* Escape or the window manager's close button destroy the dialog after
   * the response (which flushed the page): page is NULL then */
  g_object_add_weak_pointer (G_OBJECT (page), (gpointer *) &page);
  the_dialog = dialog;
  gtk_widget_show_all (dialog);

#ifdef HAVE_GTK_X11
  if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    gdk_x11_set_sm_client_id ("FAKE ID");
#endif

  gtk_main ();

  /* also after a signal, which bypasses the response */
  if (page != NULL)
    speech_page_flush (page);
  g_object_unref (app);

  return EXIT_SUCCESS;
}
