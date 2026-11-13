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

#include <gtk/gtk.h>
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
  GtkWidget *page;
  GtkWidget *dialog;
  GtkWidget *button;
  GError    *error = NULL;

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

  page = speech_page_new ();

#ifdef HAVE_GTK_X11
  if (opt_socket_id != 0 && GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    {
      GtkWidget *plug;

      plug = gtk_plug_new (opt_socket_id);
      g_signal_connect (plug, "delete-event", G_CALLBACK (gtk_main_quit), NULL);
      gtk_container_add (GTK_CONTAINER (plug), page);
      gtk_widget_show_all (plug);

      gdk_notify_startup_complete ();

      /* keep the dialog out of the session */
      gdk_x11_set_sm_client_id ("FAKE ID");

      gtk_main ();

      speech_page_flush (page);
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
  gtk_window_set_icon_name (GTK_WINDOW (dialog), "audio-speakers");
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
  gtk_widget_show_all (dialog);

#ifdef HAVE_GTK_X11
  if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    gdk_x11_set_sm_client_id ("FAKE ID");
#endif

  gtk_main ();

  return EXIT_SUCCESS;
}
