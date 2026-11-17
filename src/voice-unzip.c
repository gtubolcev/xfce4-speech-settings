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
 * Unpacks the zip archives of RHVoice. Only what such an archive needs:
 * stored and deflated files and directories. Links, encryption, zip64 and
 * names that leave the target directory are refused; the archive comes from
 * the net. The checksum of the whole archive is checked before, so the CRC
 * of each file is not.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#ifdef HAVE_XFCE
#include <libxfce4util/libxfce4util.h>
#else
#include <glib/gi18n.h>
#endif

#include "voice-installer-private.h"

#define EOCD_SIGNATURE    0x06054b50
#define CENTRAL_SIGNATURE 0x02014b50
#define LOCAL_SIGNATURE   0x04034b50
#define EOCD_SIZE         22
#define CENTRAL_SIZE      46
#define LOCAL_SIZE        30
#define MAX_COMMENT       0xffff
/* the largest RHVoice voice is some 100 MB */
#define MAX_UNPACKED      ((guint64) 2 * 1024 * 1024 * 1024)
#define MAX_ENTRIES       10000
#define CHUNK_SIZE        (64 * 1024)

typedef struct
{
  const guint8 *data;
  gsize         size;
} Zip;



static guint16
get16 (const guint8 *p)
{
  return p[0] | (p[1] << 8);
}



static guint32
get32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16)
         | ((guint32) p[3] << 24);
}



static gboolean
damaged (GError    **error,
         const gchar *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               _("the archive is damaged or not supported (%s)"), what);
  return FALSE;
}



/* "voice/data.bin" yes; "", "/etc", "a/../b", "a\b" no */
static gboolean
name_is_safe (const gchar *name)
{
  gchar  **parts;
  gboolean safe = TRUE;
  guint    i;

  if (*name == '\0' || *name == '/' || strchr (name, '\\') != NULL
      || !g_utf8_validate (name, -1, NULL))
    return FALSE;

  parts = g_strsplit (name, "/", -1);
  for (i = 0; parts[i] != NULL; i++)
    {
      /* a trailing slash leaves an empty last part */
      if (strcmp (parts[i], "..") == 0 || strcmp (parts[i], ".") == 0
          || (*parts[i] == '\0' && parts[i + 1] != NULL))
        safe = FALSE;
    }
  g_strfreev (parts);

  return safe;
}



static gboolean
write_all (gint          fd,
           const guint8 *data,
           gsize         size,
           GError      **error)
{
  while (size > 0)
    {
      gssize written = write (fd, data, size);

      if (written < 0)
        {
          if (errno == EINTR)
            continue;
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               g_strerror (errno));
          return FALSE;
        }
      data += written;
      size -= written;
    }

  return TRUE;
}



static gboolean
inflate_to (gint          fd,
            const guint8 *data,
            gsize         csize,
            guint64       usize,
            GCancellable *cancellable,
            GError      **error)
{
  GConverter *zlib = G_CONVERTER (g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW));
  guint8     *buffer = g_malloc (CHUNK_SIZE);
  guint64     out = 0;
  gboolean    ok = FALSE;

  for (;;)
    {
      gsize           read = 0, written = 0;
      GConverterResult result;

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        break;

      result = g_converter_convert (zlib, data, csize, buffer, CHUNK_SIZE,
                                    G_CONVERTER_INPUT_AT_END, &read, &written, error);
      if (result == G_CONVERTER_ERROR)
        break;

      data += read;
      csize -= read;
      out += written;
      if (out > usize)
        {
          damaged (error, "size");
          break;
        }
      if (!write_all (fd, buffer, written, error))
        break;

      if (result == G_CONVERTER_FINISHED)
        {
          ok = out == usize || damaged (error, "size");
          break;
        }
      if (read == 0 && written == 0)
        {
          damaged (error, "deflate");
          break;
        }
    }

  g_free (buffer);
  g_object_unref (zlib);

  return ok;
}



static gboolean
extract_entry (const Zip    *zip,
               const guint8 *central,
               const gchar  *name,
               const gchar  *dest,
               GCancellable *cancellable,
               GError      **error)
{
  guint16       flags = get16 (central + 8);
  guint16       method = get16 (central + 10);
  guint32       csize = get32 (central + 20);
  guint32       usize = get32 (central + 24);
  guint32       offset = get32 (central + 42);
  guint8        host = central[5];
  guint32       mode = get32 (central + 38) >> 16;
  const guint8 *local, *data;
  gchar        *path, *dir;
  gint          fd;
  gboolean      ok;

  if (flags & 1)
    return damaged (error, "encrypted");
  /* made on Unix: the file type is in the upper half of the attributes */
  if (host == 3 && mode != 0 && !S_ISREG (mode) && !S_ISDIR (mode))
    return damaged (error, "link");
  if (!name_is_safe (name))
    return damaged (error, "name");

  path = g_build_filename (dest, name, NULL);
  if (g_str_has_suffix (name, "/"))
    {
      ok = g_mkdir_with_parents (path, 0755) == 0;
      if (!ok)
        g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                             g_strerror (errno));
      g_free (path);
      return ok;
    }

  if (offset > zip->size - LOCAL_SIZE || get32 (zip->data + offset) != LOCAL_SIGNATURE)
    {
      g_free (path);
      return damaged (error, "local header");
    }
  local = zip->data + offset;
  data = local + LOCAL_SIZE + get16 (local + 26) + get16 (local + 28);
  if (data > zip->data + zip->size || csize > (gsize) (zip->data + zip->size - data))
    {
      g_free (path);
      return damaged (error, "size");
    }
  if (method != 0 && method != 8)
    {
      g_free (path);
      return damaged (error, "method");
    }
  if (method == 0 && csize != usize)
    {
      g_free (path);
      return damaged (error, "size");
    }

  dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           g_strerror (errno));
      g_free (dir);
      g_free (path);
      return FALSE;
    }
  g_free (dir);

  /* the target is new and empty: a name twice or a link in the way fails */
  fd = g_open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s",
                   name, g_strerror (errno));
      g_free (path);
      return FALSE;
    }

  if (method == 0)
    ok = write_all (fd, data, csize, error);
  else
    ok = inflate_to (fd, data, csize, usize, cancellable, error);

  if (close (fd) != 0 && ok)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           g_strerror (errno));
      ok = FALSE;
    }
  g_free (path);

  return ok;
}



gboolean
vi_unzip (const gchar  *zip_path,
          const gchar  *dest,
          GCancellable *cancellable,
          GError      **error)
{
  GMappedFile  *mapped;
  Zip           zip;
  const guint8 *eocd = NULL, *central;
  gsize         i, low;
  guint         entries, n;
  guint32       cd_size, cd_offset;
  guint64       unpacked = 0;
  gboolean      ok = TRUE;

  mapped = g_mapped_file_new (zip_path, FALSE, error);
  if (mapped == NULL)
    return FALSE;
  zip.data = (const guint8 *) g_mapped_file_get_contents (mapped);
  zip.size = g_mapped_file_get_length (mapped);

  if (zip.size < EOCD_SIZE)
    {
      g_mapped_file_unref (mapped);
      return damaged (error, "too short");
    }

  /* the end record, before a comment of up to 64 KB */
  low = zip.size - EOCD_SIZE > MAX_COMMENT ? zip.size - EOCD_SIZE - MAX_COMMENT : 0;
  for (i = zip.size - EOCD_SIZE + 1; i-- > low;)
    if (get32 (zip.data + i) == EOCD_SIGNATURE)
      {
        eocd = zip.data + i;
        break;
      }
  if (eocd == NULL)
    {
      g_mapped_file_unref (mapped);
      return damaged (error, "no directory");
    }

  entries = get16 (eocd + 10);
  cd_size = get32 (eocd + 12);
  cd_offset = get32 (eocd + 16);
  if (entries == 0xffff || cd_offset == 0xffffffff || get16 (eocd + 4) != 0)
    {
      g_mapped_file_unref (mapped);
      return damaged (error, "zip64");
    }
  if (entries > MAX_ENTRIES || cd_offset > (gsize) (eocd - zip.data)
      || cd_size > (gsize) (eocd - zip.data) - cd_offset)
    {
      g_mapped_file_unref (mapped);
      return damaged (error, "directory");
    }

  central = zip.data + cd_offset;
  for (n = 0; ok && n < entries; n++)
    {
      gsize  left = zip.data + cd_offset + cd_size - central;
      gsize  length;
      gchar *name;

      if (left < CENTRAL_SIZE || get32 (central) != CENTRAL_SIGNATURE)
        {
          ok = damaged (error, "directory");
          break;
        }
      length = CENTRAL_SIZE + get16 (central + 28) + get16 (central + 30)
               + get16 (central + 32);
      if (length > left)
        {
          ok = damaged (error, "directory");
          break;
        }

      unpacked += get32 (central + 24);
      if (unpacked > MAX_UNPACKED)
        {
          ok = damaged (error, "size");
          break;
        }

      name = g_strndup ((const gchar *) central + CENTRAL_SIZE, get16 (central + 28));
      if (strlen (name) != get16 (central + 28))
        ok = damaged (error, "name");
      else
        ok = !g_cancellable_set_error_if_cancelled (cancellable, error)
             && extract_entry (&zip, central, name, dest, cancellable, error);
      g_free (name);
      central += length;
    }

  g_mapped_file_unref (mapped);

  return ok;
}



typedef struct
{
  gchar *zip;
  gchar *dest;
} UnzipJob;



static void
unzip_job_free (gpointer data)
{
  UnzipJob *job = data;

  g_free (job->zip);
  g_free (job->dest);
  g_free (job);
}



static void
unzip_thread (GTask        *task,
              gpointer      source,
              gpointer      data,
              GCancellable *cancellable)
{
  UnzipJob *job = data;
  GError   *error = NULL;

  if (vi_unzip (job->zip, job->dest, cancellable, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}



void
vi_unzip_async (const gchar         *zip,
                const gchar         *dest,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             data)
{
  GTask    *task = g_task_new (NULL, cancellable, callback, data);
  UnzipJob *job = g_new0 (UnzipJob, 1);

  job->zip = g_strdup (zip);
  job->dest = g_strdup (dest);
  g_task_set_task_data (task, job, unzip_job_free);
  g_task_run_in_thread (task, unzip_thread);
  g_object_unref (task);
}



gboolean
vi_unzip_finish (GAsyncResult *result,
                 GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
