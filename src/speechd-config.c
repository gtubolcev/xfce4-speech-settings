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

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "speechd-config.h"

/* Speech Dispatcher reads ~/.config/speech-dispatcher/speechd.conf when it
 * exists and the system file otherwise. Includes and module configurations
 * are still looked up in the system directory, so a copy of the system file
 * is a complete user configuration (this is also what spd-conf does). */

/* The file may change while the dialog is open (the user, spd-conf, another
 * program). The dialog therefore keeps its changes as a list of edits, and
 * speechd_config_save() applies them to the file as it is then, not to the
 * copy read at the start: lines written by others in between stay. */
typedef enum
{
  EDIT_SET,
  EDIT_UNSET,
  EDIT_DETECT_MODULES,
  EDIT_LANGUAGE_MODULE,
} EditKind;

typedef struct
{
  EditKind  kind;
  gchar    *key;
  gchar    *value;
  gboolean  quote;
} Edit;

struct _SpeechdConfig
{
  gchar     *path;
  GPtrArray *lines;
  gboolean   dirty;
  gchar     *base;      /* the contents the lines were read from */
  GPtrArray *edits;     /* Edit, since base */
  gboolean   replaying;
};



static void
edit_free (gpointer data)
{
  Edit *edit = data;

  g_free (edit->key);
  g_free (edit->value);
  g_free (edit);
}



static void
add_edit (SpeechdConfig *config,
          EditKind       kind,
          const gchar   *key,
          const gchar   *value,
          gboolean       quote)
{
  Edit *edit;

  if (config->replaying)
    return;

  edit = g_new0 (Edit, 1);
  edit->kind = kind;
  edit->key = g_strdup (key);
  edit->value = g_strdup (value);
  edit->quote = quote;
  g_ptr_array_add (config->edits, edit);
}



/* Returns the key of an option line: "Key value" or, when @commented is
 * set, "# Key value". The key is copied to @key, the rest of the line is
 * returned. NULL when the line is no such option. */
static const gchar *
parse_option_line (const gchar *line,
                   gboolean     commented,
                   gchar      **key)
{
  const gchar *p = line;
  const gchar *start;

  while (g_ascii_isspace (*p))
    p++;

  if (commented)
    {
      if (*p != '#')
        return NULL;
      while (*p == '#' || g_ascii_isspace (*p))
        p++;
    }

  if (!g_ascii_isalpha (*p))
    return NULL;

  start = p;
  while (g_ascii_isalnum (*p))
    p++;

  if (*p != '\0' && !g_ascii_isspace (*p))
    return NULL;

  *key = g_strndup (start, p - start);

  while (g_ascii_isspace (*p))
    p++;

  return p;
}



/* Parses one value: a quoted string or a bare word. Returns NULL when there
 * is none. With @end, the position after the value is stored there. */
static gchar *
parse_value (const gchar  *p,
             const gchar **end)
{
  const gchar *start;
  gchar       *value;

  while (g_ascii_isspace (*p))
    p++;

  if (*p == '"')
    {
      start = ++p;
      while (*p != '\0' && *p != '"')
        p++;
      if (*p != '"')
        return NULL;
      value = g_strndup (start, p - start);
      p++;
    }
  else
    {
      start = p;
      while (*p != '\0' && *p != '#' && !g_ascii_isspace (*p))
        p++;
      if (p == start)
        return NULL;
      value = g_strndup (start, p - start);
    }

  if (end != NULL)
    *end = p;

  return value;
}



/* Client sections (BeginClient … EndClient) set per-client values, they
 * must not be mistaken for the global defaults. */
static gboolean
update_client_section (const gchar *line,
                       gboolean     in_client)
{
  const gchar *rest;
  gchar       *key = NULL;

  rest = parse_option_line (line, FALSE, &key);
  if (rest != NULL)
    {
      if (g_ascii_strcasecmp (key, "BeginClient") == 0)
        in_client = TRUE;
      else if (g_ascii_strcasecmp (key, "EndClient") == 0)
        in_client = FALSE;
    }
  g_free (key);

  return in_client;
}



/* Index of the last active line setting @key (the one that wins), or -1. */
static gint
find_active (SpeechdConfig *config,
             const gchar   *key)
{
  gboolean in_client = FALSE;
  gint     found = -1;
  guint    i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      gchar       *k = NULL;

      in_client = update_client_section (line, in_client);
      if (in_client)
        continue;

      if (parse_option_line (line, FALSE, &k) != NULL
          && g_ascii_strcasecmp (k, key) == 0)
        found = i;
      g_free (k);
    }

  return found;
}



/* Index of the last commented-out example of @key ("# DefaultRate  0"),
 * where the new value goes, or -1. Prose comments are not matched. */
static gint
find_commented (SpeechdConfig *config,
                const gchar   *key)
{
  gint  found = -1;
  guint i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      const gchar *rest, *end;
      gchar       *k = NULL, *value;

      rest = parse_option_line (line, TRUE, &k);
      if (rest != NULL && g_ascii_strcasecmp (k, key) == 0)
        {
          value = parse_value (rest, &end);
          if (value != NULL)
            {
              while (g_ascii_isspace (*end))
                end++;
              if (*end == '\0')
                found = i;
            }
          g_free (value);
        }
      g_free (k);
    }

  return found;
}



static gchar *
format_line (const gchar *key,
             const gchar *value,
             gboolean     quote)
{
  if (quote)
    return g_strdup_printf ("%s \"%s\"", key, value);

  return g_strdup_printf ("%s %s", key, value);
}



static void
set_line (SpeechdConfig *config,
          const gchar   *key,
          const gchar   *value,
          gboolean       quote)
{
  gchar *line;
  gchar *current;
  gint   i;

  add_edit (config, EDIT_SET, key, value, quote);

  current = speechd_config_get (config, key);
  if (g_strcmp0 (current, value) == 0)
    {
      g_free (current);
      return;
    }
  g_free (current);

  line = format_line (key, value, quote);

  i = find_active (config, key);
  if (i >= 0)
    {
      g_free (g_ptr_array_index (config->lines, i));
      g_ptr_array_index (config->lines, i) = line;
    }
  else
    {
      i = find_commented (config, key);
      if (i >= 0)
        g_ptr_array_insert (config->lines, i + 1, line);
      else
        g_ptr_array_add (config->lines, line);
    }

  config->dirty = TRUE;
}



/* The contents Speech Dispatcher would read now: the user's file, or the
 * system file when there is none (the user file is only written once
 * something is changed). */
static gchar *
read_contents (const gchar *path,
               GError     **error)
{
  gchar *contents = NULL;

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      if (!g_file_get_contents (path, &contents, NULL, error))
        return NULL;
    }
  else
    {
      gchar *system_path;

      system_path = g_build_filename (SPEECHD_SYSCONFDIR, "speechd.conf", NULL);
      if (!g_file_get_contents (system_path, &contents, NULL, NULL))
        contents = g_strdup ("");
      g_free (system_path);
    }

  return contents;
}



static void
set_contents (SpeechdConfig *config,
              gchar         *contents)
{
  gchar **lines;
  guint   i, n;

  g_ptr_array_set_size (config->lines, 0);

  lines = g_strsplit (contents, "\n", -1);
  n = g_strv_length (lines);

  /* a final newline gives an empty last element, which is not a line */
  if (n > 0 && *lines[n - 1] == '\0')
    n--;

  for (i = 0; i < n; i++)
    g_ptr_array_add (config->lines, g_strdup (lines[i]));

  g_strfreev (lines);
  g_free (config->base);
  config->base = contents;
}



SpeechdConfig *
speechd_config_load (GError **error)
{
  SpeechdConfig *config;
  gchar         *contents;

  config = g_new0 (SpeechdConfig, 1);
  config->path = g_build_filename (g_get_user_config_dir (),
                                   "speech-dispatcher", "speechd.conf", NULL);
  config->lines = g_ptr_array_new_with_free_func (g_free);
  config->edits = g_ptr_array_new_with_free_func (edit_free);

  contents = read_contents (config->path, error);
  if (contents == NULL)
    {
      speechd_config_free (config);
      return NULL;
    }
  set_contents (config, contents);

  return config;
}



void
speechd_config_free (SpeechdConfig *config)
{
  if (config == NULL)
    return;

  g_ptr_array_unref (config->lines);
  g_ptr_array_unref (config->edits);
  g_free (config->base);
  g_free (config->path);
  g_free (config);
}



const gchar *
speechd_config_get_path (SpeechdConfig *config)
{
  return config->path;
}



gboolean
speechd_config_is_dirty (SpeechdConfig *config)
{
  return config->dirty;
}



/* The value of @key without quotes, or NULL when it is not set. */
gchar *
speechd_config_get (SpeechdConfig *config,
                    const gchar   *key)
{
  const gchar *rest;
  gchar       *k = NULL;
  gchar       *value;
  gint         i;

  i = find_active (config, key);
  if (i < 0)
    return NULL;

  rest = parse_option_line (g_ptr_array_index (config->lines, i), FALSE, &k);
  g_free (k);
  value = parse_value (rest, NULL);

  return value;
}



gint
speechd_config_get_int (SpeechdConfig *config,
                        const gchar   *key,
                        gint           fallback)
{
  gchar  *value;
  gchar  *end;
  gint64  number;

  value = speechd_config_get (config, key);
  if (value == NULL)
    return fallback;

  number = g_ascii_strtoll (value, &end, 10);
  if (*end != '\0' || end == value)
    number = fallback;
  g_free (value);

  return CLAMP (number, G_MININT, G_MAXINT);
}



void
speechd_config_set (SpeechdConfig *config,
                    const gchar   *key,
                    const gchar   *value)
{
  g_return_if_fail (value != NULL);
  g_return_if_fail (strchr (value, '"') == NULL);

  set_line (config, key, value, TRUE);
}



void
speechd_config_set_int (SpeechdConfig *config,
                        const gchar   *key,
                        gint           value)
{
  gchar text[16];

  g_snprintf (text, sizeof (text), "%d", value);
  set_line (config, key, text, FALSE);
}



/* Comments out every active line of @key, so the built-in default of
 * Speech Dispatcher applies again. A line that follows a commented example
 * of @key, where speechd_config_set() puts new lines, is removed instead:
 * otherwise every set and unset would leave one more comment behind. */
void
speechd_config_unset (SpeechdConfig *config,
                      const gchar   *key)
{
  gint i;

  add_edit (config, EDIT_UNSET, key, NULL, FALSE);

  while ((i = find_active (config, key)) >= 0)
    {
      gchar *line = g_ptr_array_index (config->lines, i);

      if (i > 0 && find_commented (config, key) == i - 1)
        g_ptr_array_remove_index (config->lines, i);
      else
        {
          g_ptr_array_index (config->lines, i) = g_strconcat ("# ", line, NULL);
          g_free (line);
        }
      config->dirty = TRUE;
    }
}



static void
replay_edits (SpeechdConfig *config)
{
  guint i;

  config->replaying = TRUE;
  for (i = 0; i < config->edits->len; i++)
    {
      Edit *edit = g_ptr_array_index (config->edits, i);

      switch (edit->kind)
        {
        case EDIT_SET:
          set_line (config, edit->key, edit->value, edit->quote);
          break;
        case EDIT_UNSET:
          speechd_config_unset (config, edit->key);
          break;
        case EDIT_LANGUAGE_MODULE:
          speechd_config_set_language_module (config, edit->key, edit->value);
          break;
        case EDIT_DETECT_MODULES:
          /* it worked on the old contents; if not on these, keep them */
          speechd_config_use_detected_modules (config, NULL);
          break;
        }
    }
  config->replaying = FALSE;
  /* the user's edits are still to be written, even where the other
   * writer made the same change */
  config->dirty = TRUE;
}



gboolean
speechd_config_save (SpeechdConfig *config,
                     GError       **error)
{
  GString *contents;
  gchar   *dirname;
  gchar   *current;
  guint    i;
  gboolean ok;

  if (!config->dirty)
    return TRUE;

  /* changed by someone else since it was read: apply our edits to that */
  current = read_contents (config->path, error);
  if (current == NULL)
    return FALSE;
  if (strcmp (current, config->base) != 0)
    {
      set_contents (config, current);
      replay_edits (config);
    }
  else
    g_free (current);

  dirname = g_path_get_dirname (config->path);
  if (g_mkdir_with_parents (dirname, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", dirname, g_strerror (errsv));
      g_free (dirname);
      return FALSE;
    }
  g_free (dirname);

  contents = g_string_new (NULL);
  for (i = 0; i < config->lines->len; i++)
    {
      g_string_append (contents, g_ptr_array_index (config->lines, i));
      g_string_append_c (contents, '\n');
    }

  ok = g_file_set_contents (config->path, contents->str, contents->len, error);

  if (ok)
    {
      config->dirty = FALSE;
      g_free (config->base);
      config->base = g_string_free (contents, FALSE);
      g_ptr_array_set_size (config->edits, 0);
    }
  else
    g_string_free (contents, TRUE);

  return ok;
}



/* All values of an option line ("AddModule "a" "b" "c"") from @rest on. */
static gchar **
parse_values (const gchar *rest)
{
  GPtrArray *values = g_ptr_array_new ();
  gchar     *value;

  while ((value = parse_value (rest, &rest)) != NULL)
    g_ptr_array_add (values, value);
  g_ptr_array_add (values, NULL);

  return (gchar **) g_ptr_array_free (values, FALSE);
}



/* Calls @func for every active option line outside the client sections
 * with its index, key and values. */
typedef void (*LineFunc) (SpeechdConfig *config,
                          guint          index,
                          const gchar   *key,
                          gchar        **values,
                          gpointer       data);

static void
foreach_option (SpeechdConfig *config,
                gboolean       with_clients,
                LineFunc       func,
                gpointer       data)
{
  gboolean in_client = FALSE;
  guint    i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      const gchar *rest;
      gchar       *key = NULL;
      gchar      **values;

      in_client = update_client_section (line, in_client);
      rest = parse_option_line (line, FALSE, &key);
      if (rest != NULL && (with_clients || !in_client))
        {
          values = parse_values (rest);
          func (config, i, key, values, data);
          g_strfreev (values);
        }
      g_free (key);
    }
}



typedef struct
{
  const gchar *language;
  gint         index;      /* the last line for the language */
  gint         last_any;   /* the last LanguageDefaultModule line */
  gint         default_module;
  gchar       *module;
} LanguageModuleData;

static void
find_language_module (SpeechdConfig *config,
                      guint          index,
                      const gchar   *key,
                      gchar        **values,
                      gpointer       user_data)
{
  LanguageModuleData *data = user_data;

  if (g_ascii_strcasecmp (key, "DefaultModule") == 0)
    data->default_module = index;
  if (g_ascii_strcasecmp (key, "LanguageDefaultModule") != 0
      || g_strv_length (values) < 2)
    return;

  data->last_any = index;
  if (g_ascii_strcasecmp (values[0], data->language) == 0)
    {
      data->index = index;
      g_free (data->module);
      data->module = g_strdup (values[1]);
    }
}



/* The module of a LanguageDefaultModule line for @language, or NULL */
gchar *
speechd_config_get_language_module (SpeechdConfig *config,
                                    const gchar   *language)
{
  LanguageModuleData data = { language, -1, -1, -1, NULL };

  g_return_val_if_fail (language != NULL, NULL);

  foreach_option (config, FALSE, find_language_module, &data);

  return data.module;
}



static void
add_used_module (SpeechdConfig *config,
                 guint          index,
                 const gchar   *key,
                 gchar        **values,
                 gpointer       user_data)
{
  GPtrArray   *used = user_data;
  const gchar *module = NULL;
  guint        i;

  if (g_ascii_strcasecmp (key, "DefaultModule") == 0 && values[0] != NULL)
    module = values[0];
  else if (g_ascii_strcasecmp (key, "LanguageDefaultModule") == 0
           && g_strv_length (values) >= 2)
    module = values[1];
  if (module == NULL || *module == '\0')
    return;
  for (i = 0; i < used->len; i++)
    if (strcmp (g_ptr_array_index (used, i), module) == 0)
      return;
  g_ptr_array_add (used, g_strdup (module));
}

/* The modules the configuration speaks with: DefaultModule and those of
 * the LanguageDefaultModule lines (not the client sections). Free with
 * g_ptr_array_unref. */
GPtrArray *
speechd_config_used_modules (SpeechdConfig *config)
{
  GPtrArray *used = g_ptr_array_new_with_free_func (g_free);

  foreach_option (config, FALSE, add_used_module, used);

  return used;
}



/* Sets the LanguageDefaultModule line of @language to @module, or comments
 * it out when @module is NULL. A new line goes after the other languages'
 * lines, else after DefaultModule. */
void
speechd_config_set_language_module (SpeechdConfig *config,
                                    const gchar   *language,
                                    const gchar   *module)
{
  LanguageModuleData data = { language, -1, -1, -1, NULL };
  gchar             *line;

  g_return_if_fail (language != NULL && strchr (language, '"') == NULL);
  g_return_if_fail (module == NULL || strchr (module, '"') == NULL);

  add_edit (config, EDIT_LANGUAGE_MODULE, language, module, TRUE);

  foreach_option (config, FALSE, find_language_module, &data);

  if (module == NULL)
    {
      /* every line of the language: an earlier one would apply again */
      while (data.index >= 0)
        {
          line = g_ptr_array_index (config->lines, data.index);
          g_ptr_array_index (config->lines, data.index) = g_strconcat ("# ", line, NULL);
          g_free (line);
          config->dirty = TRUE;

          g_clear_pointer (&data.module, g_free);
          data.index = data.last_any = data.default_module = -1;
          foreach_option (config, FALSE, find_language_module, &data);
        }
      g_free (data.module);
      return;
    }

  if (g_strcmp0 (data.module, module) == 0)
    {
      g_free (data.module);
      return;
    }
  g_free (data.module);

  line = g_strdup_printf ("LanguageDefaultModule \"%s\" \"%s\"", language, module);
  if (data.index >= 0)
    {
      g_free (g_ptr_array_index (config->lines, data.index));
      g_ptr_array_index (config->lines, data.index) = line;
    }
  else if (data.last_any >= 0)
    g_ptr_array_insert (config->lines, data.last_any + 1, line);
  else if (data.default_module >= 0)
    g_ptr_array_insert (config->lines, data.default_module + 1, line);
  else
    g_ptr_array_add (config->lines, line);

  config->dirty = TRUE;
}



static void
find_add_module (SpeechdConfig *config,
                 guint          index,
                 const gchar   *key,
                 gchar        **values,
                 gpointer       data)
{
  if (g_ascii_strcasecmp (key, "AddModule") == 0)
    *(gboolean *) data = TRUE;
}



/* Whether the configuration lists its output modules with AddModule. Then
 * Speech Dispatcher loads only those and does not look for the installed
 * ones. */
gboolean
speechd_config_has_module_list (SpeechdConfig *config)
{
  gboolean found = FALSE;

  foreach_option (config, FALSE, find_add_module, &found);

  return found;
}



typedef struct
{
  gchar      *user_dir;
  GPtrArray  *copies;  /* pairs of source and destination */
  GHashTable *renames; /* old module name → detected name */
  GError     *error;
} DetectData;

static void
plan_detection (SpeechdConfig *config,
                guint          index,
                const gchar   *key,
                gchar        **values,
                gpointer       user_data)
{
  DetectData *data = user_data;
  gchar      *conf, *binary, *name, *source, *dest;

  if (data->error != NULL || g_ascii_strcasecmp (key, "AddModule") != 0)
    return;

  if (g_strv_length (values) < 3)
    {
      g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s", (gchar *) g_ptr_array_index (config->lines, index));
      return;
    }

  conf = g_path_get_basename (values[2]);

  if (strcmp (values[1], "sd_generic") == 0)
    {
      /* generic modules are found as modules/NAME-generic.conf, and
       * then they are called NAME-generic */
      if (g_str_has_suffix (conf, "-generic.conf"))
        name = g_strndup (conf, strlen (conf) - strlen (".conf"));
      else
        name = g_strconcat (values[0], "-generic", NULL);

      dest = g_strconcat (data->user_dir, G_DIR_SEPARATOR_S, name, ".conf",
                          NULL);
      source = g_build_filename (data->user_dir, conf, NULL);
      if (!g_file_test (source, G_FILE_TEST_IS_REGULAR))
        {
          g_free (source);
          source = g_build_filename (SPEECHD_SYSCONFDIR, "modules", conf, NULL);
        }

      if (strcmp (source, dest) != 0 && !g_file_test (dest, G_FILE_TEST_EXISTS))
        {
          if (!g_file_test (source, G_FILE_TEST_IS_REGULAR))
            g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "%s: %s", source, g_strerror (ENOENT));
          g_ptr_array_add (data->copies, source);
          g_ptr_array_add (data->copies, dest);
        }
      else
        {
          g_free (source);
          g_free (dest);
        }

      if (strcmp (values[0], name) != 0)
        g_hash_table_insert (data->renames, g_strdup (values[0]), name);
      else
        g_free (name);
    }
  else
    {
      /* a module binary is found as sd_NAME with NAME.conf */
      binary = g_strconcat ("sd_", values[0], NULL);
      name = g_strconcat (values[0], ".conf", NULL);
      if (strcmp (values[1], binary) != 0 || strcmp (conf, name) != 0)
        g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "“%s” (%s, %s)", values[0], values[1], values[2]);
      g_free (binary);
      g_free (name);
    }

  g_free (conf);
}



static void
rename_modules (SpeechdConfig *config,
                guint          index,
                const gchar   *key,
                gchar        **values,
                gpointer       user_data)
{
  DetectData  *data = user_data;
  const gchar *line = g_ptr_array_index (config->lines, index);
  const gchar *new_name;
  gchar       *indent;
  guint        n;

  if (g_ascii_strcasecmp (key, "DefaultModule") == 0)
    n = 0;
  else if (g_ascii_strcasecmp (key, "LanguageDefaultModule") == 0)
    n = 1;
  else
    return;

  if (g_strv_length (values) <= n)
    return;

  new_name = g_hash_table_lookup (data->renames, values[n]);
  if (new_name == NULL)
    return;

  indent = g_strndup (line, strspn (line, " \t"));
  g_free (g_ptr_array_index (config->lines, index));
  if (n == 0)
    g_ptr_array_index (config->lines, index) =
      g_strdup_printf ("%s%s \"%s\"", indent, key, new_name);
  else
    g_ptr_array_index (config->lines, index) =
      g_strdup_printf ("%s%s \"%s\" \"%s\"", indent, key, values[0], new_name);
  g_free (indent);

  config->dirty = TRUE;
}



/* Comments out the AddModule lines, so Speech Dispatcher loads every
 * installed module again. A generic module that would not be found under
 * its name is copied to the user's modules/NAME-generic.conf, and the
 * DefaultModule settings follow its new name. Nothing is changed when a
 * module cannot be detected this way. The configuration still has to be
 * saved. */
gboolean
speechd_config_use_detected_modules (SpeechdConfig *config,
                                     GError       **error)
{
  DetectData data = { NULL, };
  gchar     *contents;
  gsize      length;
  guint      i;
  gboolean   ok = FALSE;

  data.user_dir = g_build_filename (g_get_user_config_dir (),
                                    "speech-dispatcher", "modules", NULL);
  data.copies = g_ptr_array_new_with_free_func (g_free);
  data.renames = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_free);

  foreach_option (config, FALSE, plan_detection, &data);
  if (data.error != NULL)
    {
      g_propagate_error (error, data.error);
      goto out;
    }

  if (data.copies->len > 0 && g_mkdir_with_parents (data.user_dir, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", data.user_dir, g_strerror (errsv));
      goto out;
    }

  for (i = 0; i < data.copies->len; i += 2)
    {
      if (!g_file_get_contents (g_ptr_array_index (data.copies, i),
                                &contents, &length, error))
        goto out;
      if (!g_file_set_contents (g_ptr_array_index (data.copies, i + 1),
                                contents, length, error))
        {
          g_free (contents);
          goto out;
        }
      g_free (contents);
    }

  {
    /* one edit for the whole step, not its parts */
    gboolean replaying = config->replaying;

    config->replaying = TRUE;
    speechd_config_unset (config, "AddModule");
    foreach_option (config, TRUE, rename_modules, &data);
    config->replaying = replaying;
  }
  add_edit (config, EDIT_DETECT_MODULES, NULL, NULL, FALSE);
  ok = TRUE;

out:
  g_free (data.user_dir);
  g_ptr_array_unref (data.copies);
  g_hash_table_unref (data.renames);

  return ok;
}



/* Makes the running server read its configuration again. SIGHUP resets all
 * defaults to the built-in values before the file is read, and restarts the
 * output modules. Not an error when no server is running: the next one
 * reads the new file anyway. */
gboolean
speechd_server_reload (GError **error)
{
  gchar  *pid_file;
  gchar  *contents = NULL;
  gchar  *comm_file;
  gchar  *comm = NULL;
  gchar  *end;
  gint64  pid;

  pid_file = g_build_filename (g_get_user_runtime_dir (), "speech-dispatcher",
                               "pid", "speech-dispatcher.pid", NULL);
  if (!g_file_get_contents (pid_file, &contents, NULL, NULL))
    {
      g_free (pid_file);
      return TRUE;
    }
  g_free (pid_file);

  pid = g_ascii_strtoll (g_strstrip (contents), &end, 10);
  if (*end != '\0' || pid <= 1)
    {
      g_free (contents);
      return TRUE;
    }
  g_free (contents);

  /* the pid file can be stale; only signal Speech Dispatcher itself */
  comm_file = g_strdup_printf ("/proc/%" G_GINT64_FORMAT "/comm", pid);
  if (!g_file_get_contents (comm_file, &comm, NULL, NULL)
      || !g_str_has_prefix (comm, "speech-dispatch"))
    {
      g_free (comm_file);
      g_free (comm);
      return TRUE;
    }
  g_free (comm_file);
  g_free (comm);

  if (kill ((pid_t) pid, SIGHUP) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "kill (%" G_GINT64_FORMAT ", SIGHUP): %s", pid,
                   g_strerror (errsv));
      return FALSE;
    }

  return TRUE;
}



/* ---- Per-language voices in the output module configurations ---- */

/* Speech Dispatcher has no per-language voice. Modules choose it in their
 * own configuration: the generic modules map the voice types of a language
 * to voices with AddVoice lines (a later line for the same language and type
 * replaces an earlier one), our modules read "<Module>Voice "lang" "voice"". */

/* the fallback order of module_getvoice() for a type without a voice */
static const gchar *const voice_type_order[] =
{
  "MALE1", "FEMALE1", "MALE2", "FEMALE2", "MALE3", "FEMALE3",
  "CHILD_MALE", "CHILD_FEMALE",
};

static gchar *
module_config_path (const gchar *module,
                    gboolean     user)
{
  gchar *name = g_strconcat (module, ".conf", NULL);
  gchar *path;

  if (user)
    path = g_build_filename (g_get_user_config_dir (), "speech-dispatcher",
                             "modules", name, NULL);
  else
    path = g_build_filename (SPEECHD_SYSCONFDIR, "modules", name, NULL);
  g_free (name);

  return path;
}

/* The lines of the user's (@user) or the system copy of the module
 * configuration, NULL when there is none */
static gchar **
module_config_read_copy (const gchar *module,
                         gboolean     user)
{
  gchar *contents = NULL;
  gchar *path;
  gchar **lines;
  guint  n;

  if (module == NULL || *module == '\0' || strchr (module, '/') != NULL)
    return NULL;

  path = module_config_path (module, user);
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    contents = NULL;
  g_free (path);

  if (contents == NULL)
    return NULL;

  lines = g_strsplit (contents, "\n", -1);
  g_free (contents);
  n = g_strv_length (lines);
  if (n > 0 && *lines[n - 1] == '\0')
    {
      g_free (lines[n - 1]);
      lines[n - 1] = NULL;
    }

  return lines;
}

/* The lines of the module configuration that Speech Dispatcher reads: the
 * user's copy, else the system one. NULL when there is none. */
static gchar **
module_config_read (const gchar *module)
{
  gchar **lines = module_config_read_copy (module, TRUE);

  return lines != NULL ? lines : module_config_read_copy (module, FALSE);
}

/* Writes @lines as the user's copy of the module configuration */
static gboolean
module_config_write (const gchar *module,
                     GPtrArray   *lines,
                     GError     **error)
{
  GString *contents = g_string_new (NULL);
  gchar   *path, *dir;
  gboolean ok;
  guint    i;

  for (i = 0; i < lines->len; i++)
    {
      g_string_append (contents, g_ptr_array_index (lines, i));
      g_string_append_c (contents, '\n');
    }

  path = module_config_path (module, TRUE);
  dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", dir, g_strerror (errsv));
      ok = FALSE;
    }
  else
    ok = g_file_set_contents (path, contents->str, contents->len, error);

  g_free (dir);
  g_free (path);
  g_string_free (contents, TRUE);

  return ok;
}



/* The values of line @line when its key is @key, else NULL */
static gchar **
module_line_values (const gchar *line,
                    const gchar *key)
{
  const gchar *rest;
  gchar       *k = NULL;
  gchar      **values = NULL;

  rest = parse_option_line (line, FALSE, &k);
  if (rest != NULL && g_ascii_strcasecmp (k, key) == 0)
    values = parse_values (rest);
  g_free (k);

  return values;
}

/* our modules, which read "<Module>Voice" ("zonos3" is built by hand) */
static const gchar *const own_modules[] = { "zonos3", "sherpa", "piper", "vosk", "kokoro", "teratts", NULL };

/* Synthesizer names of our modules and the option prefix each reads:
 * sd_sherpa-onnx is installed under two names, with a configuration each;
 * the piper-sherpa-onnx names are those of speech-dispatcher-modules before
 * Piper joined the synthesizer sherpa-onnx (2026-11-16) */
static const gchar *const module_variants[][2] = {
  { "sherpa-onnx", "sherpa" },
  { "sherpa-onnx-vulkan-rs", "sherpa" },
  { "piper-sherpa-onnx", "piper" },
  { "piper-sherpa-onnx-vulkan-rs", "piper" },
};

/* The option prefix of @module: "sherpa" for "sherpa-onnx-vulkan-rs",
 * "piper" for the former "piper-sherpa-onnx"; @module itself for the others */
const gchar *
speechd_module_base (const gchar *module)
{
  guint i;

  for (i = 0; module != NULL && i < G_N_ELEMENTS (module_variants); i++)
    if (strcmp (module, module_variants[i][0]) == 0)
      return module_variants[i][1];

  return module;
}

/* "<Module><suffix>", the name of an option of @module: "SherpaVoice" for
 * "sherpa-onnx" and "sherpa-onnx-vulkan-rs" */
gchar *
speechd_module_option_key (const gchar *module,
                           const gchar *suffix)
{
  gchar *key = g_strconcat (speechd_module_base (module), suffix, NULL);

  key[0] = g_ascii_toupper (key[0]);

  return key;
}

static gchar *
module_voice_key (const gchar *module)
{
  return speechd_module_option_key (module, "Voice");
}

SpeechdVoiceSetting
speechd_module_voice_setting (const gchar *module)
{
  gchar             **lines;
  SpeechdVoiceSetting setting = SPEECHD_VOICE_SETTING_NONE;
  guint               i;

  if (module != NULL && g_strv_contains (own_modules, speechd_module_base (module)))
    return SPEECHD_VOICE_SETTING_MODULE;

  lines = module_config_read (module);
  if (lines == NULL)
    return setting;

  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *rest;
      gchar       *key = NULL;

      /* commented-out examples tell as much as active lines */
      rest = parse_option_line (lines[i], FALSE, &key);
      if (rest == NULL)
        rest = parse_option_line (lines[i], TRUE, &key);
      if (rest == NULL)
        continue;
      if (g_ascii_strcasecmp (key, "GenericExecuteSynth") == 0)
        setting = SPEECHD_VOICE_SETTING_ADD_VOICE;
      else if (setting == SPEECHD_VOICE_SETTING_NONE)
        {
          gchar *voice_key = module_voice_key (module);

          if (g_ascii_strcasecmp (key, voice_key) == 0)
            setting = SPEECHD_VOICE_SETTING_MODULE;
          g_free (voice_key);
        }
      g_free (key);
    }
  g_strfreev (lines);

  return setting;
}

/* The voice the module speaks @language with for voice type @type (as in
 * DefaultVoiceType), or NULL when its configuration does not say. */
gchar *
speechd_module_get_voice (const gchar *module,
                          const gchar *language,
                          const gchar *type)
{
  SpeechdVoiceSetting setting = speechd_module_voice_setting (module);
  gchar             **lines;
  gchar              *voice_key;
  gchar              *found = NULL;
  gchar              *by_type[G_N_ELEMENTS (voice_type_order)] = { NULL, };
  guint               i, t;

  if (setting == SPEECHD_VOICE_SETTING_NONE || language == NULL)
    return NULL;

  lines = module_config_read (module);
  voice_key = module_voice_key (module);
  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar **values;

      if (setting == SPEECHD_VOICE_SETTING_MODULE)
        {
          values = module_line_values (lines[i], voice_key);
          if (values != NULL && g_strv_length (values) >= 2
              && g_ascii_strcasecmp (values[0], language) == 0)
            {
              g_free (found);
              found = g_strdup (values[1]);
            }
        }
      else
        {
          values = module_line_values (lines[i], "AddVoice");
          if (values != NULL && g_strv_length (values) >= 3
              && g_ascii_strcasecmp (values[0], language) == 0)
            for (t = 0; t < G_N_ELEMENTS (voice_type_order); t++)
              if (g_ascii_strcasecmp (values[1], voice_type_order[t]) == 0)
                {
                  g_free (by_type[t]);
                  by_type[t] = g_strdup (values[2]);
                }
        }
      g_strfreev (values);
    }
  g_free (voice_key);
  g_strfreev (lines);

  if (setting == SPEECHD_VOICE_SETTING_ADD_VOICE)
    {
      for (t = 0; type != NULL && t < G_N_ELEMENTS (voice_type_order); t++)
        if (g_ascii_strcasecmp (type, voice_type_order[t]) == 0 && by_type[t])
          found = g_strdup (by_type[t]);
      for (t = 0; found == NULL && t < G_N_ELEMENTS (voice_type_order); t++)
        if (by_type[t] != NULL)
          found = g_strdup (by_type[t]);
      for (t = 0; t < G_N_ELEMENTS (voice_type_order); t++)
        g_free (by_type[t]);
    }

  return found;
}

/* Makes @voice the voice of @language in the module configuration, writing
 * the user's copy (made from the system file when there is none yet).
 * Generic modules: the AddVoice line of the voice gets type @type and moves
 * behind the other lines of the language, so it wins. Our modules: one
 * "<Module>Voice "lang" "voice"" line per language. The server has to be
 * reloaded afterwards. */
gboolean
speechd_module_set_voice (const gchar *module,
                          const gchar *language,
                          const gchar *type,
                          const gchar *voice,
                          GError     **error)
{
  SpeechdVoiceSetting setting = speechd_module_voice_setting (module);
  gchar             **lines;
  GPtrArray          *out;
  gchar              *voice_key;
  gchar              *new_line;
  gint                last = -1;
  guint               i;
  gboolean            ok;

  if (setting == SPEECHD_VOICE_SETTING_NONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "%s: no voice setting", module);
      return FALSE;
    }
  if (strchr (language, '"') != NULL || strchr (voice, '"') != NULL
      || (type != NULL && strchr (type, '"') != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s: invalid voice", voice);
      return FALSE;
    }

  voice_key = module_voice_key (module);
  if (setting == SPEECHD_VOICE_SETTING_MODULE)
    new_line = g_strdup_printf ("%s \"%s\" \"%s\"", voice_key, language, voice);
  else
    new_line = g_strdup_printf ("AddVoice \"%s\" \"%s\" \"%s\"", language,
                                type != NULL ? type : "MALE1", voice);

  /* drop the old line, remember where the language's lines end */
  lines = module_config_read (module);
  out = g_ptr_array_new ();
  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar  **values;
      gboolean drop = FALSE;

      if (setting == SPEECHD_VOICE_SETTING_MODULE)
        {
          values = module_line_values (lines[i], voice_key);
          if (values != NULL && g_strv_length (values) >= 2
              && g_ascii_strcasecmp (values[0], language) == 0)
            drop = TRUE;
        }
      else
        {
          values = module_line_values (lines[i], "AddVoice");
          if (values != NULL && g_strv_length (values) >= 3
              && g_ascii_strcasecmp (values[0], language) == 0)
            {
              drop = strcmp (values[2], voice) == 0;
              if (!drop)
                last = out->len;
            }
        }
      g_strfreev (values);

      if (!drop)
        g_ptr_array_add (out, lines[i]);
      else if (last < 0 && setting == SPEECHD_VOICE_SETTING_MODULE)
        last = (gint) out->len - 1;
    }

  if (last < 0)
    g_ptr_array_add (out, new_line);
  else
    g_ptr_array_insert (out, last + 1, new_line);

  ok = module_config_write (module, out, error);
  g_ptr_array_free (out, TRUE);
  g_strfreev (lines);
  g_free (new_line);
  g_free (voice_key);

  return ok;
}



static gboolean
lines_have_option (gchar      **lines,
                   const gchar *key)
{
  gboolean found = FALSE;
  guint    i;

  for (i = 0; lines != NULL && lines[i] != NULL && !found; i++)
    {
      gchar *k = NULL;

      if (parse_option_line (lines[i], FALSE, &k) != NULL
          || parse_option_line (lines[i], TRUE, &k) != NULL)
        found = g_ascii_strcasecmp (k, key) == 0;
      g_free (k);
    }

  return found;
}

/* Whether the module configuration knows option @key, also as a
 * commented-out example; the system copy counts too, as a user's copy
 * made before the option was added does not mention it */
gboolean
speechd_module_has_option (const gchar *module,
                           const gchar *key)
{
  gchar  **lines = module_config_read_copy (module, TRUE);
  gboolean found = lines_have_option (lines, key);

  g_strfreev (lines);
  if (!found)
    {
      lines = module_config_read_copy (module, FALSE);
      found = lines_have_option (lines, key);
      g_strfreev (lines);
    }

  return found;
}



/* The value of the last active line of option @key in the module
 * configuration, or NULL */
gchar *
speechd_module_get_option (const gchar *module,
                           const gchar *key)
{
  gchar **lines = module_config_read (module);
  gchar  *value = NULL;
  guint   i;

  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar **values = module_line_values (lines[i], key);

      if (values != NULL && values[0] != NULL)
        {
          g_free (value);
          value = g_strdup (values[0]);
        }
      g_strfreev (values);
    }
  g_strfreev (lines);

  return value;
}



/* Sets option @key of the module to @value (quoted unless a number) in the user's copy of
 * its configuration; NULL removes the active lines. The new line replaces
 * the first active one, else follows the commented-out example, else ends
 * the file. The server has to be reloaded afterwards. */
gboolean
speechd_module_set_option (const gchar *module,
                           const gchar *key,
                           const gchar *value,
                           GError     **error)
{
  gchar    **lines;
  GPtrArray *out;
  gchar     *new_line = NULL;
  gint       at = -1, example = -1;
  guint      i;
  gboolean   changed = FALSE;
  gboolean   kept = FALSE;  /* the line at @at is already new_line */
  gboolean   ok = TRUE;

  if (module == NULL || *module == '\0' || strchr (module, '/') != NULL
      || (value != NULL && (strchr (value, '"') != NULL
                            || strchr (value, '\n') != NULL)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s: invalid value", key);
      return FALSE;
    }
  if (value != NULL && *value != '\0'
      && value[strspn (value, "0123456789")] == '\0')
    new_line = g_strdup_printf ("%s %s", key, value);
  else if (value != NULL)
    new_line = g_strdup_printf ("%s \"%s\"", key, value);

  lines = module_config_read (module);
  out = g_ptr_array_new ();
  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar *k = NULL;

      if (parse_option_line (lines[i], FALSE, &k) != NULL
          && g_ascii_strcasecmp (k, key) == 0)
        {
          /* an identical line stays, so an unchanged file is not written */
          if (at < 0 && new_line != NULL && strcmp (lines[i], new_line) == 0)
            {
              at = out->len;
              kept = TRUE;
              g_ptr_array_add (out, lines[i]);
            }
          else
            {
              if (at < 0)
                at = out->len;
              changed = TRUE;
            }
        }
      else
        {
          gchar *c = NULL;

          if (parse_option_line (lines[i], TRUE, &c) != NULL
              && g_ascii_strcasecmp (c, key) == 0)
            example = out->len;
          g_free (c);
          g_ptr_array_add (out, lines[i]);
        }
      g_free (k);
    }

  /* @at is where the first active line was: it was dropped unless kept, so
   * it may be out->len (the option ended the file) */
  if (new_line != NULL && !kept)
    {
      if (at < 0)
        at = example >= 0 ? example + 1 : (gint) out->len;
      g_ptr_array_insert (out, at, new_line);
      changed = TRUE;
    }

  if (changed)
    ok = module_config_write (module, out, error);

  g_ptr_array_free (out, TRUE);
  g_strfreev (lines);
  g_free (new_line);

  return ok;
}
