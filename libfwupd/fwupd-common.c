/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2017-2018 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "config.h"

#include "fwupd-common-private.h"
#include "fwupd-device.h"
#include "fwupd-error.h"
#include "fwupd-release.h"

#include <locale.h>
#include <string.h>
#include <sys/utsname.h>
#include <json-glib/json-glib.h>

/**
 * fwupd_checksum_guess_kind:
 * @checksum: A checksum
 *
 * Guesses the checksum kind based on the length of the hash.
 *
 * Returns: a #GChecksumType, e.g. %G_CHECKSUM_SHA1
 *
 * Since: 0.9.3
 **/
GChecksumType
fwupd_checksum_guess_kind (const gchar *checksum)
{
	guint len;
	if (checksum == NULL)
		return G_CHECKSUM_SHA1;
	len = strlen (checksum);
	if (len == 32)
		return G_CHECKSUM_MD5;
	if (len == 40)
		return G_CHECKSUM_SHA1;
	if (len == 64)
		return G_CHECKSUM_SHA256;
	if (len == 128)
		return G_CHECKSUM_SHA512;
	return G_CHECKSUM_SHA1;
}

static const gchar *
_g_checksum_type_to_string (GChecksumType checksum_type)
{
	if (checksum_type == G_CHECKSUM_MD5)
		return "MD5";
	if (checksum_type == G_CHECKSUM_SHA1)
		return "SHA1";
	if (checksum_type == G_CHECKSUM_SHA256)
		return "SHA256";
	if (checksum_type == G_CHECKSUM_SHA512)
		return "SHA512";
	return NULL;
}

/**
 * fwupd_checksum_format_for_display:
 * @checksum: A checksum
 *
 * Formats a checksum for display.
 *
 * Returns: text, or %NULL for invalid
 *
 * Since: 0.9.3
 **/
gchar *
fwupd_checksum_format_for_display (const gchar *checksum)
{
	GChecksumType kind = fwupd_checksum_guess_kind (checksum);
	return g_strdup_printf ("%s(%s)", _g_checksum_type_to_string (kind), checksum);
}

/**
 * fwupd_checksum_get_by_kind:
 * @checksums: (element-type utf8): checksums
 * @kind: a #GChecksumType, e.g. %G_CHECKSUM_SHA512
 *
 * Gets a specific checksum kind.
 *
 * Returns: a checksum from the array, or %NULL if not found
 *
 * Since: 0.9.4
 **/
const gchar *
fwupd_checksum_get_by_kind (GPtrArray *checksums, GChecksumType kind)
{
	for (guint i = 0; i < checksums->len; i++) {
		const gchar *checksum = g_ptr_array_index (checksums, i);
		if (fwupd_checksum_guess_kind (checksum) == kind)
			return checksum;
	}
	return NULL;
}

/**
 * fwupd_checksum_get_best:
 * @checksums: (element-type utf8): checksums
 *
 * Gets a the best possible checksum kind.
 *
 * Returns: a checksum from the array, or %NULL if nothing was suitable
 *
 * Since: 0.9.4
 **/
const gchar *
fwupd_checksum_get_best (GPtrArray *checksums)
{
	GChecksumType checksum_types[] = {
		G_CHECKSUM_SHA512,
		G_CHECKSUM_SHA256,
		G_CHECKSUM_SHA1,
		0 };
	for (guint i = 0; checksum_types[i] != 0; i++) {
		for (guint j = 0; j < checksums->len; j++) {
			const gchar *checksum = g_ptr_array_index (checksums, j);
			if (fwupd_checksum_guess_kind (checksum) == checksum_types[i])
				return checksum;
		}
	}
	return NULL;
}

static gchar *
fwupd_build_user_agent_os_release (void)
{
	const gchar *keys[] = { "NAME=", "VERSION_ID=", "VARIANT=", NULL };
	const gchar *values[] = { NULL, NULL, NULL, NULL };
	g_autofree gchar *os_release = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GPtrArray) ids_os = g_ptr_array_new ();

	/* get raw data then parse each line */
	if (!g_file_get_contents ("/etc/os-release", &os_release, NULL, NULL)) {
		if (!g_file_get_contents ("/usr/lib/os-release", &os_release, NULL, NULL))
			return NULL;
	}
	lines = g_strsplit (os_release, "\n", -1);
	for (guint i = 0; lines[i] != NULL; i++) {
		for (guint j = 0; keys[j] != NULL; j++) {
			if (g_str_has_prefix (lines[i], keys[j])) {
				values[j] = lines[i] + strlen (keys[j]);
				break;
			}
		}
	}

	/* create an array of the keys that exist */
	for (guint j = 0; values[j] != NULL; j++) {
		if (values[j] != NULL)
			g_ptr_array_add (ids_os, (gpointer) values[j]);
	}
	if (ids_os->len == 0)
		return NULL;
	g_ptr_array_add (ids_os, NULL);
	return g_strjoinv (" ", (gchar **) ids_os->pdata);
}

static gchar *
fwupd_build_user_agent_system (void)
{
	struct utsname name_tmp = { 0 };
	g_autofree gchar *locale = NULL;
	g_autofree gchar *os_release = NULL;
	g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);

	/* system, architecture and kernel, e.g. "Linux i686 4.14.5" */
	if (uname (&name_tmp) >= 0) {
		g_ptr_array_add (ids, g_strdup_printf ("%s %s %s",
						       name_tmp.sysname,
						       name_tmp.machine,
						       name_tmp.release));
	}

	/* current locale, e.g. "en-gb" */
	locale = g_strdup (setlocale (LC_MESSAGES, NULL));
	if (locale != NULL) {
		g_strdelimit (locale, ".", '\0');
		g_strdelimit (locale, "_", '-');
		g_ptr_array_add (ids, g_steal_pointer (&locale));
	}

	/* OS release, e.g. "Fedora 27 Workstation" */
	os_release = fwupd_build_user_agent_os_release ();
	if (os_release != NULL)
		g_ptr_array_add (ids, g_steal_pointer (&os_release));

	/* convert to string */
	if (ids->len == 0)
		return NULL;
	g_ptr_array_add (ids, NULL);
	return g_strjoinv ("; ", (gchar **) ids->pdata);
}

/**
 * fwupd_build_user_agent:
 * @package_name: client program name, e.g. "gnome-software"
 * @package_version: client program version, e.g. "3.28.1"
 *
 * Builds a user-agent to use for the download.
 *
 * Supplying harmless details to the server means it knows more about each
 * client. This allows the web service to respond in a different way, for
 * instance sending a different metadata file for old versions of fwupd, or
 * returning an error for Solaris machines.
 *
 * Before freaking out about theoretical privacy implications, much more data
 * than this is sent to each and every website you visit.
 *
 * Returns: a string, e.g. `foo/0.1 (Linux i386 4.14.5; en; Fedora 27) fwupd/1.0.3`
 *
 * Since: 1.0.3
 **/
gchar *
fwupd_build_user_agent (const gchar *package_name, const gchar *package_version)
{
	GString *str = g_string_new (NULL);
	g_autofree gchar *system = NULL;

	/* application name and version */
	g_string_append_printf (str, "%s/%s", package_name, package_version);

	/* system information */
	system = fwupd_build_user_agent_system ();
	if (system != NULL)
		g_string_append_printf (str, " (%s)", system);

	/* platform, which in our case is just fwupd */
	if (g_strcmp0 (package_name, "fwupd") != 0)
		g_string_append_printf (str, " fwupd/%s", PACKAGE_VERSION);

	/* success */
	return g_string_free (str, FALSE);
}

/**
 * fwupd_build_machine_id:
 * @salt: The salt, or %NULL for none
 * @error: A #GError or %NULL
 *
 * Gets a salted hash of the /etc/machine-id contents. This can be used to
 * identify a specific machine. It is not possible to recover the original
 * machine-id from the machine-hash.
 *
 * Returns: the SHA256 machine hash, or %NULL if the ID is not present
 *
 * Since: 1.0.4
 **/
gchar *
fwupd_build_machine_id (const gchar *salt, GError **error)
{
	g_autofree gchar *buf = NULL;
	g_autoptr(GChecksum) csum = NULL;
	gsize sz = 0;

	/* this has to exist */
	if (!g_file_get_contents ("/etc/machine-id", &buf, &sz, error))
		return NULL;
	if (sz == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_READ,
				     "The machine-id is present but unset");
		return NULL;
	}
	csum = g_checksum_new (G_CHECKSUM_SHA256);
	if (salt != NULL)
		g_checksum_update (csum, (const guchar *) salt, (gssize) strlen (salt));
	g_checksum_update (csum, (const guchar *) buf, (gssize) sz);
	return g_strdup (g_checksum_get_string (csum));
}

/**
 * fwupd_build_history_report_json:
 * @devices: (element-type FwupdDevice): devices
 * @error: A #GError or %NULL
 *
 * Builds a JSON report for the list of devices.
 *
 * Returns: a string, or %NULL if the ID is not present
 *
 * Since: 1.0.4
 **/
gchar *
fwupd_build_history_report_json (GPtrArray *devices, GError **error)
{
	gchar *data;
	g_autofree gchar *machine_id = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;

	/* get a hash that represents the machine */
	machine_id = fwupd_build_machine_id ("fwupd", error);
	if (machine_id == NULL)
		return FALSE;

	/* create header */
	builder = json_builder_new ();
	json_builder_begin_object (builder);
	json_builder_set_member_name (builder, "ReportVersion");
	json_builder_add_int_value (builder, 1);
	json_builder_set_member_name (builder, "MachineId");
	json_builder_add_string_value (builder, machine_id);

	/* add each device */
	json_builder_set_member_name (builder, "Reports");
	json_builder_begin_array (builder);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel = fwupd_device_get_release_default (dev);
		GPtrArray *checksums;

		if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_REPORTED))
			continue;

		json_builder_begin_object (builder);

		/* identify different devices */
		json_builder_set_member_name (builder, "DeviceId");
		json_builder_add_string_value (builder, fwupd_device_get_id (dev));

		/* identify the firmware used */
		json_builder_set_member_name (builder, "Checksum");
		checksums = fwupd_release_get_checksums (rel);
		json_builder_add_string_value (builder, fwupd_checksum_get_by_kind (checksums, G_CHECKSUM_SHA1));

		/* set the error state of the report */
		json_builder_set_member_name (builder, "UpdateState");
		json_builder_add_int_value (builder, fwupd_device_get_update_state (dev));
		if (fwupd_device_get_update_error (dev) != NULL) {
			json_builder_set_member_name (builder, "UpdateError");
			json_builder_add_string_value (builder, fwupd_device_get_update_error (dev));
		}

		/* map back to the dev type on the LVFS */
		json_builder_set_member_name (builder, "Guid");
		json_builder_add_string_value (builder, fwupd_device_get_guid_default (dev));

		/* to know what plugin tried to handle the dev */
		json_builder_set_member_name (builder, "FwupdVersion");
		json_builder_add_string_value (builder, fwupd_release_get_vendor (rel));
		json_builder_set_member_name (builder, "Plugin");
		json_builder_add_string_value (builder, fwupd_device_get_plugin (dev));

		/* report what we're trying to update *from* and *to* */
		json_builder_set_member_name (builder, "Version");
		json_builder_add_string_value (builder, fwupd_device_get_version (dev));
		json_builder_set_member_name (builder, "VersionNew");
		json_builder_add_string_value (builder, fwupd_release_get_version (rel));

		/* to know the state of the dev we're trying to update */
		json_builder_set_member_name (builder, "Flags");
		json_builder_add_int_value (builder, fwupd_device_get_flags (dev));

		/* to know when the update tried to happen, and how soon after boot */
		json_builder_set_member_name (builder, "Created");
		json_builder_add_int_value (builder, fwupd_device_get_created (dev));
		json_builder_set_member_name (builder, "Modified");
		json_builder_add_int_value (builder, fwupd_device_get_modified (dev));

		json_builder_end_object (builder);
	}
	json_builder_end_array (builder);
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);
	if (data == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "Failed to convert to JSON string");
		return NULL;
	}
	return data;
}
