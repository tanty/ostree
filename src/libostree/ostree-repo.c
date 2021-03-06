/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-file-enumerator.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

struct OstreeRepo {
  GObject parent;

  GFile *repodir;
  GFile *tmp_dir;
  GFile *pending_dir;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  GFile *objects_dir;
  GFile *pack_dir;
  GFile *remote_cache_dir;
  GFile *config_file;

#if GLIB_CHECK_VERSION(2,32,0) && !defined(OSTREE_GLIB_TARGET_MIN)
  GMutex cache_lock;
#else
  GMutex *cache_lock;
#endif
  GPtrArray *cached_meta_indexes;
  GPtrArray *cached_content_indexes;
  GHashTable *cached_pack_index_mappings;
  GHashTable *cached_pack_data_mappings;

  gboolean inited;
  gboolean in_transaction;
  GHashTable *loose_object_devino_hash;

  GKeyFile *config;
  OstreeRepoMode mode;

  OstreeRepo *parent_repo;
};

typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  gboolean              lookup_all,
                  GFile               **out_stored_path,
                  char                **out_pack_checksum,
                  guint64              *out_pack_offset,
                  GCancellable         *cancellable,
                  GError             **error);

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);

  g_clear_object (&self->parent_repo);

  g_clear_object (&self->repodir);
  g_clear_object (&self->tmp_dir);
  g_clear_object (&self->pending_dir);
  g_clear_object (&self->local_heads_dir);
  g_clear_object (&self->remote_heads_dir);
  g_clear_object (&self->objects_dir);
  g_clear_object (&self->pack_dir);
  g_clear_object (&self->remote_cache_dir);
  g_clear_object (&self->config_file);
  if (self->loose_object_devino_hash)
    g_hash_table_destroy (self->loose_object_devino_hash);
  if (self->config)
    g_key_file_free (self->config);
  g_clear_pointer (&self->cached_meta_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&self->cached_content_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_hash_table_destroy (self->cached_pack_index_mappings);
  g_hash_table_destroy (self->cached_pack_data_mappings);
  g_mutex_clear (&self->cache_lock);

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->repodir = g_file_new_for_path (ot_gfile_get_path_cached (g_value_get_object (value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->repodir);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject *
ostree_repo_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  OstreeRepo *self;
  GObject *object;
  GObjectClass *parent_class;

  parent_class = G_OBJECT_CLASS (ostree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);
  self = (OstreeRepo*)object;

  g_assert (self->repodir != NULL);
  
  self->tmp_dir = g_file_resolve_relative_path (self->repodir, "tmp");
  self->pending_dir = g_file_resolve_relative_path (self->repodir, "tmp/pending");
  self->local_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/heads");
  self->remote_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/remotes");
  
  self->objects_dir = g_file_get_child (self->repodir, "objects");
  self->pack_dir = g_file_get_child (self->objects_dir, "pack");
  self->remote_cache_dir = g_file_get_child (self->repodir, "remote-cache");
  self->config_file = g_file_get_child (self->repodir, "config");

  return object;
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = ostree_repo_constructor;
  object_class->get_property = ostree_repo_get_property;
  object_class->set_property = ostree_repo_set_property;
  object_class->finalize = ostree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_repo_init (OstreeRepo *self)
{
  g_mutex_init (&self->cache_lock);
  self->cached_pack_index_mappings = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            g_free,
                                                            (GDestroyNotify)g_variant_unref);
  self->cached_pack_data_mappings = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                           g_free,
                                                           (GDestroyNotify)g_mapped_file_unref);
}

OstreeRepo*
ostree_repo_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lfree char *rev = NULL;

  if (!ot_gfile_load_contents_utf8 (f, &rev, NULL, NULL, &temp_error))
    goto out;

  if (rev == NULL)
    {
      if (g_error_matches (temp_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      g_strchomp (rev);
    }

  if (g_str_has_prefix (rev, "ref: "))
    {
      ot_lobj GFile *ref = NULL;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (self->local_heads_dir, rev + 5);
      subret = parse_rev_file (self, ref, &ref_sha256, error);
        
      if (!subret)
        {
          g_free (ref_sha256);
          goto out;
        }
      
      g_free (rev);
      rev = ref_sha256;
    }
  else 
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;
    }

  ot_transfer_out_value(sha256, &rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_rev_in_remotes (OstreeRepo         *self,
                     const char         *rev,
                     GFile             **out_file,
                     GError            **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *ret_file = NULL;

  dir_enum = g_file_enumerate_children (self->remote_heads_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, NULL, error)) != NULL)
    {
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&child);
          child = g_file_get_child (self->remote_heads_dir,
                                    g_file_info_get_name (file_info));
          g_clear_object (&ret_file);
          ret_file = g_file_resolve_relative_path (child, rev);
          if (!g_file_query_exists (ret_file, NULL))
            g_clear_object (&ret_file);
        }

      g_clear_object (&file_info);
      
      if (ret_file)
        break;
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         gboolean        allow_noent,
                         char          **sha256,
                         GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lfree char *tmp = NULL;
  ot_lfree char *tmp2 = NULL;
  ot_lfree char *ret_rev = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *origindir = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lvariant GVariant *parent_csum_v = NULL;
  
  g_return_val_if_fail (rev != NULL, FALSE);

  if (!ostree_validate_rev (rev, error))
    goto out;

  /* We intentionally don't allow a ref that looks like a checksum */
  if (ostree_validate_checksum_string (rev, NULL))
    {
      ret_rev = g_strdup (rev);
    }
  else if (g_str_has_suffix (rev, "^"))
    {
      tmp = g_strdup (rev);
      tmp[strlen(tmp) - 1] = '\0';

      if (!ostree_repo_resolve_rev (self, tmp, allow_noent, &tmp2, error))
        goto out;

      if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, tmp2, &commit, error))
        goto out;
      
      g_variant_get_child (commit, 1, "@ay", &parent_csum_v);
      if (g_variant_n_children (parent_csum_v) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s has no parent", tmp2);
          goto out;
        }
      ret_rev = ostree_checksum_from_bytes_v (parent_csum_v);
    }
  else
    {
      child = g_file_resolve_relative_path (self->local_heads_dir, rev);

      if (!g_file_query_exists (child, NULL))
        {
          g_clear_object (&child);

          child = g_file_resolve_relative_path (self->remote_heads_dir, rev);

          if (!g_file_query_exists (child, NULL))
            {
              g_clear_object (&child);
              
              if (!find_rev_in_remotes (self, rev, &child, error))
                goto out;
              
              if (child == NULL)
                {
                  if (self->parent_repo)
                    {
                      if (!ostree_repo_resolve_rev (self->parent_repo, rev,
                                                    allow_noent, &ret_rev,
                                                    error))
                        goto out;
                    }
                  else if (!allow_noent)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Rev '%s' not found", rev);
                      goto out;
                    }
                  else
                    g_clear_object (&child);
                }
            }
        }

      if (child)
        {
          if (!ot_gfile_load_contents_utf8 (child, &ret_rev, NULL, NULL, &temp_error))
            {
              g_propagate_error (error, temp_error);
              g_prefix_error (error, "Couldn't open ref '%s': ", ot_gfile_get_path_cached (child));
              goto out;
            }

          g_strchomp (ret_rev);
          if (!ostree_validate_checksum_string (ret_rev, error))
            goto out;
        }
    }

  ot_transfer_out_value(sha256, &ret_rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GError **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  int i;
  ot_lobj GFile *parent = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lptrarray GPtrArray *components = NULL;

  if (!ostree_validate_checksum_string (sha256, error))
    goto out;

  if (ostree_validate_checksum_string (name, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Rev name '%s' looks like a checksum", name);
      goto out;
    }

  if (!ot_util_path_split_validate (name, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty ref name");
      goto out;
    }

  parent = g_object_ref (parentdir);
  for (i = 0; i+1 < components->len; i++)
    {
      child = g_file_get_child (parent, (char*)components->pdata[i]);

      if (!ot_gfile_ensure_directory (child, FALSE, error))
        goto out;

      g_clear_object (&parent);
      parent = child;
      child = NULL;
    }

  child = g_file_get_child (parent, components->pdata[components->len - 1]);
  if ((out = (GOutputStream*)g_file_replace (child, NULL, FALSE, 0, NULL, error)) == NULL)
    goto out;
  if (!g_output_stream_write_all (out, sha256, strlen (sha256), &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_write_all (out, "\n", 1, &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_close (out, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_get_config:
 * @self:
 *
 * Returns: (transfer none): The repository configuration; do not modify
 */
GKeyFile *
ostree_repo_get_config (OstreeRepo *self)
{
  g_return_val_if_fail (self->inited, NULL);

  return self->config;
}

/**
 * ostree_repo_copy_config:
 * @self:
 *
 * Returns: (transfer full): A newly-allocated copy of the repository config
 */
GKeyFile *
ostree_repo_copy_config (OstreeRepo *self)
{
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (self->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (self->config, &len, NULL);
  if (!g_key_file_load_from_data (copy, data, len, 0, NULL))
    g_assert_not_reached ();
  g_free (data);
  return copy;
}

/**
 * ostree_repo_write_config:
 * @self:
 * @new_config: Overwrite the config file with this data.  Do not change later!
 * @error: a #GError
 *
 * Save @new_config in place of this repository's config file.  Note
 * that @new_config should not be modified after - this function
 * simply adds a reference.
 */
gboolean
ostree_repo_write_config (OstreeRepo *self,
                          GKeyFile   *new_config,
                          GError    **error)
{
  gboolean ret = FALSE;
  ot_lfree char *data = NULL;
  gsize len;

  g_return_val_if_fail (self->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_replace_contents (self->config_file, data, len, NULL, FALSE, 0, NULL,
                                NULL, error))
    goto out;
  
  g_key_file_free (self->config);
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_data (self->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                  const char    *section,
                                  const char    *value,
                                  gboolean       default_value,
                                  gboolean      *out_bool,
                                  GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean ret_bool;

  ret_bool = g_key_file_get_boolean (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_bool = default_value;
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  *out_bool = ret_bool;
 out:
  return ret;
}

static gboolean
keyfile_get_value_with_default (GKeyFile      *keyfile,
                                const char    *section,
                                const char    *value,
                                const char    *default_value,
                                char         **out_value,
                                GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lfree char *ret_value;

  ret_value = g_key_file_get_value (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_value = g_strdup (default_value);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value(out_value, &ret_value);
 out:
  return ret;
}
                                
gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  gboolean ret = FALSE;
  gboolean is_archive;
  ot_lfree char *version = NULL;
  ot_lfree char *mode = NULL;
  ot_lfree char *parent_repo_path = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->inited)
    return TRUE;

  if (!g_file_test (ot_gfile_get_path_cached (self->objects_dir), G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'",
                   ot_gfile_get_path_cached (self->objects_dir));
      goto out;
    }

  if (!ot_gfile_ensure_directory (self->pending_dir, FALSE, error))
    goto out;
  
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_file (self->config, ot_gfile_get_path_cached (self->config_file), 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      goto out;
    }

  version = g_key_file_get_value (self->config, "core", "repo_version", error);
  if (!version)
    goto out;

  if (strcmp (version, "1") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  if (!keyfile_get_boolean_with_default (self->config, "core", "archive",
                                         FALSE, &is_archive, error))
    goto out;
  
  if (is_archive)
    self->mode = OSTREE_REPO_MODE_ARCHIVE;
  else
    {
      if (!keyfile_get_value_with_default (self->config, "core", "mode",
                                           "bare", &mode, error))
        goto out;

      if (strcmp (mode, "bare") == 0)
        self->mode = OSTREE_REPO_MODE_BARE;
      else if (strcmp (mode, "archive") == 0)
        self->mode = OSTREE_REPO_MODE_ARCHIVE;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid mode '%s' in repository configuration", mode);
          goto out;
        }
    }

  if (!keyfile_get_value_with_default (self->config, "core", "parent",
                                       NULL, &parent_repo_path, error))
    goto out;

  if (parent_repo_path && parent_repo_path[0])
    {
      ot_lobj GFile *parent_repo_f = g_file_new_for_path (parent_repo_path);

      self->parent_repo = ostree_repo_new (parent_repo_f);

      if (!ostree_repo_check (self->parent_repo, error))
        {
          g_prefix_error (error, "While checking parent repository '%s': ",
                          ot_gfile_get_path_cached (parent_repo_f));
          goto out;
        }
    }

  self->inited = TRUE;
  
  ret = TRUE;
 out:
  return ret;
}

GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  return self->repodir;
}

GFile *
ostree_repo_get_tmpdir (OstreeRepo  *self)
{
  return self->tmp_dir;
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  g_return_val_if_fail (self->inited, FALSE);

  return self->mode;
}

/**
 * ostree_repo_get_parent:
 * @self:
 * 
 * Before this function can be used, ostree_repo_init() must have been
 * called.
 *
 * Returns: (transfer none): Parent repository, or %NULL if none
 */
OstreeRepo *
ostree_repo_get_parent (OstreeRepo  *self)
{
  return self->parent_repo;
}

GFile *
ostree_repo_get_file_object_path (OstreeRepo   *self,
                                  const char   *checksum)
{
  return ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
}

GFile *
ostree_repo_get_archive_content_path (OstreeRepo    *self,
                                      const char    *checksum)
{
  ot_lfree char *path = NULL;

  g_assert (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE);

  path = ostree_get_relative_archive_content_path (checksum);
  return g_file_resolve_relative_path (self->repodir, path);
}

static gboolean
commit_loose_object_impl (OstreeRepo        *self,
                          GFile             *tempfile_path,
                          GFile             *dest,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *parent = NULL;

  parent = g_file_get_parent (dest);
  if (!ot_gfile_ensure_directory (parent, FALSE, error))
    goto out;
  
  if (link (ot_gfile_get_path_cached (tempfile_path), ot_gfile_get_path_cached (dest)) < 0)
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          ot_gfile_get_path_cached (dest));
          goto out;
        }
    }

  (void) unlink (ot_gfile_get_path_cached (tempfile_path));
  ret = TRUE;
 out:
  return ret;
}

static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *checksum,
                             OstreeObjectType   objtype,
                             GFile             *tempfile_path,
                             GCancellable      *cancellable,
                             GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dest_file = NULL;

  dest_file = ostree_repo_get_object_path (self, checksum, objtype);

  if (!commit_loose_object_impl (self, tempfile_path, dest_file,
                                 cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef enum {
  OSTREE_REPO_STAGE_FLAGS_STORE_IF_PACKED = (1<<0),
  OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID = (1<<1)
} OstreeRepoStageFlags;

static gboolean
stage_object_internal (OstreeRepo         *self,
                       OstreeRepoStageFlags flags,
                       OstreeObjectType    objtype,
                       GInputStream       *input,
                       guint64             file_object_length,
                       const char         *expected_checksum,
                       guchar            **out_csum,
                       GCancellable       *cancellable,
                       GError            **error)
{
 gboolean ret = FALSE;
  const char *actual_checksum;
  gboolean do_commit;
  ot_lobj GFileInfo *temp_info = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFile *raw_temp_file = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree char *pack_checksum = NULL;
  ot_lfree guchar *ret_csum = NULL;
  ot_lobj OstreeChecksumInputStream *checksum_input = NULL;
  GChecksum *checksum = NULL;
  gboolean staged_raw_file = FALSE;
  gboolean staged_archive_file = FALSE;

  if (out_csum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (input)
        checksum_input = ostree_checksum_input_stream_new (input, checksum);
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE
      && (flags & OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID) > 0)
    {
      ot_lobj GInputStream *file_input = NULL;
      ot_lobj GFileInfo *file_info = NULL;
      ot_lvariant GVariant *xattrs = NULL;

      if (!ostree_content_stream_parse (checksum_input ? (GInputStream*)checksum_input : input,
                                        file_object_length, FALSE,
                                        &file_input, &file_info, &xattrs,
                                        cancellable, error))
        goto out;

      if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_BARE)
        {
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;
          staged_raw_file = TRUE;
        }
      else
        {
          ot_lvariant GVariant *file_meta = NULL;
          ot_lobj GInputStream *file_meta_input = NULL;
          ot_lobj GFileInfo *archive_content_file_info = NULL;

          file_meta = ostree_file_header_new (file_info, xattrs);
          file_meta_input = ot_variant_read (file_meta);

          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   NULL, NULL, file_meta_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              ot_lobj GOutputStream *content_out = NULL;
              guint32 src_mode;
              guint32 target_mode;

              if (!ostree_create_temp_regular_file (self->tmp_dir,
                                                    ostree_object_type_to_string (objtype), NULL,
                                                    &raw_temp_file, &content_out,
                                                    cancellable, error))
                goto out;

              /* Don't make setuid files in the repository; all we want to preserve
               * is file type and permissions.
               */
              src_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
              target_mode = src_mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_IFMT);
              
              if (chmod (ot_gfile_get_path_cached (raw_temp_file), target_mode) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }

              if (g_output_stream_splice (content_out, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;

              staged_archive_file = TRUE;
            }
        }
    }
  else
    {
      if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                               ostree_object_type_to_string (objtype), NULL,
                                               NULL, NULL,
                                               checksum_input ? (GInputStream*)checksum_input : input,
                                               &temp_file,
                                               cancellable, error))
        goto out;
    }
          
  if (!checksum)
    actual_checksum = expected_checksum;
  else
    {
      actual_checksum = g_checksum_get_string (checksum);
      if (expected_checksum && strcmp (actual_checksum, expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted %s object %s (actual checksum is %s)",
                       ostree_object_type_to_string (objtype),
                       expected_checksum, actual_checksum);
          goto out;
        }
    }
          
  if (!(flags & OSTREE_REPO_STAGE_FLAGS_STORE_IF_PACKED))
    {
      gboolean have_obj;
          
      if (!ostree_repo_has_object (self, objtype, actual_checksum, &have_obj,
                                   cancellable, error))
        goto out;
          
      do_commit = !have_obj;
    }
  else
    do_commit = TRUE;

  if (do_commit)
    {
      /* Only do this if we *didn't* stage a bare file above */
      if (!staged_raw_file
          && objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE)
        {
          ot_lobj GInputStream *file_input = NULL;
          ot_lobj GFileInfo *file_info = NULL;
          ot_lvariant GVariant *xattrs = NULL;
              
          if (!ostree_content_file_parse (temp_file, FALSE, &file_input,
                                          &file_info, &xattrs,
                                          cancellable, error))
            goto out;
              
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &raw_temp_file,
                                                   cancellable, error))
            goto out;

          if (!commit_loose_object_trusted (self, actual_checksum, objtype, 
                                            raw_temp_file, cancellable, error))
            goto out;
          g_clear_object (&raw_temp_file);
        }
      else
        {
          /* Commit content first so the process is atomic */
          if (staged_archive_file)
            {
              ot_lobj GFile *archive_content_dest = NULL;

              archive_content_dest = ostree_repo_get_archive_content_path (self, actual_checksum);
                                                                   
              if (!commit_loose_object_impl (self, raw_temp_file, archive_content_dest,
                                             cancellable, error))
                goto out;
              g_clear_object (&raw_temp_file);
            }
          if (!commit_loose_object_trusted (self, actual_checksum, objtype, 
                                            temp_file, cancellable, error))
            goto out;
          g_clear_object (&temp_file);
        }
    }
      
  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  if (raw_temp_file)
    (void) unlink (ot_gfile_get_path_cached (raw_temp_file));
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
  return ret;
}

static gboolean
stage_object (OstreeRepo         *self,
              OstreeRepoStageFlags flags,
              OstreeObjectType    objtype,
              GInputStream       *input,
              guint64             file_object_length,
              const char         *expected_checksum,
              guchar            **out_csum,
              GCancellable       *cancellable,
              GError            **error)
{
  gboolean ret = FALSE;
  guint64 pack_offset;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree char *pack_checksum = NULL;
  ot_lfree guchar *ret_csum = NULL;

  g_return_val_if_fail (self->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_csum);

  if (expected_checksum)
    {
      if (!(flags & OSTREE_REPO_STAGE_FLAGS_STORE_IF_PACKED))
        {
          if (!repo_find_object (self, objtype, expected_checksum, FALSE,
                                 &stored_path, &pack_checksum, &pack_offset,
                                 cancellable, error))
            goto out;
        }
      else
        {
          if (!repo_find_object (self, objtype, expected_checksum, FALSE,
                                 &stored_path, NULL, NULL,
                                 cancellable, error))
            goto out;
        }
    }

  if (stored_path == NULL && pack_checksum == NULL)
    {
      if (!stage_object_internal (self, flags, objtype, input,
                                  file_object_length, expected_checksum,
                                  out_csum ? &ret_csum : NULL,
                                  cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  return ret;
}

static gboolean
get_loose_object_dirs (OstreeRepo       *self,
                       GPtrArray       **out_object_dirs,
                       GCancellable     *cancellable,
                       GError          **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lptrarray GPtrArray *ret_object_dirs = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;

  ret_object_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  enumerator = g_file_enumerate_children (self->objects_dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          GFile *objdir = g_file_get_child (self->objects_dir, name);
          g_ptr_array_add (ret_object_dirs, objdir);  /* transfer ownership */
        }
      g_clear_object (&file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_object_dirs, &ret_object_dirs);
 out:
  return ret;
}

typedef struct {
  dev_t dev;
  ino_t ino;
} OstreeDevIno;

static guint
devino_hash (gconstpointer a)
{
  OstreeDevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  OstreeDevIno *a_i = (gpointer)a;
  OstreeDevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

static gboolean
scan_loose_devino (OstreeRepo                     *self,
                   GHashTable                     *devino_cache,
                   GCancellable                   *cancellable,
                   GError                        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  guint i;
  ot_lptrarray GPtrArray *object_dirs = NULL;
  ot_lobj GFile *objdir = NULL;

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      ot_lobj GFileEnumerator *enumerator = NULL;
      ot_lobj GFileInfo *file_info = NULL;
      const char *dirname;

      enumerator = g_file_enumerate_children (objdir, OSTREE_GIO_FAST_QUERYINFO, 
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, 
                                              error);
      if (!enumerator)
        goto out;

      dirname = ot_gfile_get_basename_cached (objdir);
  
      while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
        {
          const char *name;
          const char *dot;
          guint32 type;
          OstreeDevIno *key;
          GString *checksum;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_DIRECTORY)
            {
              g_clear_object (&file_info);
              continue;
            }
      
          if (!((ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE
                 && g_str_has_suffix (name, ".filecontent"))
                || (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_BARE
                    && g_str_has_suffix (name, ".file"))))
            {
              g_clear_object (&file_info);
              continue;
            }

          dot = strrchr (name, '.');
          g_assert (dot);

          if ((dot - name) != 62)
            {
              g_clear_object (&file_info);
              continue;
            }
                  
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = g_new (OstreeDevIno, 1);
          key->dev = g_file_info_get_attribute_uint32 (file_info, "unix::device");
          key->ino = g_file_info_get_attribute_uint64 (file_info, "unix::inode");
          
          g_hash_table_replace (devino_cache, key, g_string_free (checksum, FALSE));
          g_clear_object (&file_info);
        }

      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      if (!g_file_enumerator_close (enumerator, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static const char *
devino_cache_lookup (OstreeRepo           *self,
                     GFileInfo            *finfo)
{
  OstreeDevIno dev_ino;

  if (!self->loose_object_devino_hash)
    return NULL;

  dev_ino.dev = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  dev_ino.ino = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
  return g_hash_table_lookup (self->loose_object_devino_hash, &dev_ino);
}

gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == FALSE, FALSE);

  self->in_transaction = TRUE;

  if (!self->loose_object_devino_hash)
    {
      self->loose_object_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, g_free);
    }
  g_hash_table_remove_all (self->loose_object_devino_hash);
  if (!scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean      
ostree_repo_commit_transaction (OstreeRepo     *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  ret = TRUE;
  /* out: */
  self->in_transaction = FALSE;
  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  return ret;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  self->in_transaction = FALSE;
  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  ret = TRUE;
  return ret;
}

static gboolean
stage_metadata_object (OstreeRepo         *self,
                       OstreeObjectType    type,
                       GVariant           *variant,
                       guchar            **out_csum,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GInputStream *input = NULL;

  input = ot_variant_read (variant);
  
  if (!stage_object (self, 0, type, input, 0, NULL,
                     out_csum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_directory_meta (OstreeRepo   *self,
                      GFileInfo    *file_info,
                      GVariant     *xattrs,
                      guchar      **out_csum,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  if (!stage_metadata_object (self, OSTREE_OBJECT_TYPE_DIR_META, 
                              dirmeta, out_csum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

GFile *
ostree_repo_get_object_path (OstreeRepo  *self,
                             const char    *checksum,
                             OstreeObjectType type)
{
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, type);
  ret = g_file_resolve_relative_path (self->repodir, relpath);
  g_free (relpath);
 
  return ret;
}

gboolean      
ostree_repo_stage_object_trusted (OstreeRepo   *self,
                                  OstreeObjectType objtype,
                                  const char   *checksum,
                                  gboolean          store_if_packed,
                                  GInputStream     *object_input,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  int flags = 0;
  if (store_if_packed)
    flags |= OSTREE_REPO_STAGE_FLAGS_STORE_IF_PACKED;
  return stage_object (self, flags, objtype,
                       object_input, 0, checksum, NULL,
                       cancellable, error);
}

gboolean
ostree_repo_stage_object (OstreeRepo       *self,
                          OstreeObjectType  objtype,
                          const char       *expected_checksum,
                          GInputStream     *object_input,
                          GCancellable     *cancellable,
                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lfree guchar *actual_csum = NULL;
  
  if (!stage_object (self, 0, objtype, 
                     object_input, 0, expected_checksum, &actual_csum,
                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean      
ostree_repo_stage_file_object_trusted (OstreeRepo       *self,
                                       const char       *checksum,
                                       gboolean          store_if_packed,
                                       GInputStream     *object_input,
                                       guint64           length,
                                       GCancellable     *cancellable,
                                       GError          **error)
{
  int flags = OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID;
  if (store_if_packed)
    flags |= OSTREE_REPO_STAGE_FLAGS_STORE_IF_PACKED;
  return stage_object (self, flags, OSTREE_OBJECT_TYPE_FILE,
                       object_input, length, checksum, NULL,
                       cancellable, error);
}

gboolean
ostree_repo_stage_file_object (OstreeRepo       *self,
                               const char       *expected_checksum,
                               GInputStream     *object_input,
                               guint64           length,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean ret = FALSE;
  int flags = OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID;
  ot_lfree guchar *actual_csum = NULL;
  
  if (!stage_object (self, flags, OSTREE_OBJECT_TYPE_FILE, 
                     object_input, length, expected_checksum, &actual_csum,
                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

static gboolean
enumerate_refs_recurse (OstreeRepo    *repo,
                        GFile         *base,
                        GFile         *dir,
                        GHashTable    *refs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFile *child = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (dir, g_file_info_get_name (file_info));
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!enumerate_refs_recurse (repo, base, child, refs, cancellable, error))
            goto out;
        }
      else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          char *contents;
          gsize len;

          if (!g_file_load_contents (child, cancellable, &contents, &len, NULL, error))
            goto out;

          g_strchomp (contents);

          g_hash_table_insert (refs, g_file_get_relative_path (base, child), contents);
        }

      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_list_all_refs (OstreeRepo       *repo,
                           GHashTable      **out_all_refs,
                           GCancellable     *cancellable,
                           GError          **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_all_refs = NULL;
  ot_lobj GFile *heads_dir = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  heads_dir = g_file_resolve_relative_path (ostree_repo_get_path (repo), "refs/heads");
  if (!enumerate_refs_recurse (repo, heads_dir, heads_dir, ret_all_refs, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_all_refs, &ret_all_refs);
 out:
  return ret;
}

static gboolean
write_ref_summary (OstreeRepo      *self,
                   GCancellable    *cancellable,
                   GError         **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gsize bytes_written;
  ot_lhash GHashTable *all_refs = NULL;
  ot_lobj GFile *summary_path = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lfree char *buf = NULL;

  if (!ostree_repo_list_all_refs (self, &all_refs, cancellable, error))
    goto out;

  summary_path = g_file_resolve_relative_path (ostree_repo_get_path (self),
                                               "refs/summary");

  out = (GOutputStream*) g_file_replace (summary_path, NULL, FALSE, 0, cancellable, error);
  if (!out)
    goto out;
  
  g_hash_table_iter_init (&hash_iter, all_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *sha256 = value;

      g_free (buf);
      buf = g_strdup_printf ("%s %s\n", sha256, name);
      if (!g_output_stream_write_all (out, buf, strlen (buf), &bytes_written, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close (out, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean      
ostree_repo_write_ref (OstreeRepo  *self,
                       const char  *remote,
                       const char  *name,
                       const char  *rev,
                       GError     **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dir = NULL;

  if (remote == NULL)
    dir = g_object_ref (self->local_heads_dir);
  else
    {
      dir = g_file_get_child (self->remote_heads_dir, remote);

      if (!ot_gfile_ensure_directory (dir, FALSE, error))
        goto out;
    }

  if (!write_checksum_file (dir, name, rev, error))
    goto out;

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE)
    {
      if (!write_ref_summary (self, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_commit (OstreeRepo *self,
                          const char   *branch,
                          const char   *parent,
                          const char   *subject,
                          const char   *body,
                          GVariant     *metadata,
                          GVariant     *related_objects,
                          const char   *root_contents_checksum,
                          const char   *root_metadata_checksum,
                          char        **out_commit,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  ot_lfree char *ret_commit = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lfree guchar *commit_csum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (root_contents_checksum != NULL, FALSE);
  g_return_val_if_fail (root_metadata_checksum != NULL, FALSE);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          related_objects ? related_objects : g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (root_contents_checksum),
                          ostree_checksum_to_bytes_v (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!stage_metadata_object (self, OSTREE_OBJECT_TYPE_COMMIT,
                              commit, &commit_csum, cancellable, error))
    goto out;

  ret_commit = ostree_checksum_from_bytes (commit_csum);

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  if (now)
    g_date_time_unref (now);
  return ret;
}

static gboolean
list_files_in_dir_matching (GFile                  *dir,
                            const char             *prefix,
                            const char             *suffix,
                            GPtrArray             **out_files,
                            GCancellable           *cancellable,
                            GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lptrarray GPtrArray *ret_files = NULL;

  g_return_val_if_fail (prefix != NULL || suffix != NULL, FALSE);

  ret_files = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type != G_FILE_TYPE_REGULAR)
        goto loop_next;

      if (prefix)
        {
          if (!g_str_has_prefix (name, prefix))
            goto loop_next;
        }
      if (suffix)
        {
          if (!g_str_has_suffix (name, suffix))
            goto loop_next;
        }

      g_ptr_array_add (ret_files, g_file_get_child (dir, name));
      
    loop_next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_files, &ret_files);
 out:
  return ret;
}

static gboolean
map_variant_file_check_header_string (GFile         *path,
                                      const GVariantType  *variant_type,
                                      const char    *expected_header,
                                      gboolean       trusted,
                                      GVariant     **out_variant,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  gboolean ret = FALSE;
  const char *header;
  ot_lvariant GVariant *ret_variant = NULL;

  if (!ot_util_variant_map (path, variant_type, trusted, &ret_variant, error))
    goto out;

  g_variant_get_child (ret_variant, 0, "&s", &header);

  if (strcmp (header, expected_header) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid variant file '%s', expected header '%s'",
                   ot_gfile_get_path_cached (path),
                   expected_header);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}


static char *
get_checksum_from_pack_name (const char *name)
{
  const char *dash;
  const char *dot;

  dash = strchr (name, '-');
  g_assert (dash);
  dot = strrchr (name, '.');
  g_assert (dot);

  g_assert_cmpint (dot - (dash + 1), ==, 64);
  
  return g_strndup (dash + 1, 64);
}

static gboolean
list_pack_indexes_from_dir (OstreeRepo              *self,
                            gboolean                 is_meta,
                            GPtrArray              **out_indexes,
                            GCancellable            *cancellable,
                            GError                 **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *index_files = NULL;
  ot_lptrarray GPtrArray *ret_indexes = NULL;

  if (!list_files_in_dir_matching (self->pack_dir,
                                   is_meta ? "ostmetapack-" : "ostdatapack-", ".index",
                                   &index_files, 
                                   cancellable, error))
    goto out;

  ret_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free);
  for (i = 0; i < index_files->len; i++)
    {
      GFile *index_path = index_files->pdata[i];
      const char *basename = ot_gfile_get_basename_cached (index_path);
      g_ptr_array_add (ret_indexes, get_checksum_from_pack_name (basename));
    }

  ret = TRUE;
  ot_transfer_out_value (out_indexes, &ret_indexes);
 out:
  return ret;
}

static gboolean
list_pack_checksums_from_superindex_file (GFile         *superindex_path,
                                          GPtrArray    **out_meta_indexes,
                                          GPtrArray    **out_data_indexes,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  gboolean ret = FALSE;
  const char *magic;
  ot_lptrarray GPtrArray *ret_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_data_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  ot_lvariant GVariant *checksum = NULL;
  ot_lvariant GVariant *bloom = NULL;
  GVariantIter *meta_variant_iter = NULL;
  GVariantIter *data_variant_iter = NULL;

  if (!ot_util_variant_map (superindex_path, OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT,
                            TRUE, &superindex_variant, error))
    goto out;
  
  g_variant_get (superindex_variant, "(&s@a{sv}a(ayay)a(ayay))",
                 &magic, NULL, &meta_variant_iter, &data_variant_iter);
  
  if (strcmp (magic, "OSTv0SUPERPACKINDEX") != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid header in super pack index");
      goto out;
    }

  ret_meta_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
  while (g_variant_iter_loop (meta_variant_iter, "(@ay@ay)",
                              &checksum, &bloom))
    g_ptr_array_add (ret_meta_indexes, ostree_checksum_from_bytes_v (checksum));
  checksum = NULL;
  bloom = NULL;

  ret_data_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
  while (g_variant_iter_loop (data_variant_iter, "(@ay@ay)",
                              &checksum, &bloom))
    g_ptr_array_add (ret_data_indexes, ostree_checksum_from_bytes_v (checksum));
  checksum = NULL;
  bloom = NULL;

  ret = TRUE;
  ot_transfer_out_value (out_meta_indexes, &ret_meta_indexes);
  ot_transfer_out_value (out_data_indexes, &ret_data_indexes);
 out:
  if (meta_variant_iter)
    g_variant_iter_free (meta_variant_iter);
  if (data_variant_iter)
    g_variant_iter_free (data_variant_iter);
  return ret;
}

gboolean
ostree_repo_list_pack_indexes (OstreeRepo              *self,
                               GPtrArray              **out_meta_indexes,
                               GPtrArray              **out_data_indexes,
                               GCancellable            *cancellable,
                               GError                 **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *superindex_path = NULL;
  ot_lptrarray GPtrArray *ret_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_data_indexes = NULL;

  g_mutex_lock (&self->cache_lock);
  if (self->cached_meta_indexes)
    {
      ret_meta_indexes = g_ptr_array_ref (self->cached_meta_indexes);
      ret_data_indexes = g_ptr_array_ref (self->cached_content_indexes);
    }
  else
    {
      superindex_path = g_file_get_child (self->pack_dir, "index");

      if (g_file_query_exists (superindex_path, cancellable))
        {
          if (!list_pack_checksums_from_superindex_file (superindex_path, &ret_meta_indexes,
                                                         &ret_data_indexes,
                                                         cancellable, error))
            goto out;
        }
      else
        {
          ret_meta_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
          ret_data_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
        }

      self->cached_meta_indexes = g_ptr_array_ref (ret_meta_indexes);
      self->cached_content_indexes = g_ptr_array_ref (ret_data_indexes);
    }

  ret = TRUE;
  ot_transfer_out_value (out_meta_indexes, &ret_meta_indexes);
  ot_transfer_out_value (out_data_indexes, &ret_data_indexes);
 out:
  g_mutex_unlock (&self->cache_lock);
  return ret;
}

static gboolean
create_index_bloom (OstreeRepo          *self,
                    const char          *pack_checksum,
                    GVariant           **out_bloom,
                    GCancellable        *cancellable,
                    GError             **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *ret_bloom;

  /* TODO - define and compute bloom filter */

  ret_bloom = ot_gvariant_new_bytearray (NULL, 0);
  g_variant_ref_sink (ret_bloom);

  ret = TRUE;
  ot_transfer_out_value (out_bloom, &ret_bloom);
  /* out: */
  return ret;
}

static gboolean
append_index_builder (OstreeRepo           *self,
                      GPtrArray            *indexes,
                      GVariantBuilder      *builder,
                      GCancellable         *cancellable,
                      GError              **error)
{
  gboolean ret = FALSE;
  guint i;

  for (i = 0; i < indexes->len; i++)
    {
      const char *pack_checksum = indexes->pdata[i];
      ot_lvariant GVariant *bloom = NULL;

      if (!create_index_bloom (self, pack_checksum, &bloom, cancellable, error))
        goto out;

      g_variant_builder_add (builder,
                             "(@ay@ay)",
                             ostree_checksum_to_bytes_v (pack_checksum),
                             bloom);
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * Regenerate the pack superindex file based on the set of pack
 * indexes currently in the filesystem.
 */
gboolean
ostree_repo_regenerate_pack_index (OstreeRepo       *self,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *superindex_path = NULL;
  ot_lptrarray GPtrArray *pack_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  GVariantBuilder *meta_index_content_builder = NULL;
  GVariantBuilder *data_index_content_builder = NULL;

  g_clear_pointer (&self->cached_meta_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&self->cached_content_indexes, (GDestroyNotify) g_ptr_array_unref);

  superindex_path = g_file_get_child (self->pack_dir, "index");

  g_clear_pointer (&pack_indexes, (GDestroyNotify) g_ptr_array_unref);
  if (!list_pack_indexes_from_dir (self, TRUE, &pack_indexes,
                                   cancellable, error))
    goto out;
  meta_index_content_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));
  if (!append_index_builder (self, pack_indexes, meta_index_content_builder,
                             cancellable, error))
    goto out;

  g_clear_pointer (&pack_indexes, (GDestroyNotify) g_ptr_array_unref);
  if (!list_pack_indexes_from_dir (self, FALSE, &pack_indexes,
                                   cancellable, error))
    goto out;
  data_index_content_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));
  if (!append_index_builder (self, pack_indexes, data_index_content_builder,
                             cancellable, error))
    goto out;

  superindex_variant = g_variant_new ("(s@a{sv}@a(ayay)@a(ayay))",
                                      "OSTv0SUPERPACKINDEX",
                                      g_variant_new_array (G_VARIANT_TYPE ("{sv}"),
                                                           NULL, 0),
                                      g_variant_builder_end (meta_index_content_builder),
                                      g_variant_builder_end (data_index_content_builder));
  g_variant_ref_sink (superindex_variant);

  if (!ot_util_variant_save (superindex_path, superindex_variant,
                             cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (meta_index_content_builder)
    g_variant_builder_unref (meta_index_content_builder);
  if (data_index_content_builder)
    g_variant_builder_unref (data_index_content_builder);
  return ret;
}

static GFile *
get_pack_index_path (GFile            *parent,
                     gboolean          is_meta,
                     const char       *checksum)
{
  char *path = ostree_get_pack_index_name (is_meta, checksum);
  GFile *ret = g_file_resolve_relative_path (parent, path);
  g_free (path);
  return ret;
}

static GFile *
get_pack_data_path (GFile            *parent,
                    gboolean          is_meta,
                    const char       *checksum)
{
  char *path = ostree_get_pack_data_name (is_meta, checksum);
  GFile *ret = g_file_resolve_relative_path (parent, path);
  g_free (path);
  return ret;
}

gboolean
ostree_repo_add_pack_file (OstreeRepo       *self,
                           const char       *pack_checksum,
                           gboolean          is_meta,
                           GFile            *index_path,
                           GFile            *data_path,
                           GCancellable     *cancellable,
                           GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *pack_index_path = NULL;
  ot_lobj GFile *pack_data_path = NULL;

  if (!ot_gfile_ensure_directory (self->pack_dir, FALSE, error))
    goto out;

  pack_data_path = get_pack_data_path (self->pack_dir, is_meta, pack_checksum);
  if (!ot_gfile_rename (data_path, pack_data_path, cancellable, error))
    goto out;

  pack_index_path = get_pack_index_path (self->pack_dir, is_meta, pack_checksum);
  if (!ot_gfile_rename (index_path, pack_index_path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
ensure_remote_cache_dir (OstreeRepo       *self,
                         const char       *remote_name,
                         GFile           **out_cache_dir,
                         GCancellable     *cancellable,
                         GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ret_cache_dir = NULL;

  ret_cache_dir = g_file_get_child (self->remote_cache_dir, remote_name);
  
  if (!ot_gfile_ensure_directory (ret_cache_dir, FALSE, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_cache_dir, &ret_cache_dir);
 out:
  return ret;
}

static gboolean
delete_no_longer_referenced (OstreeRepo                   *self,
                             GFile                        *cache_path,
                             const char                   *prefix,
                             const char                   *suffix,
                             GHashTable                   *new_files,
                             GPtrArray                    *inout_cached,
                             GCancellable                 *cancellable,
                             GError                      **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *current_files = NULL;
  ot_lfree char *pack_checksum = NULL;

  if (!list_files_in_dir_matching (cache_path,
                                   prefix, suffix,
                                   &current_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < current_files->len; i++)
    {
      GFile *file = current_files->pdata[i];
      
      g_free (pack_checksum);
      pack_checksum = get_checksum_from_pack_name (ot_gfile_get_basename_cached (file));
      
      if (!g_hash_table_lookup (new_files, pack_checksum))
        {
          if (!ot_gfile_unlink (file, cancellable, error))
            goto out;
        }
      
      if (inout_cached)
        {
          g_ptr_array_add (inout_cached, pack_checksum);
          pack_checksum = NULL; /* transfer ownership */
        }
    }
  ret = TRUE;
 out:
  return ret;
}

static void
gather_uncached (GHashTable   *new_files,
                 GPtrArray    *cached,
                 GPtrArray    *inout_uncached)
{
  guint i;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, new_files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *cur_pack_checksum = key;
      gboolean found = FALSE;

      for (i = 0; i < cached->len; i++)
        {
          const char *checksum = cached->pdata[i];
          if (strcmp (cur_pack_checksum, checksum) == 0)
            {
              found = TRUE;
              break;
            }
        }
      
      if (!found)
        g_ptr_array_add (inout_uncached, g_strdup (cur_pack_checksum));
    }
}

/**
 * Take a pack superindex file @superindex_path, and clean up any
 * no-longer-referenced pack files in the lookaside cache for
 * @remote_name.  The updated index file will also be saved into the
 * cache.
 *
 * Upon successful return, @out_cached_indexes will hold checksum
 * strings for indexes which are already in the cache, and
 * @out_uncached_indexes will hold strings for those which are not.
 */
gboolean
ostree_repo_resync_cached_remote_pack_indexes (OstreeRepo       *self,
                                               const char       *remote_name,
                                               GFile            *superindex_path,
                                               GPtrArray       **out_cached_meta_indexes,
                                               GPtrArray       **out_cached_data_indexes,
                                               GPtrArray       **out_uncached_meta_indexes,
                                               GPtrArray       **out_uncached_data_indexes,
                                               GCancellable     *cancellable,
                                               GError          **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *superindex_variant = NULL;
  ot_lobj GFile *cache_path = NULL;
  ot_lobj GFile *superindex_cache_path = NULL;
  ot_lptrarray GPtrArray *meta_index_files = NULL;
  ot_lptrarray GPtrArray *data_index_files = NULL;
  ot_lptrarray GPtrArray *meta_data_files = NULL;
  ot_lptrarray GPtrArray *data_data_files = NULL;
  ot_lhash GHashTable *new_pack_meta_indexes = NULL;
  ot_lhash GHashTable *new_pack_data_indexes = NULL;
  ot_lptrarray GPtrArray *ret_cached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_cached_data_indexes = NULL;
  ot_lptrarray GPtrArray *ret_uncached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_uncached_data_indexes = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lvariant GVariant *bloom = NULL;
  ot_lfree char *pack_checksum = NULL;
  GVariantIter *superindex_contents_iter = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_path, cancellable, error))
    goto out;

  ret_cached_meta_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_cached_data_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_uncached_meta_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_uncached_data_indexes = g_ptr_array_new_with_free_func (g_free);

  if (!ot_util_variant_map (superindex_path, OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT,
                            FALSE, &superindex_variant, error))
    goto out;

  if (!ostree_validate_structureof_pack_superindex (superindex_variant, error))
    goto out;

  new_pack_meta_indexes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  new_pack_data_indexes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_variant_get_child (superindex_variant, 2, "a(ayay)",
                       &superindex_contents_iter);
  while (g_variant_iter_loop (superindex_contents_iter,
                              "(@ay@ay)", &csum_bytes, &bloom))
    {
      pack_checksum = ostree_checksum_from_bytes_v (csum_bytes);
      g_hash_table_insert (new_pack_meta_indexes, pack_checksum, pack_checksum);
      pack_checksum = NULL; /* transfer ownership */
    }
  g_variant_iter_free (superindex_contents_iter);

  g_variant_get_child (superindex_variant, 3, "a(ayay)",
                       &superindex_contents_iter);
  while (g_variant_iter_loop (superindex_contents_iter,
                              "(@ay@ay)", &csum_bytes, &bloom))
    {
      pack_checksum = ostree_checksum_from_bytes_v (csum_bytes);
      g_hash_table_insert (new_pack_data_indexes, pack_checksum, pack_checksum);
      pack_checksum = NULL; /* transfer ownership */
    }

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostmetapack-", ".index",
                                    new_pack_meta_indexes,
                                    ret_cached_meta_indexes,
                                    cancellable, error))
    goto out;

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostdatapack-", ".index",
                                    new_pack_data_indexes,
                                    ret_cached_data_indexes,
                                    cancellable, error))
    goto out;

  gather_uncached (new_pack_meta_indexes, ret_cached_meta_indexes, ret_uncached_meta_indexes);
  gather_uncached (new_pack_data_indexes, ret_cached_data_indexes, ret_uncached_data_indexes);
  
  superindex_cache_path = g_file_get_child (cache_path, "index");
  if (!ot_util_variant_save (superindex_cache_path, superindex_variant, cancellable, error))
    goto out;

  /* Now also delete stale pack files */

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostmetapack-", ".data",
                                    new_pack_meta_indexes, NULL,
                                    cancellable, error))
    goto out;
  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostdatapack-", ".data",
                                    new_pack_data_indexes, NULL,
                                    cancellable, error))
    goto out;
      
  ret = TRUE;
  ot_transfer_out_value (out_cached_meta_indexes, &ret_cached_meta_indexes);
  ot_transfer_out_value (out_cached_data_indexes, &ret_cached_data_indexes);
  ot_transfer_out_value (out_uncached_meta_indexes, &ret_uncached_meta_indexes);
  ot_transfer_out_value (out_uncached_data_indexes, &ret_uncached_data_indexes);
 out:
  if (superindex_contents_iter)
    g_variant_iter_free (superindex_contents_iter);
  return ret;
}

gboolean
ostree_repo_clean_cached_remote_pack_data (OstreeRepo       *self,
                                           const char       *remote_name,
                                           GCancellable     *cancellable,
                                           GError          **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lobj GFile *cache_path = NULL;
  ot_lptrarray GPtrArray *data_files = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_path, cancellable, error))
    goto out;

  if (!list_files_in_dir_matching (cache_path,
                                   "ostmetapack-", ".data",
                                   &data_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < data_files->len; i++)
    {
      GFile *data_file = data_files->pdata[i];
      
      if (!ot_gfile_unlink (data_file, cancellable, error))
        goto out;
    }

  g_clear_pointer (&data_files, (GDestroyNotify) g_ptr_array_unref);
  if (!list_files_in_dir_matching (cache_path,
                                   "ostdatapack-", ".data",
                                   &data_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < data_files->len; i++)
    {
      GFile *data_file = data_files->pdata[i];
      
      if (!ot_gfile_unlink (data_file, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * Load the index for pack @pack_checksum from cache directory for
 * @remote_name.
 */
gboolean
ostree_repo_map_cached_remote_pack_index (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GVariant        **out_variant,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lobj GFile *cache_dir = NULL;
  ot_lobj GFile *cached_pack_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_dir,
                                cancellable, error))
    goto out;

  cached_pack_path = get_pack_index_path (cache_dir, is_meta, pack_checksum);
  if (!ot_util_variant_map (cached_pack_path, OSTREE_PACK_INDEX_VARIANT_FORMAT,
                            FALSE, &ret_variant, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * The variable @cached_path should refer to a file containing a pack
 * index.  It will be validated and added to the cache directory for
 * @remote_name.
 */
gboolean
ostree_repo_add_cached_remote_pack_index (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GFile            *cached_path,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cachedir = NULL;
  ot_lobj GFile *target_path = NULL;
  ot_lvariant GVariant *input_index_variant = NULL;
  ot_lvariant GVariant *output_index_variant = NULL;

  if (!map_variant_file_check_header_string (cached_path,
                                             OSTREE_PACK_INDEX_VARIANT_FORMAT,
                                             "OSTv0PACKINDEX",
                                             FALSE, &input_index_variant,
                                             cancellable, error))
    goto out;

  if (!ostree_validate_structureof_pack_index (input_index_variant, error))
    goto out;

  output_index_variant = g_variant_get_normal_form (input_index_variant);
  
  if (!ensure_remote_cache_dir (self, remote_name, &cachedir, cancellable, error))
    goto out;
  
  target_path = get_pack_index_path (cachedir, is_meta, pack_checksum);
  if (!ot_util_variant_save (target_path, output_index_variant, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * Check for availability of the pack index pointing to @pack_checksum
 * in the lookaside cache for @remote_name.  If not found, then the
 * output parameter @out_cached_path will be %NULL.
 */
gboolean
ostree_repo_get_cached_remote_pack_data (OstreeRepo       *self,
                                         const char       *remote_name,
                                         const char       *pack_checksum,
                                         gboolean          is_meta,
                                         GFile           **out_cached_path,
                                         GCancellable     *cancellable,
                                         GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cache_dir = NULL;
  ot_lobj GFile *cached_pack_path = NULL;
  ot_lobj GFile *ret_cached_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_dir,
                                cancellable, error))
    goto out;

  cached_pack_path = get_pack_data_path (cache_dir, is_meta, pack_checksum);
  if (g_file_query_exists (cached_pack_path, cancellable))
    {
      ret_cached_path = cached_pack_path;
      cached_pack_path = NULL;
    }

  ret = TRUE;
  ot_transfer_out_value (out_cached_path, &ret_cached_path);
 out:
  return ret;
}

/**
 * Add file @cached_path into the cache for given @remote_name.  If
 * @cached_path is %NULL, delete the cached pack data (if any).
 *
 * <note>
 *   This unlinks @cached_path.
 * </note>
 */
gboolean
ostree_repo_take_cached_remote_pack_data (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GFile            *cached_path,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cachedir = NULL;
  ot_lobj GFile *target_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cachedir, cancellable, error))
    goto out;

  target_path = get_pack_data_path (cachedir, is_meta, pack_checksum);
  if (cached_path)
    {
      if (!ot_gfile_rename (cached_path, target_path, cancellable, error))
        goto out;
    }
  else
    {
      (void) ot_gfile_unlink (target_path, cancellable, NULL);
    }

  ret = TRUE;
 out:
  return ret;
}

static GVariant *
create_tree_variant_from_hashes (GHashTable            *file_checksums,
                                 GHashTable            *dir_contents_checksums,
                                 GHashTable            *dir_metadata_checksums)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  GVariant *serialized_tree;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(say)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sayay)"));

  g_hash_table_iter_init (&hash_iter, file_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(s@ay)", name,
                             ostree_checksum_to_bytes_v (value));
    }
  
  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, dir_metadata_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *content_checksum;
      const char *meta_checksum;

      content_checksum = g_hash_table_lookup (dir_contents_checksums, name);
      meta_checksum = g_hash_table_lookup (dir_metadata_checksums, name);

      g_variant_builder_add (&dirs_builder, "(s@ay@ay)",
                             name,
                             ostree_checksum_to_bytes_v (content_checksum),
                             ostree_checksum_to_bytes_v (meta_checksum));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(@a(say)@a(sayay))",
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  g_variant_ref_sink (serialized_tree);

  return serialized_tree;
}

static GFileInfo *
create_modified_file_info (GFileInfo               *info,
                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *ret;

  if (!modifier)
    return (GFileInfo*)g_object_ref (info);

  ret = g_file_info_dup (info);
  
  return ret;
}

static OstreeRepoCommitFilterResult
apply_commit_filter (OstreeRepo            *self,
                     OstreeRepoCommitModifier *modifier,
                     GPtrArray                *path,
                     GFileInfo                *file_info,
                     GFileInfo               **out_modified_info)
{
  GString *path_buf;
  guint i;
  OstreeRepoCommitFilterResult result;
  GFileInfo *modified_info;
  
  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];
          
          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  modified_info = g_file_info_dup (file_info);
  result = modifier->filter (self, path_buf->str, modified_info, modifier->user_data);
  *out_modified_info = modified_info;

  g_string_free (path_buf, TRUE);
  return result;
}

static gboolean
stage_directory_to_mtree_internal (OstreeRepo           *self,
                                   GFile                *dir,
                                   OstreeMutableTree    *mtree,
                                   OstreeRepoCommitModifier *modifier,
                                   GPtrArray             *path,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean repo_dir_was_empty = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  ot_lobj OstreeRepoFile *repo_dir = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *child_info = NULL;

  /* We can only reuse checksums directly if there's no modifier */
  if (OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile*)g_object_ref (dir);

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        goto out;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_get_checksum (repo_dir));
      repo_dir_was_empty = 
        g_hash_table_size (ostree_mutable_tree_get_files (mtree)) == 0
        && g_hash_table_size (ostree_mutable_tree_get_subdirs (mtree)) == 0;

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      ot_lobj GFileInfo *modified_info = NULL;
      ot_lvariant GVariant *xattrs = NULL;
      ot_lfree guchar *child_file_csum = NULL;
      ot_lfree char *tmp_checksum = NULL;

      child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, error);
      if (!child_info)
        goto out;
      
      filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!(modifier && modifier->skip_xattrs))
            {
              if (!ostree_get_xattrs_for_file (dir, &xattrs, cancellable, error))
                goto out;
            }
          
          if (!stage_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                     cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }

      g_clear_object (&child_info);
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, 
                                            error);
      if (!dir_enum)
        goto out;

      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
        {
          ot_lobj GFileInfo *modified_info = NULL;
          ot_lobj GFile *child = NULL;
          ot_lobj OstreeMutableTree *child_mtree = NULL;
          const char *name = g_file_info_get_name (child_info);

          g_ptr_array_add (path, (char*)name);
          filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

          if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
            {
              child = g_file_get_child (dir, name);

              if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
                    goto out;

                  if (!stage_directory_to_mtree_internal (self, child, child_mtree,
                                                          modifier, path, cancellable, error))
                    goto out;
                }
              else if (repo_dir)
                {
                  if (!ostree_mutable_tree_replace_file (mtree, name, 
                                                         ostree_repo_file_get_checksum ((OstreeRepoFile*) child),
                                                         error))
                    goto out;
                }
              else
                {
                  guint64 file_obj_length;
                  const char *loose_checksum;
                  ot_lobj GInputStream *file_input = NULL;
                  ot_lvariant GVariant *xattrs = NULL;
                  ot_lobj GInputStream *file_object_input = NULL;
                  ot_lfree guchar *child_file_csum = NULL;
                  ot_lfree char *tmp_checksum = NULL;

                  loose_checksum = devino_cache_lookup (self, child_info);

                  if (loose_checksum)
                    {
                      if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum,
                                                             error))
                        goto out;
                    }
                  else
                    {
                     if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
                        {
                          file_input = (GInputStream*)g_file_read (child, cancellable, error);
                          if (!file_input)
                            goto out;
                        }

                      if (!(modifier && modifier->skip_xattrs))
                        {
                          g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);
                          if (!ostree_get_xattrs_for_file (child, &xattrs, cancellable, error))
                            goto out;
                        }

                      if (!ostree_raw_file_to_content_stream (file_input,
                                                              modified_info, xattrs,
                                                              &file_object_input, &file_obj_length,
                                                              cancellable, error))
                        goto out;
                      if (!stage_object (self, OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID,
                                         OSTREE_OBJECT_TYPE_FILE, file_object_input, file_obj_length,
                                         NULL, &child_file_csum, cancellable, error))
                        goto out;

                      g_free (tmp_checksum);
                      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
                      if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                             error))
                        goto out;
                    }
                }

              g_ptr_array_remove_index (path, path->len - 1);
            }

          g_clear_object (&child_info);
        }
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  if (repo_dir && repo_dir_was_empty)
    ostree_mutable_tree_set_contents_checksum (mtree, ostree_repo_file_tree_get_content_checksum (repo_dir));

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_directory_to_mtree (OstreeRepo           *self,
                                      GFile                *dir,
                                      OstreeMutableTree    *mtree,
                                      OstreeRepoCommitModifier *modifier,
                                      GCancellable         *cancellable,
                                      GError              **error)
{
  gboolean ret = FALSE;
  GPtrArray *path = NULL;

  path = g_ptr_array_new ();
  if (!stage_directory_to_mtree_internal (self, dir, mtree, modifier, path, cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (path)
    g_ptr_array_free (path, TRUE);
  return ret;
}

gboolean
ostree_repo_stage_mtree (OstreeRepo           *self,
                         OstreeMutableTree    *mtree,
                         char                **out_contents_checksum,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  const char *existing_checksum;
  ot_lfree char *ret_contents_checksum = NULL;
  ot_lhash GHashTable *dir_metadata_checksums = NULL;
  ot_lhash GHashTable *dir_contents_checksums = NULL;
  ot_lvariant GVariant *serialized_tree = NULL;
  ot_lfree guchar *contents_csum = NULL;

  existing_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (existing_checksum)
    {
      ret_contents_checksum = g_strdup (existing_checksum);
    }
  else
    {
      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      
      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          const char *metadata_checksum;
          OstreeMutableTree *child_dir = value;
          char *child_dir_contents_checksum;

          if (!ostree_repo_stage_mtree (self, child_dir, &child_dir_contents_checksum,
                                        cancellable, error))
            goto out;
      
          g_assert (child_dir_contents_checksum);
          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                child_dir_contents_checksum); /* Transfer ownership */
          metadata_checksum = ostree_mutable_tree_get_metadata_checksum (child_dir);
          g_assert (metadata_checksum);
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (metadata_checksum));
        }
    
      serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (mtree),
                                                         dir_contents_checksums,
                                                         dir_metadata_checksums);
      
      if (!stage_metadata_object (self, OSTREE_OBJECT_TYPE_DIR_TREE,
                                  serialized_tree, &contents_csum,
                                  cancellable, error))
        goto out;
      ret_contents_checksum = ostree_checksum_from_bytes (contents_csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
 out:
  return ret;
}

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static GFileInfo *
file_info_from_archive_entry_and_modifier (struct archive_entry  *entry,
                                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *info = g_file_info_new ();
  GFileInfo *modified_info = NULL;
  const struct stat *st;
  guint32 file_type;

  st = archive_entry_stat (entry);

  file_type = ot_gfile_type_for_mode (st->st_mode);
  g_file_info_set_attribute_boolean (info, "standard::is-symlink", S_ISLNK (st->st_mode));
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);
  g_file_info_set_attribute_uint32 (info, "unix::uid", st->st_uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", st->st_gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", st->st_mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", st->st_size);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", archive_entry_symlink (entry));
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      g_file_info_set_attribute_uint32 (info, "unix::rdev", st->st_rdev);
    }

  modified_info = create_modified_file_info (info, modifier);

  g_object_unref (info);
  
  return modified_info;
}

static gboolean
import_libarchive_entry_file (OstreeRepo           *self,
                              struct archive       *a,
                              struct archive_entry *entry,
                              GFileInfo            *file_info,
                              guchar              **out_csum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lobj GInputStream *archive_stream = NULL;
  guint64 length;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = ostree_libarchive_input_stream_new (a);
  
  if (!ostree_raw_file_to_content_stream (archive_stream, file_info, NULL,
                                          &file_object_input, &length, cancellable, error))
    goto out;
  
  if (!stage_object (self, OSTREE_REPO_STAGE_FLAGS_LENGTH_VALID, OSTREE_OBJECT_TYPE_FILE,
                     file_object_input, length, NULL, out_csum,
                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_libarchive_entry_to_mtree (OstreeRepo           *self,
                                 OstreeMutableTree    *root,
                                 struct archive       *a,
                                 struct archive_entry *entry,
                                 OstreeRepoCommitModifier *modifier,
                                 const guchar         *tmp_dir_csum,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  gboolean ret = FALSE;
  const char *pathname;
  const char *hardlink;
  const char *basename;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lptrarray GPtrArray *split_path = NULL;
  ot_lptrarray GPtrArray *hardlink_split_path = NULL;
  ot_lobj OstreeMutableTree *subdir = NULL;
  ot_lobj OstreeMutableTree *parent = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_parent = NULL;
  ot_lfree char *hardlink_source_checksum = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_subdir = NULL;
  ot_lfree guchar *tmp_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;

  pathname = archive_entry_pathname (entry); 
      
  if (!ot_util_path_split_validate (pathname, &split_path, error))
    goto out;

  if (split_path->len == 0)
    {
      parent = NULL;
      basename = NULL;
    }
  else
    {
      if (tmp_dir_csum)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_dir_csum);
          if (!ostree_mutable_tree_ensure_parent_dirs (root, split_path,
                                                       tmp_checksum,
                                                       &parent,
                                                       error))
            goto out;
        }
      else
        {
          if (!ostree_mutable_tree_walk (root, split_path, 0, &parent, error))
            goto out;
        }
      basename = (char*)split_path->pdata[split_path->len-1];
    }

  hardlink = archive_entry_hardlink (entry);
  if (hardlink)
    {
      const char *hardlink_basename;
      
      g_assert (parent != NULL);

      if (!ot_util_path_split_validate (hardlink, &hardlink_split_path, error))
        goto out;
      if (hardlink_split_path->len == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid hardlink path %s", hardlink);
          goto out;
        }
      
      hardlink_basename = hardlink_split_path->pdata[hardlink_split_path->len - 1];
      
      if (!ostree_mutable_tree_walk (root, hardlink_split_path, 0, &hardlink_source_parent, error))
        goto out;
      
      if (!ostree_mutable_tree_lookup (hardlink_source_parent, hardlink_basename,
                                       &hardlink_source_checksum,
                                       &hardlink_source_subdir,
                                       error))
        {
              g_prefix_error (error, "While resolving hardlink target: ");
              goto out;
        }
      
      if (hardlink_source_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Hardlink %s refers to directory %s",
                       pathname, hardlink);
          goto out;
        }
      g_assert (hardlink_source_checksum);
      
      if (!ostree_mutable_tree_replace_file (parent,
                                             basename,
                                             hardlink_source_checksum,
                                             error))
        goto out;
    }
  else
    {
      file_info = file_info_from_archive_entry_and_modifier (entry, modifier);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_UNKNOWN)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file for import: %s", pathname);
          goto out;
        }

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {

          if (!stage_directory_meta (self, file_info, NULL, &tmp_csum, cancellable, error))
            goto out;

          if (parent == NULL)
            {
              subdir = g_object_ref (root);
            }
          else
            {
              if (!ostree_mutable_tree_ensure_dir (parent, basename, &subdir, error))
                goto out;
            }

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          ostree_mutable_tree_set_metadata_checksum (subdir, tmp_checksum);
        }
      else 
        {
          if (parent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't import file as root");
              goto out;
            }

          if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_csum, cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          if (!ostree_mutable_tree_replace_file (parent, basename,
                                                 tmp_checksum,
                                                 error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
#endif
                          
gboolean
ostree_repo_stage_archive_to_mtree (OstreeRepo                *self,
                                    GFile                     *archive_f,
                                    OstreeMutableTree         *root,
                                    OstreeRepoCommitModifier  *modifier,
                                    gboolean                   autocreate_parents,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = NULL;
  struct archive_entry *entry;
  int r;
  ot_lobj GFileInfo *tmp_dir_info = NULL;
  ot_lfree guchar *tmp_csum = NULL;

  a = archive_read_new ();
  archive_read_support_compression_all (a);
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, ot_gfile_get_path_cached (archive_f), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  while (TRUE)
    {
      r = archive_read_next_header (a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      else if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }

      if (autocreate_parents && !tmp_csum)
        {
          tmp_dir_info = g_file_info_new ();
          
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::uid", archive_entry_uid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::gid", archive_entry_gid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::mode", 0755 | S_IFDIR);
          
          if (!stage_directory_meta (self, tmp_dir_info, NULL, &tmp_csum, cancellable, error))
            goto out;
        }

      if (!stage_libarchive_entry_to_mtree (self, root, a,
                                            entry, modifier,
                                            autocreate_parents ? tmp_csum : NULL,
                                            cancellable, error))
        goto out;
    }
  if (archive_read_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  if (a)
    (void)archive_read_close (a);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (void)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);

  modifier->refcount = 1;

  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  g_free (modifier);
  return;
}

static gboolean
list_loose_object_dir (OstreeRepo             *self,
                       GFile                  *dir,
                       GHashTable             *inout_objects,
                       GCancellable           *cancellable,
                       GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  const char *dirname = NULL;
  const char *dot = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  GString *checksum = NULL;

  dirname = ot_gfile_get_basename_cached (dir);

  /* We're only querying name */
  enumerator = g_file_enumerate_children (dir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      OstreeObjectType objtype;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        goto loop_next;
      
      if (g_str_has_suffix (name, ".file"))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (g_str_has_suffix (name, ".dirtree"))
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (g_str_has_suffix (name, ".dirmeta"))
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (g_str_has_suffix (name, ".commit"))
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        goto loop_next;
          
      dot = strrchr (name, '.');
      g_assert (dot);

      if ((dot - name) == 62)
        {
          GVariant *key, *value;

          if (checksum)
            g_string_free (checksum, TRUE);
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = ostree_object_name_serialize (checksum->str, objtype);
          value = g_variant_new ("(b@as)",
                                 TRUE, g_variant_new_strv (NULL, 0));
          /* transfer ownership */
          g_hash_table_replace (inout_objects, key,
                                g_variant_ref_sink (value));
        }
    loop_next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  if (checksum)
    g_string_free (checksum, TRUE);
  return ret;
}

static gboolean
list_loose_objects (OstreeRepo                     *self,
                    GHashTable                     *inout_objects,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *object_dirs = NULL;
  ot_lobj GFile *objdir = NULL;

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      if (!list_loose_object_dir (self, objdir, inout_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_load_pack_index (OstreeRepo    *self,
                             const char    *pack_checksum, 
                             gboolean       is_meta,
                             GVariant     **out_variant,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lobj GFile *path = NULL;
  
  g_mutex_lock (&self->cache_lock);

  ret_variant = g_hash_table_lookup (self->cached_pack_index_mappings, pack_checksum);
  if (ret_variant)
    {
      g_variant_ref (ret_variant);
    }
  else
    {
      path = get_pack_index_path (self->pack_dir, is_meta, pack_checksum);
      if (!map_variant_file_check_header_string (path,
                                                 OSTREE_PACK_INDEX_VARIANT_FORMAT,
                                                 "OSTv0PACKINDEX", TRUE,
                                                 &ret_variant,
                                                 cancellable, error))
        goto out;
      g_hash_table_insert (self->cached_pack_index_mappings, g_strdup (pack_checksum),
                           g_variant_ref (ret_variant));
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  g_mutex_unlock (&self->cache_lock);
  return ret;
}

/**
 * @sha256: Checksum of pack file
 * @out_data: (out): Pointer to pack file data
 *
 * Ensure that the given pack file is mapped into
 * memory.
 */
gboolean
ostree_repo_map_pack_file (OstreeRepo    *self,
                           const char    *pack_checksum,
                           gboolean       is_meta,
                           guchar       **out_data,
                           guint64       *out_len,
                           GCancellable  *cancellable,
                           GError       **error)
{
  gboolean ret = FALSE;
  gpointer ret_data;
  guint64 ret_len;
  GMappedFile *map = NULL;
  ot_lobj GFile *path = NULL;

  g_mutex_lock (&self->cache_lock);

  map = g_hash_table_lookup (self->cached_pack_data_mappings, pack_checksum);
  if (map == NULL)
    {
      path = get_pack_data_path (self->pack_dir, is_meta, pack_checksum);

      map = g_mapped_file_new (ot_gfile_get_path_cached (path), FALSE, error);
      if (!map)
        goto out;

      g_hash_table_insert (self->cached_pack_data_mappings, g_strdup (pack_checksum), map);
      ret_data = g_mapped_file_get_contents (map);
    }

  ret_data = g_mapped_file_get_contents (map);
  ret_len = (guint64)g_mapped_file_get_length (map);

  ret = TRUE;
  if (out_data)
    *out_data = ret_data;
  if (out_len)
    *out_len = ret_len;
 out:
  g_mutex_unlock (&self->cache_lock);
  return ret;
}

gboolean
ostree_repo_load_file (OstreeRepo         *self,
                       const char         *checksum,
                       GInputStream      **out_input,
                       GFileInfo         **out_file_info,
                       GVariant          **out_xattrs,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  guchar *pack_data;
  guint64 pack_len;
  guint64 pack_offset;
  ot_lvariant GVariant *packed_object = NULL;
  ot_lvariant GVariant *file_data = NULL;
  ot_lobj GFile *loose_path = NULL;
  ot_lobj GFileInfo *content_loose_info = NULL;
  ot_lfree char *pack_checksum = NULL;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFileInfo *ret_file_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;

  if (!repo_find_object (self, OSTREE_OBJECT_TYPE_FILE,
                         checksum, FALSE, &loose_path,
                         &pack_checksum, &pack_offset,
                         cancellable, error))
    goto out;

  if (loose_path)
    {
      if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE)
        {
          ot_lvariant GVariant *archive_meta = NULL;

          if (!ot_util_variant_map (loose_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT,
                                    TRUE, &archive_meta, error))
            goto out;

          if (!ostree_file_header_parse (archive_meta, &ret_file_info, &ret_xattrs,
                                         error))
            goto out;

          if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
            {
              ot_lobj GFile *archive_content_path = NULL;
              ot_lobj GFileInfo *content_info = NULL;

              archive_content_path = ostree_repo_get_archive_content_path (self, checksum);
              content_info = g_file_query_info (archive_content_path, OSTREE_GIO_FAST_QUERYINFO,
                                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
              if (!content_info)
                goto out;

              if (out_input)
                {
                  ret_input = (GInputStream*)g_file_read (archive_content_path, cancellable, error);
                  if (!ret_input)
                    goto out;
                }
              g_file_info_set_size (ret_file_info, g_file_info_get_size (content_info));
            }
        }
      else
        {
          ret_file_info = g_file_query_info (loose_path, OSTREE_GIO_FAST_QUERYINFO,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             cancellable, error);
          if (!ret_file_info)
            goto out;

          if (out_xattrs)
            {
              if (!ostree_get_xattrs_for_file (loose_path, &ret_xattrs,
                                               cancellable, error))
                goto out;
            }

          if (out_input && g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
            {
              ret_input = (GInputStream*) g_file_read (loose_path, cancellable, error);
              if (!ret_input)
                {
                  g_prefix_error (error, "Error opening loose file object %s: ", ot_gfile_get_path_cached (loose_path));
                  goto out;
                }
            }
        }
    }
  else if (pack_checksum)
    {
      if (!ostree_repo_map_pack_file (self, pack_checksum, FALSE,
                                      &pack_data, &pack_len,
                                      cancellable, error))
        goto out;

      if (!ostree_read_pack_entry_raw (pack_data, pack_len,
                                       pack_offset, TRUE, FALSE,
                                       &packed_object, cancellable, error))
        goto out;

      if (!ostree_parse_file_pack_entry (packed_object,
                                         out_input ? &ret_input : NULL,
                                         out_file_info ? &ret_file_info : NULL,
                                         out_xattrs ? &ret_xattrs : NULL,
                                         cancellable, error))
        goto out;
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_file (self->parent_repo, checksum, 
                                  out_input ? &ret_input : NULL,
                                  out_file_info ? &ret_file_info : NULL,
                                  out_xattrs ? &ret_xattrs : NULL,
                                  cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find file object '%s'", checksum);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

static gboolean
list_objects_in_index (OstreeRepo                     *self,
                       const char                     *pack_checksum,
                       gboolean                        is_meta,
                       GHashTable                     *inout_objects,
                       GCancellable                   *cancellable,
                       GError                        **error)
{
  gboolean ret = FALSE;
  guint8 objtype_u8;
  guint64 offset;
  ot_lobj GFile *index_path = NULL;
  ot_lvariant GVariant *index_variant = NULL;
  ot_lvariant GVariant *contents = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lfree char *checksum = NULL;
  GVariantIter content_iter;

  index_path = get_pack_index_path (self->pack_dir, is_meta, pack_checksum);

  if (!ostree_repo_load_pack_index (self, pack_checksum, is_meta, 
                                    &index_variant, cancellable, error))
    goto out;

  contents = g_variant_get_child_value (index_variant, 2);
  g_variant_iter_init (&content_iter, contents);

  while (g_variant_iter_loop (&content_iter, "(y@ayt)", &objtype_u8, &csum_bytes, &offset))
    {
      GVariant *obj_key;
      GVariant *objdata;
      OstreeObjectType objtype;
      GVariantBuilder pack_contents_builder;
      gboolean is_loose;

      objtype = (OstreeObjectType) objtype_u8;
      offset = GUINT64_FROM_BE (offset);

      g_variant_builder_init (&pack_contents_builder,
                              G_VARIANT_TYPE_STRING_ARRAY);
      
      g_free (checksum);
      checksum = ostree_checksum_from_bytes_v (csum_bytes);
      obj_key = ostree_object_name_serialize (checksum, objtype);
      ot_util_variant_take_ref (obj_key);

      objdata = g_hash_table_lookup (inout_objects, obj_key);
      if (!objdata)
        {
          is_loose = FALSE;
        }
      else
        {
          GVariantIter *current_packs_iter;
          const char *current_pack_checksum;

          g_variant_get (objdata, "(bas)", &is_loose, &current_packs_iter);

          while (g_variant_iter_loop (current_packs_iter, "&s", &current_pack_checksum))
            {
              g_variant_builder_add (&pack_contents_builder, "s", current_pack_checksum);
            }
          g_variant_iter_free (current_packs_iter);
        }
      g_variant_builder_add (&pack_contents_builder, "s", pack_checksum);
      objdata = g_variant_new ("(b@as)", is_loose,
                               g_variant_builder_end (&pack_contents_builder));
      g_variant_ref_sink (objdata);
      g_hash_table_replace (inout_objects, obj_key, objdata);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
list_packed_objects (OstreeRepo                     *self,
                     GHashTable                     *inout_objects,
                     GCancellable                   *cancellable,
                     GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *meta_index_checksums = NULL;
  ot_lptrarray GPtrArray *data_index_checksums = NULL;

  if (!ostree_repo_list_pack_indexes (self, &meta_index_checksums, &data_index_checksums,
                                      cancellable, error))
    goto out;

  for (i = 0; i < meta_index_checksums->len; i++)
    {
      const char *checksum = meta_index_checksums->pdata[i];
      if (!list_objects_in_index (self, checksum, TRUE, inout_objects, cancellable, error))
        goto out;
    }
  
  for (i = 0; i < data_index_checksums->len; i++)
    {
      const char *checksum = data_index_checksums->pdata[i];
      if (!list_objects_in_index (self, checksum, FALSE, inout_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_object_in_packs (OstreeRepo        *self,
                      const char        *checksum,
                      OstreeObjectType   objtype,
                      char             **out_pack_checksum,
                      guint64           *out_pack_offset,
                      GCancellable      *cancellable,
                      GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  guint64 ret_pack_offset = 0;
  gboolean is_meta;
  ot_lptrarray GPtrArray *index_checksums = NULL;
  ot_lfree char *ret_pack_checksum = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lvariant GVariant *index_variant = NULL;

  csum_bytes = ostree_checksum_to_bytes_v (checksum);

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);

  if (is_meta)
    {
      if (!ostree_repo_list_pack_indexes (self, &index_checksums, NULL,
                                          cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_list_pack_indexes (self, NULL, &index_checksums,
                                          cancellable, error))
        goto out;
    }

  for (i = 0; i < index_checksums->len; i++)
    {
      const char *pack_checksum = index_checksums->pdata[i];
      guint64 offset;

      g_clear_pointer (&index_variant, (GDestroyNotify) g_variant_unref);
      if (!ostree_repo_load_pack_index (self, pack_checksum, is_meta, &index_variant,
                                        cancellable, error))
        goto out;

      if (!ostree_pack_index_search (index_variant, csum_bytes, objtype, &offset))
        continue;

      ret_pack_checksum = g_strdup (pack_checksum);
      ret_pack_offset = offset;
      break;
    }

  ret = TRUE;
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_pack_offset)
    *out_pack_offset = ret_pack_offset;
 out:
  return ret;
}

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  gboolean              lookup_all,
                  GFile               **out_stored_path,
                  char                **out_pack_checksum,
                  guint64              *out_pack_offset,
                  GCancellable         *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  guint64 ret_pack_offset = 0;
  struct stat stbuf;
  ot_lobj GFile *object_path = NULL;
  ot_lobj GFile *ret_stored_path = NULL;
  ot_lfree char *ret_pack_checksum = NULL;

  /* Look up metadata in packs first, but content loose first.  We
   * want to find loose content since that's preferable for
   * hardlinking scenarios.
   *
   * Metadata is much more efficient packed.
   */
  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (out_pack_checksum)
        {
          if (!find_object_in_packs (self, checksum, objtype,
                                     &ret_pack_checksum, &ret_pack_offset,
                                     cancellable, error))
            goto out;
        }
      if (!ret_pack_checksum || lookup_all)
        {
          object_path = ostree_repo_get_object_path (self, checksum, objtype);
  
          if (lstat (ot_gfile_get_path_cached (object_path), &stbuf) == 0)
            {
              ret_stored_path = object_path;
              object_path = NULL;
            }
          else
            {
              g_clear_object (&object_path);
            }
        }
    }
  else
    {
      if (out_stored_path)
        {
          object_path = ostree_repo_get_object_path (self, checksum, objtype);
  
          if (lstat (ot_gfile_get_path_cached (object_path), &stbuf) == 0)
            {
              ret_stored_path = object_path;
              object_path = NULL;
            }
          else
            {
              g_clear_object (&object_path);
            }
        }
      if (!ret_stored_path || lookup_all)
        {
          if (out_pack_checksum)
            {
              if (!find_object_in_packs (self, checksum, objtype,
                                         &ret_pack_checksum, &ret_pack_offset,
                                         cancellable, error))
                goto out;
            }
        }
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_stored_path, &ret_stored_path);
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_pack_offset)
    *out_pack_offset = ret_pack_offset;
out:
  return ret;
}

gboolean
ostree_repo_has_object (OstreeRepo           *self,
                        OstreeObjectType      objtype,
                        const char           *checksum,
                        gboolean             *out_have_object,
                        GCancellable         *cancellable,
                        GError              **error)
{
  gboolean ret = FALSE;
  gboolean ret_have_object;
  ot_lobj GFile *loose_path = NULL;
  ot_lfree char *pack_checksum = NULL;

  if (!repo_find_object (self, objtype, checksum, FALSE,
                         &loose_path,
                         &pack_checksum, NULL,
                         cancellable, error))
    goto out;

  ret_have_object = (loose_path != NULL) || (pack_checksum != NULL);

  if (!ret_have_object && self->parent_repo)
    {
      if (!ostree_repo_has_object (self->parent_repo, objtype, checksum,
                                   &ret_have_object, cancellable, error))
        goto out;
    }
                                
  ret = TRUE;
  if (out_have_object)
    *out_have_object = ret_have_object;
 out:
  return ret;
}

gboolean
ostree_repo_load_variant_c (OstreeRepo          *self,
                            OstreeObjectType     objtype,
                            const guchar        *csum, 
                            GVariant           **out_variant,
                            GError             **error)
{
  gboolean ret = FALSE;
  ot_lfree char *checksum = NULL;

  checksum = ostree_checksum_from_bytes (csum);

  if (!ostree_repo_load_variant (self, objtype, checksum, out_variant, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_load_variant (OstreeRepo  *self,
                          OstreeObjectType  objtype,
                          const char    *sha256, 
                          GVariant     **out_variant,
                          GError       **error)
{
  gboolean ret = FALSE;
  guchar *pack_data;
  guint64 pack_len;
  guint64 object_offset;
  GCancellable *cancellable = NULL;
  ot_lobj GFile *object_path = NULL;
  ot_lvariant GVariant *packed_object = NULL;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lfree char *pack_checksum = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);

  if (!repo_find_object (self, objtype, sha256, FALSE,
                         &object_path, &pack_checksum, &object_offset,
                         cancellable, error))
    goto out;

  if (pack_checksum != NULL)
    {
      if (!ostree_repo_map_pack_file (self, pack_checksum, TRUE, &pack_data, &pack_len,
                                      cancellable, error))
        goto out;
      
      if (!ostree_read_pack_entry_raw (pack_data, pack_len, object_offset,
                                       TRUE, TRUE, &packed_object, cancellable, error))
        goto out;

      g_variant_get_child (packed_object, 2, "v", &ret_variant);
    }
  else if (object_path != NULL)
    {
      if (!ot_util_variant_map (object_path, ostree_metadata_variant_type (objtype),
                                TRUE, &ret_variant, error))
        goto out;
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_variant (self->parent_repo, objtype, sha256, &ret_variant, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such metadata object %s.%s",
                   sha256, ostree_object_type_to_string (objtype));
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * ostree_repo_list_objects:
 * @self:
 * @flags:
 * @out_objects: (out): Map of serialized object name to variant data
 * @cancellable:
 * @error:
 *
 * This function synchronously enumerates all objects in the
 * repository, returning data in @out_objects.  @out_objects
 * maps from keys returned by ostree_object_name_serialize()
 * to #GVariant values of type %OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */ 
gboolean
ostree_repo_list_objects (OstreeRepo                  *self,
                          OstreeRepoListObjectsFlags   flags,
                          GHashTable                 **out_objects,
                          GCancellable                *cancellable,
                          GError                     **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_objects = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);
  
  ret_objects = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                       (GDestroyNotify) g_variant_unref,
                                       (GDestroyNotify) g_variant_unref);

  if (flags & OSTREE_REPO_LIST_OBJECTS_ALL)
    flags |= (OSTREE_REPO_LIST_OBJECTS_LOOSE | OSTREE_REPO_LIST_OBJECTS_PACKED);

  if (flags & OSTREE_REPO_LIST_OBJECTS_LOOSE)
    {
      if (!list_loose_objects (self, ret_objects, cancellable, error))
        goto out;
      if (self->parent_repo)
        {
          if (!list_loose_objects (self->parent_repo, ret_objects, cancellable, error))
            goto out;
        }
    }

  if (flags & OSTREE_REPO_LIST_OBJECTS_PACKED)
    {
      if (!list_packed_objects (self, ret_objects, cancellable, error))
        goto out;
      if (self->parent_repo)
        {
          if (!list_packed_objects (self->parent_repo, ret_objects, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_objects, &ret_objects);
 out:
  return ret;
}

static gboolean
checkout_file_from_input (GFile          *file,
                          OstreeRepoCheckoutMode mode,
                          OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                          GFileInfo      *finfo,
                          GVariant       *xattrs,
                          GInputStream   *input,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFile *dir = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFileInfo *temp_info = NULL;

  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      temp_info = g_file_info_dup (finfo);
      
      g_file_info_set_attribute_uint32 (temp_info, "unix::uid", geteuid ());
      g_file_info_set_attribute_uint32 (temp_info, "unix::gid", getegid ());

      xattrs = NULL;
    }

  if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    {
      if (g_file_info_get_file_type (temp_info ? temp_info : finfo) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ostree_create_file_from_input (file, temp_info ? temp_info : finfo,
                                              xattrs, input,
                                              cancellable, &temp_error))
            {
              if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_clear_error (&temp_error);
                }
              else
                {
                  g_propagate_error (error, temp_error);
                  goto out;
                }
            }
        }
      else
        {
          dir = g_file_get_parent (file);
          if (!ostree_create_temp_file_from_input (dir, NULL, "checkout",
                                                   temp_info ? temp_info : finfo,
                                                   xattrs, input, &temp_file, 
                                                   cancellable, error))
            goto out;
          
          if (rename (ot_gfile_get_path_cached (temp_file), ot_gfile_get_path_cached (file)) < 0)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }
  else
    {
      if (!ostree_create_file_from_input (file, temp_info ? temp_info : finfo,
                                          xattrs, input, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
checkout_file_hardlink (OstreeRepo                  *self,
                        OstreeRepoCheckoutMode    mode,
                        OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                        GFile                    *source,
                        GFile                    *destination,
                        gboolean                 *out_was_supported,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  gboolean ret = FALSE;
  gboolean ret_was_supported = FALSE;
  ot_lobj GFile *dir = NULL;

  if (link (ot_gfile_get_path_cached (source), ot_gfile_get_path_cached (destination)) != -1)
    ret_was_supported = TRUE;
  else if (errno == EMLINK || errno == EXDEV)
    {
      /* EMLINK and EXDEV shouldn't be fatal; we just can't do the
       * optimization of hardlinking instead of copying.
       */
      ret_was_supported = FALSE;
    }
  else if (errno == EEXIST && overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    { 
      /* Idiocy, from man rename(2)
       *
       * "If oldpath and newpath are existing hard links referring to
       * the same file, then rename() does nothing, and returns a
       * success status."
       *
       * So we can't make this atomic.  
       */
      (void) unlink (ot_gfile_get_path_cached (destination));
      if (link (ot_gfile_get_path_cached (source), ot_gfile_get_path_cached (destination)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
  if (out_was_supported)
    *out_was_supported = ret_was_supported;
 out:
  return ret;
}

static gboolean
find_loose_for_checkout (OstreeRepo             *self,
                         const char             *checksum,
                         GFile                 **out_loose_path,
                         GCancellable           *cancellable,
                         GError                **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *path = NULL;
  struct stat stbuf;

  do
    {
      if (self->mode == OSTREE_REPO_MODE_BARE)
        path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
      else
        path = ostree_repo_get_archive_content_path (self, checksum);

      if (lstat (ot_gfile_get_path_cached (path), &stbuf) < 0)
        {
          if (errno != ENOENT)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
          self = self->parent_repo;
        }
      else if (S_ISLNK (stbuf.st_mode))
        {
          /* Don't check out symbolic links via hardlink; it's very easy
           * to hit the maximum number of hardlinks to an inode this way,
           * especially since right now we have a lot of symbolic links to
           * busybox.
           *
           * fs/ext4/ext4.h:#define EXT4_LINK_MAX		65000
           */
          self = self->parent_repo;
        }
      else
        break;

      g_clear_object (&path);
    } while (self != NULL);

  ret = TRUE;
  ot_transfer_out_value (out_loose_path, &path);
 out:
  return ret;
}

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  GSimpleAsyncResult       *result;
} CheckoutOneFileAsyncData;

static void
checkout_file_async_data_free (gpointer      data)
{
  CheckoutOneFileAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  g_free (checkout_data);
}

static void
checkout_file_thread (GSimpleAsyncResult     *result,
                      GObject                *src,
                      GCancellable           *cancellable)
{
  const char *checksum;
  gboolean hardlink_supported;
  GError *local_error = NULL;
  GError **error = &local_error;
  ot_lobj GFile *loose_path = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_simple_async_result_get_op_res_gpointer (result);

  /* Hack to avoid trying to create device files as a user */
  if (checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && g_file_info_get_file_type (checkout_data->source_info) == G_FILE_TYPE_SPECIAL)
    goto out;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)checkout_data->source);

  if ((checkout_data->repo->mode == OSTREE_REPO_MODE_BARE
       && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_NONE)
      || (checkout_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE
          && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER))
    {
      if (!find_loose_for_checkout (checkout_data->repo, checksum, &loose_path,
                                    cancellable, error))
        goto out;
    }

  if (loose_path)
    {
      /* If we found one, try hardlinking */
      if (!checkout_file_hardlink (checkout_data->repo, checkout_data->mode,
                                   checkout_data->overwrite_mode, loose_path,
                                   checkout_data->destination,
                                   &hardlink_supported, cancellable, error))
        goto out;
    }

  /* Fall back to copy if there's no loose object, or we couldn't hardlink */
  if (loose_path == NULL || !hardlink_supported)
    {
      if (!ostree_repo_load_file (checkout_data->repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      if (!checkout_file_from_input (checkout_data->destination,
                                     checkout_data->mode,
                                     checkout_data->overwrite_mode,
                                     checkout_data->source_info, xattrs, 
                                     input, cancellable, error))
        goto out;
    }

 out:
  if (local_error)
    g_simple_async_result_take_error (result, local_error);
}

static void
checkout_one_file_async (OstreeRepo                  *self,
                         OstreeRepoCheckoutMode    mode,
                         OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                         OstreeRepoFile           *source,
                         GFileInfo                *source_info,
                         GFile                    *destination,
                         GCancellable             *cancellable,
                         GAsyncReadyCallback       callback,
                         gpointer                  user_data)
{
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_new0 (CheckoutOneFileAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     checkout_one_file_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_file_async_data_free);

  g_simple_async_result_run_in_thread (checkout_data->result,
                                       checkout_file_thread, G_PRIORITY_DEFAULT,
                                       cancellable);
  g_object_unref (checkout_data->result);
}

static gboolean
checkout_one_file_finish (OstreeRepo               *self,
                          GAsyncResult             *result,
                          GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, checkout_one_file_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  guint                     pending_ops;
  GMainLoop                *loop;
  GSimpleAsyncResult       *result;
} CheckoutTreeAsyncData;

static void
checkout_tree_async_data_free (gpointer      data)
{
  CheckoutTreeAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  g_free (checkout_data);
}

static void
on_tree_async_child_op_complete (CheckoutTreeAsyncData   *data,
                                 GError                  *local_error)
{
  data->pending_ops--;

  if (local_error)
    {
      if (!data->caught_error)
        {
          data->caught_error = TRUE;
          g_propagate_error (&data->error, local_error);
        }
      else
        g_clear_error (&local_error);
    }

  if (data->pending_ops != 0)
    return;

  if (data->caught_error)
    g_simple_async_result_take_error (data->result, data->error);
  g_simple_async_result_complete_in_idle (data->result);
  g_object_unref (data->result);
}

static void
on_one_subdir_checked_out (GObject          *src,
                           GAsyncResult     *result,
                           gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!ostree_repo_checkout_tree_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  on_tree_async_child_op_complete (data, local_error);
}

static void
on_one_file_checked_out (GObject          *src,
                         GAsyncResult     *result,
                         gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!checkout_one_file_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  on_tree_async_child_op_complete (data, local_error);
}

static void
on_got_next_files (GObject          *src,
                   GAsyncResult     *result,
                   gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;
  GList *files = NULL;
  GList *iter = NULL;

  files = g_file_enumerator_next_files_finish ((GFileEnumerator*) src, result, &local_error);
  if (local_error)
    goto out;

  if (files)
    {
      g_file_enumerator_next_files_async ((GFileEnumerator*)src, 50, G_PRIORITY_DEFAULT,
                                          data->cancellable,
                                          on_got_next_files, data);
      data->pending_ops++;
    }

  for (iter = files; iter; iter = iter->next)
    {
      GFileInfo *file_info = iter->data;
      const char *name;
      guint32 type;
      ot_lobj GFile *dest_path = NULL;
      ot_lobj GFile *src_child = NULL;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      dest_path = g_file_get_child (data->destination, name);
      src_child = g_file_get_child ((GFile*)data->source, name);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          ostree_repo_checkout_tree_async (data->repo,
                                           data->mode,
                                           data->overwrite_mode,
                                           dest_path, (OstreeRepoFile*)src_child, file_info,
                                           data->cancellable,
                                           on_one_subdir_checked_out,
                                           data);
        }
      else
        {
          checkout_one_file_async (data->repo, data->mode,
                                   data->overwrite_mode,
                                   (OstreeRepoFile*)src_child, file_info, 
                                   dest_path, data->cancellable,
                                   on_one_file_checked_out,
                                   data);
        }
      data->pending_ops++;
      g_object_unref (file_info);
    }
  g_list_free (files);

 out:
  on_tree_async_child_op_complete (data, local_error);
}

void
ostree_repo_checkout_tree_async (OstreeRepo               *self,
                                 OstreeRepoCheckoutMode    mode,
                                 OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                                 GFile                    *destination,
                                 OstreeRepoFile           *source,
                                 GFileInfo                *source_info,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  CheckoutTreeAsyncData *checkout_data;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  checkout_data = g_new0 (CheckoutTreeAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  checkout_data->pending_ops++; /* Count this function */

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     ostree_repo_checkout_tree_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_tree_async_data_free);

  if (!ostree_repo_file_get_xattrs (checkout_data->source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (checkout_data->destination,
                                 checkout_data->mode,
                                 checkout_data->overwrite_mode,
                                 checkout_data->source_info,
                                 xattrs, NULL,
                                 cancellable, error))
    goto out;

  g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);

  dir_enum = g_file_enumerate_children ((GFile*)checkout_data->source,
                                        OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  g_file_enumerator_next_files_async (dir_enum, 50, G_PRIORITY_DEFAULT, cancellable,
                                      on_got_next_files, checkout_data);
  checkout_data->pending_ops++;

 out:
  on_tree_async_child_op_complete (checkout_data, local_error);
}

gboolean
ostree_repo_checkout_tree_finish (OstreeRepo               *self,
                                  GAsyncResult             *result,
                                  GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_repo_checkout_tree_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

gboolean
ostree_repo_read_commit (OstreeRepo *self,
                         const char *rev, 
                         GFile       **out_root,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ret_root = NULL;
  ot_lfree char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = ostree_repo_file_new_root (self, resolved_rev);
  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, &ret_root);
 out:
  return ret;
}
