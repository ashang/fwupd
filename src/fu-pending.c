/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2018 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <fwupd.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <stdlib.h>

#include "fu-device-private.h"
#include "fu-pending.h"

static void fu_pending_finalize			 (GObject *object);

typedef struct {
	sqlite3				*db;
} FuPendingPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (FuPending, fu_pending, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (fu_pending_get_instance_private (o))

static gboolean
fu_pending_load (FuPending *pending, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	const char *statement;
	gint rc;
	g_autofree gchar *dirname = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(GFile) file = NULL;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);
	g_return_val_if_fail (priv->db == NULL, FALSE);

	/* create directory */
	dirname = g_build_filename (LOCALSTATEDIR, "lib", "fwupd", NULL);
	file = g_file_new_for_path (dirname);
	if (!g_file_query_exists (file, NULL)) {
		if (!g_file_make_directory_with_parents (file, NULL, error))
			return FALSE;
	}

	/* open */
	filename = g_build_filename (dirname, "pending.db", NULL);
	g_debug ("FuPending: trying to open database '%s'", filename);
	rc = sqlite3_open (filename, &priv->db);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "Can't open %s: %s",
			     filename, sqlite3_errmsg (priv->db));
		sqlite3_close (priv->db);
		return FALSE;
	}

	/* check devices */
	rc = sqlite3_exec (priv->db, "SELECT * FROM history LIMIT 1",
			   NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_debug ("FuPending: creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE history ("
			    "device_id TEXT PRIMARY KEY,"
			    "update_state INTEGER DEFAULT 0,"
			    "update_error TEXT,"
			    "filename TEXT,"
			    "display_name TEXT,"
			    "plugin TEXT,"
			    "device_created INTEGER DEFAULT 0,"
			    "device_modified INTEGER DEFAULT 0,"
			    "checksum TEXT DEFAULT NULL,"
			    "flags INTEGER DEFAULT 0,"
			    "fwupd_version TEXT DEFAULT NULL,"
			    "guid_default TEXT DEFAULT NULL,"
			    "version_old TEXT,"
			    "version_new TEXT);";
		rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
		if (rc != SQLITE_OK) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_WRITE,
				     "Cannot create database: %s",
				     error_msg);
			sqlite3_free (error_msg);
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
fu_pending_add_device (FuPending *pending, FuDevice *device, FwupdRelease *release, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	char *statement;
	const gchar *checksum = NULL;
	gboolean ret = TRUE;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);
	g_return_val_if_fail (FU_IS_DEVICE (device), FALSE);
	g_return_val_if_fail (FWUPD_IS_RELEASE (release), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	g_debug ("FuPending: add device %s", fu_device_get_id (device));
	if (release != NULL) {
		GPtrArray *checksums = fwupd_release_get_checksums (release);
		checksum = fwupd_checksum_get_by_kind (checksums, G_CHECKSUM_SHA1);
	}
	statement = sqlite3_mprintf ("INSERT INTO history (device_id,"
							  "update_state,"
							  "update_error,"
							  "flags,"
							  "filename,"
							  "checksum,"
							  "display_name,"
							  "plugin,"
							  "guid_default,"
							  "fwupd_version,"
							  "device_created,"
							  "device_modified,"
							  "version_old,"
							  "version_new) "
				     "VALUES (%Q,%i,%Q,%i,%Q,%Q,%Q,%Q,%Q,%Q,%i,%i,%Q,%Q)",
				     fu_device_get_id (device),
				     fu_device_get_update_state (device),
				     fu_device_get_update_error (device),
				     fu_device_get_flags (device),
				     fwupd_release_get_filename (release),
				     checksum,
				     fu_device_get_name (device),
				     fu_device_get_plugin (device),
				     fu_device_get_guid_default (device),
				     VERSION,
				     fu_device_get_created (device),
				     fu_device_get_modified (device),
				     fu_device_get_version (device),
				     fwupd_release_get_version (release));

	/* insert entry */
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	sqlite3_free (statement);
	return ret;
}

gboolean
fu_pending_remove_all_with_state (FuPending *pending,
				  FwupdUpdateState update_state,
				  GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	g_autofree gchar *statement = NULL;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	/* remove entries */
	g_debug ("FuPending: removing all devices with update_state %s",
		 fwupd_update_state_to_string (update_state));
	statement = g_strdup_printf ("DELETE FROM history WHERE update_state = '%u')",
				     update_state);
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_pending_remove_all (FuPending *pending, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	/* remove entries */
	g_debug ("FuPending: removing all devices");
	rc = sqlite3_exec (priv->db, "DELETE FROM history;", NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_pending_remove_device (FuPending *pending, FuDevice *device, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	char *statement;
	gboolean ret = TRUE;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	g_debug ("FuPending: remove device %s", fu_device_get_id (device));
	statement = sqlite3_mprintf ("DELETE FROM history WHERE "
				     "device_id = %Q;",
				     fu_device_get_id (device));

	/* remove entry */
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	sqlite3_free (statement);
	return ret;
}

static gint
fu_pending_device_sqlite_cb (void *data,
			gint argc,
			gchar **argv,
			gchar **col_name)
{
	GPtrArray *array = (GPtrArray *) data;
	FuDevice *device;
	g_autoptr(FwupdRelease) release = fwupd_release_new ();

	/* create new result */
	device = fu_device_new ();
	fwupd_device_add_release (FWUPD_DEVICE (device), release);
	g_ptr_array_add (array, device);

	g_debug ("FuPending: got sql result %s", argv[0]);
	for (gint i = 0; i < argc; i++) {
		if (g_strcmp0 (col_name[i], "device_id") == 0) {
			/* we don't want to hash-the-hash */
			fwupd_device_set_id (FWUPD_DEVICE (device), argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "filename") == 0) {
			fwupd_release_set_filename (release, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "display_name") == 0) {
			fu_device_set_name (device, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "version_old") == 0) {
			fu_device_set_version (device, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "version_new") == 0) {
			fwupd_release_set_version (release, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "plugin") == 0) {
			fu_device_set_plugin (device, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "update_state") == 0) {
			FwupdUpdateState update_state = atoi (argv[i]);
			fu_device_set_update_state (device, update_state);
			continue;
		}
		if (g_strcmp0 (col_name[i], "device_created") == 0) {
			guint64 timestamp = g_ascii_strtoull (argv[i], NULL, 10);
			if (timestamp > 0)
				fu_device_set_created (device, timestamp);
			continue;
		}
		if (g_strcmp0 (col_name[i], "device_modified") == 0) {
			guint64 timestamp = g_ascii_strtoull (argv[i], NULL, 10);
			if (timestamp > 0)
				fu_device_set_modified (device, timestamp);
			continue;
		}
		if (g_strcmp0 (col_name[i], "update_error") == 0) {
			if (argv[i] != NULL)
				fu_device_set_update_error (device, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "checksum") == 0) {
			if (argv[i] != NULL)
				fwupd_release_add_checksum (release, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "fwupd_version") == 0) {
			if (argv[i] != NULL)
				fwupd_release_set_vendor (release, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "guid_default") == 0) {
			if (argv[i] != NULL)
				fu_device_add_guid (device, argv[i]);
			continue;
		}
		if (g_strcmp0 (col_name[i], "flags") == 0) {
			guint64 flags = g_ascii_strtoull (argv[i], NULL, 10);
			if (flags > 0)
				fu_device_set_flags (device, flags);
			continue;
		}
	}

	return 0;
}

FuDevice *
fu_pending_get_device (FuPending *pending, const gchar *device_id, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	FuDevice *device = NULL;
	char *error_msg = NULL;
	char *statement;
	gint rc;
	g_autoptr(GPtrArray) array_tmp = NULL;

	g_return_val_if_fail (FU_IS_PENDING (pending), NULL);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return NULL;
	}

	/* get all the devices */
	g_debug ("FuPending: get device");
	statement = sqlite3_mprintf ("SELECT * FROM history WHERE "
				     "device_id = %Q;",
				     device_id);
	array_tmp = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	rc = sqlite3_exec (priv->db,
			   statement,
			   fu_pending_device_sqlite_cb,
			   array_tmp,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		goto out;
	}
	if (array_tmp->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_FOUND,
				     "No devices found");
		goto out;
	}
	device = g_object_ref (g_ptr_array_index (array_tmp, 0));
out:
	sqlite3_free (statement);
	return device;
}

GPtrArray *
fu_pending_get_devices (FuPending *pending, GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	GPtrArray *array = NULL;
	char *error_msg = NULL;
	char *statement;
	gint rc;
	g_autoptr(GPtrArray) array_tmp = NULL;

	g_return_val_if_fail (FU_IS_PENDING (pending), NULL);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return NULL;
	}

	/* get all the devices */
	g_debug ("FuPending: get devices");
	statement = sqlite3_mprintf ("SELECT * FROM history;");
	array_tmp = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	rc = sqlite3_exec (priv->db,
			   statement,
			   fu_pending_device_sqlite_cb,
			   array_tmp,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		goto out;
	}

	/* success */
	array = g_ptr_array_ref (array_tmp);
out:
	sqlite3_free (statement);
	return array;
}

gboolean
fu_pending_set_device_flags (FuPending *pending,
			     FuDevice *device,
			     FwupdDeviceFlags device_flags,
			     GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	char *statement;
	gboolean ret = TRUE;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	/* overwrite entry if it exists */
	g_debug ("FuPending: set device-flags of %s to %" G_GUINT64_FORMAT,
		 fu_device_get_id (device), device_flags);
	statement = sqlite3_mprintf ("UPDATE history SET flags = %i WHERE "
				     "device_id = %Q;",
				     device_flags, fu_device_get_id (device));
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	sqlite3_free (statement);
	return ret;
}

gboolean
fu_pending_set_update_state (FuPending *pending,
			     FuDevice *device,
			     FwupdUpdateState update_state,
			     GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	char *statement;
	gboolean ret = TRUE;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	g_debug ("FuPending: set update-state of %s to %s",
		 fu_device_get_id (device),
		 fwupd_update_state_to_string (update_state));
	statement = sqlite3_mprintf ("UPDATE history SET update_state = %i WHERE "
				     "device_id = %Q;",
				     update_state, fu_device_get_id (device));

	/* remove entry */
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	sqlite3_free (statement);
	return ret;
}

gboolean
fu_pending_set_error_msg (FuPending *pending,
			  FuDevice *device,
			  const gchar *error_msg2,
			  GError **error)
{
	FuPendingPrivate *priv = GET_PRIVATE (pending);
	char *error_msg = NULL;
	char *statement;
	gboolean ret = TRUE;
	gint rc;

	g_return_val_if_fail (FU_IS_PENDING (pending), FALSE);

	/* lazy load */
	if (priv->db == NULL) {
		if (!fu_pending_load (pending, error))
			return FALSE;
	}

	g_debug ("FuPending: set error to %s: %s",
		 fu_device_get_id (device), error_msg2);
	statement = sqlite3_mprintf ("UPDATE history SET update_error = %Q WHERE "
				     "device_id = %Q;",
				     error_msg2,
				     fu_device_get_id (device));

	/* remove entry */
	rc = sqlite3_exec (priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "SQL error: %s",
			     error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	sqlite3_free (statement);
	return ret;
}

static void
fu_pending_class_init (FuPendingClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = fu_pending_finalize;
}

static void
fu_pending_init (FuPending *pending)
{
}

static void
fu_pending_finalize (GObject *object)
{
	FuPending *pending = FU_PENDING (object);
	FuPendingPrivate *priv = GET_PRIVATE (pending);

	if (priv->db != NULL)
		sqlite3_close (priv->db);

	G_OBJECT_CLASS (fu_pending_parent_class)->finalize (object);
}

FuPending *
fu_pending_new (void)
{
	FuPending *pending;
	pending = g_object_new (FU_TYPE_PENDING, NULL);
	return FU_PENDING (pending);
}
