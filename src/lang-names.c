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
 * Names of languages and countries in the language of the user, from
 * iso-codes: its JSON files map the codes to the English names, and its
 * gettext domains translate those. Without iso-codes the English names of
 * the voice catalogues (or the codes) are shown.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include "lang-names.h"

#define ISO_JSON_DIR ISO_CODES_PREFIX "/share/iso-codes/json"



/* code -> English name of one iso-codes table; loaded once, never freed */
static GHashTable *
lang_names_table (const gchar *file,
                  const gchar *array,
                  const gchar *key2,
                  const gchar *key3)
{
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  JsonParser *parser = json_parser_new ();
  gchar      *path = g_build_filename (ISO_JSON_DIR, file, NULL);
  JsonNode   *root;
  JsonArray  *entries = NULL;
  guint       i;

  if (json_parser_load_from_file (parser, path, NULL)
      && (root = json_parser_get_root (parser)) != NULL
      && JSON_NODE_HOLDS_OBJECT (root)
      && json_object_has_member (json_node_get_object (root), array))
    {
      JsonNode *node = json_object_get_member (json_node_get_object (root), array);

      if (JSON_NODE_HOLDS_ARRAY (node))
        entries = json_node_get_array (node);
    }
  for (i = 0; entries != NULL && i < json_array_get_length (entries); i++)
    {
      JsonNode    *node = json_array_get_element (entries, i);
      JsonObject  *entry;
      const gchar *name, *keys[2] = { key2, key3 };
      guint        k;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        continue;
      entry = json_node_get_object (node);
      /* "Taiwan" rather than "Taiwan, Province of China" */
      name = json_object_get_string_member_with_default (entry, "common_name", NULL);
      if (name == NULL)
        name = json_object_get_string_member_with_default (entry, "name", NULL);
      if (name == NULL)
        continue;
      for (k = 0; k < G_N_ELEMENTS (keys); k++)
        {
          const gchar *code = keys[k] != NULL
                              ? json_object_get_string_member_with_default (entry, keys[k], NULL)
                              : NULL;

          if (code != NULL && !g_hash_table_contains (table, code))
            g_hash_table_insert (table, g_ascii_strdown (code, -1), g_strdup (name));
        }
    }
  g_free (path);
  g_object_unref (parser);
  return table;
}



static gchar *
lang_names_lookup (GHashTable  *table,
                   const gchar *domain,
                   const gchar *code,
                   gsize        length,
                   const gchar *fallback)
{
  gchar       *key;
  const gchar *name;

  if (code == NULL || length == 0 || length > 3)
    return g_strdup (fallback != NULL ? fallback : code);
  key = g_ascii_strdown (code, length);
  name = g_hash_table_lookup (table, key);
  g_free (key);
  if (name == NULL)
    return g_strdup (fallback != NULL ? fallback : code);
  return g_strdup (g_dgettext (domain, name));
}



/* "Neugriechisch (ab 1453)" -> "Neugriechisch": the region follows in
 * parentheses, two pairs would be hard to read */
static gchar *
lang_names_strip (gchar *name)
{
  gchar *open = strstr (name, " (");

  if (open != NULL && open != name && g_str_has_suffix (name, ")"))
    *open = '\0';
  return name;
}



/* "de", "de_DE", "en-us" -> "Deutsch"; fallback (or code) if unknown */
gchar *
lang_names_language (const gchar *code,
                     const gchar *fallback)
{
  static GHashTable *table = NULL;

  if (table == NULL)
    table = lang_names_table ("iso_639-3.json", "639-3", "alpha_2", "alpha_3");
  return lang_names_strip (lang_names_lookup (table, "iso_639-3", code,
                                              code != NULL ? strcspn (code, "-_") : 0,
                                              fallback));
}



/* "DE" -> "Deutschland" */
gchar *
lang_names_region (const gchar *code,
                   const gchar *fallback)
{
  static GHashTable *table = NULL;

  if (table == NULL)
    table = lang_names_table ("iso_3166-1.json", "3166-1", "alpha_2", "alpha_3");
  return lang_names_lookup (table, "iso_3166-1", code,
                            code != NULL ? strlen (code) : 0, fallback);
}



/* "de_DE" or ("de", "DE") -> "Deutsch (Deutschland)"; the region from the
 * code when region is NULL; fallback when the language is unknown */
gchar *
lang_names_label (const gchar *code,
                  const gchar *region,
                  const gchar *fallback)
{
  gchar *language, *place, *label;

  if (code == NULL)
    return g_strdup (fallback);
  language = lang_names_language (code, NULL);
  if (g_strcmp0 (language, code) == 0 && fallback != NULL)
    {
      g_free (language);
      return g_strdup (fallback);
    }
  if (region == NULL && code[strcspn (code, "-_")] != '\0')
    region = code + strcspn (code, "-_") + 1;
  if (region == NULL || *region == '\0')
    return language;
  place = lang_names_region (region, region);
  label = g_strdup_printf ("%s (%s)", language, place);
  g_free (language);
  g_free (place);
  return label;
}
