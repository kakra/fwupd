/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Intel Corporation <thunderbolt-software@lists.01.org>
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
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

#include <appstream-glib.h>
#include <gudev/gudev.h>
#include <tbt/tbt_fwu.h>

#include "fu-plugin.h"
#include "fu-plugin-vfuncs.h"

#define FU_PLUGIN_THUNDERBOLT_MAX_ID_LEN	50

struct FuPluginData {
	/* A handle on a list of libtbtfwu controller objects.  These must
	 * eventually be freed.
	 */
	struct tbt_fwu_Controller		**controllers;

	/* The number of controller objects in controllers and in
	 * controller_info.
	 */
	gsize					 controllers_len;

	/* A handle on some state for dealing with our registration
	 * for udev events.
	 */
	GUdevClient				*gudev_client;
};

//FIXME: this needs to move to the plugin core
static gchar *
fu_plugin_thunderbolt_get_id (GUdevDevice *device)
{
	gchar *id;
	id = g_strdup_printf ("ro-%s", g_udev_device_get_sysfs_path (device));
	g_strdelimit (id, "/:.-", '_');
	return id;
}

static gboolean
fu_plugin_thunderbolt_rescan_controllers (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	gint rc;

	/*
	 * This sleep is unfortunate.  It is a work-around for potential race
	 * conditions in which the udev event arrives to fwupd prior to the
	 * daemon refreshing itself.
	 *
	 * In this case, our daemon poll will get the stale data, and we will
	 * not properly refresh the fwupd controller list.
	 */
	g_usleep (3 * G_USEC_PER_SEC);

	/* get the new list */
	if (data->controllers != NULL) {
		tbt_fwu_freeControllerList (data->controllers,
					    data->controllers_len);
	}
	rc = tbt_fwu_getControllerList (&data->controllers,
					&data->controllers_len);
	if (rc != TBT_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "failed to retrieve TBT controller list: %s",
			     tbt_strerror (rc));
		return FALSE;
	}
	return TRUE;
}

static struct tbt_fwu_Controller *
fu_plugin_thunderbolt_find_controller_by_id (FuPlugin *plugin,
					     const gchar *tdbid,
					     GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);

	/* look at all the controllers */
	for (gsize i = 0; i < data->controllers_len; i++) {
		gchar tdbid_tmp[FU_PLUGIN_THUNDERBOLT_MAX_ID_LEN];
		gint rc;
		gsize tdbid_sz = sizeof (tdbid_tmp);

		/* get the ID */
		rc = tbt_fwu_Controller_getID (data->controllers[i],
					       tdbid_tmp, &tdbid_sz);
		if (rc != TBT_OK) {
			g_warning ("failed to get tbd ID: %s",
				   tbt_strerror (rc));
			continue;
		}

		/* is the same */
		if (g_strcmp0 (tdbid_tmp, tdbid) == 0)
			return data->controllers[i];
	}

	/* nothing found */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_FOUND,
		     "no TBT device with ID %s found",
		     tdbid);
	return NULL;
}

static struct tbt_fwu_Controller *
fu_plugin_thunderbolt_find_controller_by_dev (FuPlugin *plugin,
					      guint16 vendor_id,
					      guint16 device_id,
					      GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);

	/* look at all the controllers */
	for (gsize i = 0; i < data->controllers_len; i++) {
		gint rc;
		guint16 device_id_tmp;
		guint16 vendor_id_tmp;

		/* get the IDs: FIXME -- do we cache these? */
		rc = tbt_fwu_Controller_getVendorID (data->controllers[i],
						     &vendor_id_tmp);
		if (rc != TBT_OK) {
			g_warning ("failed to get tbd vendor ID: %s",
				   tbt_strerror (rc));
			continue;
		}
		rc = tbt_fwu_Controller_getModelID (data->controllers[i],
						    &device_id_tmp);
		if (rc != TBT_OK) {
			g_warning ("failed to get tbd model ID: %s",
				   tbt_strerror (rc));
			continue;
		}

		/* is the same -- FIXME: if more that 1 with the same VID:DID? */
		if (vendor_id == vendor_id_tmp &&
		    device_id == device_id_tmp)
			return data->controllers[i];
	}

	/* nothing found */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_FOUND,
		     "no TBT device with VID:DID %04X:%04X found",
		     vendor_id, device_id);
	return NULL;
}

static void
fu_plugin_thunderbolt_add (FuPlugin *plugin, GUdevDevice *device)
{
	const gchar *model;
	const gchar *vendor;
	gchar tdbid[FU_PLUGIN_THUNDERBOLT_MAX_ID_LEN];
	gint rc;
	gsize tdbid_sz = sizeof (tdbid);
	guint16 device_id;
	guint16 vendor_id;
	guint32 version_major;
	guint32 version_minor;
	struct tbt_fwu_Controller *controller;
	g_autofree gchar *guid_id = NULL;
	g_autofree gchar *guid = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *id = NULL;
	g_autoptr(AsProfile) profile = as_profile_new ();
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(FuDevice) dev = NULL;
	g_autoptr(GError) error = NULL;

	/* check vendor ID */
	vendor_id = g_udev_device_get_sysfs_attr_as_int (device, "vendor");
	if (vendor_id != 0x8086)
		return;

	/* check device ID */
	device_id = g_udev_device_get_sysfs_attr_as_int (device, "device");
	if (device_id != 0x1577 && device_id != 0x1575 &&
	    device_id != 0x15BF && device_id != 0x15D2 &&
	    device_id != 0x15D9)
		return;

	/* is already in database */
	id = fu_plugin_thunderbolt_get_id (device);
	dev = fu_plugin_cache_lookup (plugin, id);
	if (dev != NULL) {
		g_debug ("ignoring duplicate %s", id);
		return;
	}

	/* start profiling */
	ptask = as_profile_start (profile, "FuPluginThunderbolt:add{%s}", id);
	g_assert (ptask != NULL);
	g_debug ("adding thunderbolt device: %s",
		 g_udev_device_get_sysfs_path (device));

	/* find controller object */
	controller = fu_plugin_thunderbolt_find_controller_by_dev (plugin,
								   vendor_id,
								   device_id,
								   &error);
	if (controller == NULL) {
		g_warning ("%s", error->message);
		return;
	}

	/* get the Thunderbolt ID */
	rc = tbt_fwu_Controller_getID (controller, tdbid, &tdbid_sz);
	if (rc != TBT_OK) {
		g_warning ("failed to get tbd ID: %s",
			   tbt_strerror (rc));
		return;
	}

	/* get the version */
	rc = tbt_fwu_Controller_getNVMVersion (controller,
					       &version_major,
					       &version_minor);
	if (rc != TBT_OK) {
		g_warning ("failed to get tbd firmware version: %s",
			   tbt_strerror (rc));
		return;
	}

	/* did we get enough data */
	dev = fu_device_new ();
	fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_ALLOW_ONLINE);
	fu_device_set_id (dev, id);
	fu_device_set_metadata (dev, "Thunderbolt::ID", tdbid);
	guid_id = g_strdup_printf ("PCI\\VEN_%04X&DEV_%04X",
				   vendor_id, device_id);
	guid = as_utils_guid_from_string (guid_id);
	fu_device_add_guid (dev, guid);
	vendor = g_udev_device_get_property (device, "ID_VENDOR_FROM_DATABASE");
	if (vendor != NULL)
		fu_device_set_vendor (dev, vendor);
	model = g_udev_device_get_property (device, "ID_MODEL_FROM_DATABASE");
	if (model != NULL)
		fu_device_set_name (dev, model);
	version = g_strdup_printf ("%" G_GUINT32_FORMAT ".%02" G_GUINT32_FORMAT,
				   version_major, version_minor);
	fu_device_set_version (dev, version);

	/* insert to hash */
	fu_plugin_cache_add (plugin, id, dev);
	fu_plugin_device_add (plugin, dev);
}

static void
fu_plugin_thunderbolt_remove (FuPlugin *plugin, GUdevDevice *device)
{
	FuDevice *dev;
	g_autofree gchar *id = fu_plugin_thunderbolt_get_id (device);
	dev = fu_plugin_cache_lookup (plugin, id);
	if (dev == NULL)
		return;
	fu_plugin_device_remove (plugin, dev);
}

gboolean
fu_plugin_update_online (FuPlugin *plugin,
			 FuDevice *dev,
			 GBytes *blob_fw,
			 FwupdInstallFlags flags,
			 GError **error)
{
	const gchar *tdbid;
	const guint8 *blob;
	gint rc;
	gsize blob_sz;
	struct tbt_fwu_Controller *controller;

	/* find controller */
	tdbid = fu_device_get_metadata (dev, "Thunderbolt::ID");
	controller = fu_plugin_thunderbolt_find_controller_by_id (plugin,
								  tdbid,
								  error);
	if (controller == NULL)
		return FALSE;


	/* validate the image */
	blob = (const guint8 *) g_bytes_get_data (blob_fw, &blob_sz);
	rc = tbt_fwu_Controller_validateFWImage (controller, blob, blob_sz);
	if (rc != TBT_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "TBT firmware validation failed: %s",
			     tbt_strerror (rc));
		return FALSE;
	}

	/* update the device */
	rc = tbt_fwu_Controller_updateFW (controller, blob, blob_sz);
	if (rc != TBT_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "TBT firmware update failed: %s",
			     tbt_strerror (rc));
		return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_plugin_thunderbolt_uevent_cb (GUdevClient *gudev_client,
				 const gchar *action,
				 GUdevDevice *udev_device,
				 FuPlugin *plugin)
{
	if (g_strcmp0 (action, "remove") == 0) {
		/* FIXME: do we need to rescan? With the delay? */
		fu_plugin_thunderbolt_remove (plugin, udev_device);
		return;
	}
	if (g_strcmp0 (action, "add") == 0) {
		g_autoptr(GError) error = NULL;
		/* rescan */
		if (!fu_plugin_thunderbolt_rescan_controllers (plugin, &error)) {
			g_warning ("%s", error->message);
			return;
		}
		fu_plugin_thunderbolt_add (plugin, udev_device);
		return;
	}
}

void
fu_plugin_init (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_alloc_data (plugin, sizeof (FuPluginData));
	const gchar* subsystems[] = { "pci", NULL };
	data->gudev_client = g_udev_client_new (subsystems);
	g_signal_connect (data->gudev_client, "uevent",
			  G_CALLBACK (fu_plugin_thunderbolt_uevent_cb), plugin);
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	tbt_fwu_freeControllerList (data->controllers, data->controllers_len);
	tbt_fwu_shutdown ();
	g_object_unref (data->gudev_client);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
	gint rc = tbt_fwu_init ();
	if (rc != TBT_OK) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "TBT initialization failed: %s",
			     tbt_strerror (rc));
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_plugin_coldplug (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_autoptr(GList) devices = NULL;

	/* get all devices of class */
	devices = g_udev_client_query_by_subsystem (data->gudev_client, "pci");
	for (GList *l = devices; l != NULL; l = l->next) {
		GUdevDevice *udev_device = l->data;
		fu_plugin_thunderbolt_add (plugin, udev_device);
	}
	g_list_foreach (devices, (GFunc) g_object_unref, NULL);
	return TRUE;
}
