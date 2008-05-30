/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-device.c
 *
 * Copyright (C) 2007 David Zeuthen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <time.h>

#include "gdu-private.h"
#include "gdu-pool.h"
#include "gdu-device.h"
#include "devkit-disks-device-glue.h"

/* --- SUCKY CODE BEGIN --- */

/* This totally sucks; dbus-bindings-tool and dbus-glib should be able
 * to do this for us.
 *
 * TODO: keep in sync with code in tools/devkit-disks in DeviceKit-disks.
 */

typedef struct
{
        char *native_path;

        char    *device_file;
        char   **device_file_by_id;
        char   **device_file_by_path;
        gboolean device_is_partition;
        gboolean device_is_partition_table;
        gboolean device_is_removable;
        gboolean device_is_media_available;
        gboolean device_is_read_only;
        gboolean device_is_drive;
        gboolean device_is_crypto_cleartext;
        gboolean device_is_mounted;
        gboolean device_is_busy;
        gboolean device_is_linux_md_component;
        gboolean device_is_linux_md;
        char    *device_mount_path;
        guint64  device_size;
        guint64  device_block_size;

        gboolean job_in_progress;
        char    *job_id;
        gboolean job_is_cancellable;
        int      job_num_tasks;
        int      job_cur_task;
        char    *job_cur_task_id;
        double   job_cur_task_percentage;

        char    *id_usage;
        char    *id_type;
        char    *id_version;
        char    *id_uuid;
        char    *id_label;

        char    *partition_slave;
        char    *partition_scheme;
        int      partition_number;
        char    *partition_type;
        char    *partition_label;
        char    *partition_uuid;
        char   **partition_flags;
        guint64  partition_offset;
        guint64  partition_size;

        char    *partition_table_scheme;
        int      partition_table_count;
        int      partition_table_max_number;
        GArray  *partition_table_offsets;
        GArray  *partition_table_sizes;

        char    *crypto_cleartext_slave;

        char    *drive_vendor;
        char    *drive_model;
        char    *drive_revision;
        char    *drive_serial;
        char    *drive_connection_interface;
        guint64  drive_connection_speed;
        char   **drive_media_compatibility;
        char    *drive_media;

        gboolean               drive_smart_is_capable;
        gboolean               drive_smart_is_enabled;
        guint64                drive_smart_time_collected;
        gboolean               drive_smart_is_failing;
        double                 drive_smart_temperature;
        guint64                drive_smart_time_powered_on;
        char                  *drive_smart_last_self_test_result;
        GValue                 drive_smart_attributes;

        char    *linux_md_component_level;
        int      linux_md_component_num_raid_devices;
        char    *linux_md_component_uuid;
        char    *linux_md_component_name;
        char    *linux_md_component_version;
        guint64  linux_md_component_update_time;
        guint64  linux_md_component_events;

        char    *linux_md_level;
        int      linux_md_num_raid_devices;
        char    *linux_md_version;
        char   **linux_md_slaves;
        char   **linux_md_slaves_state;
        gboolean linux_md_is_degraded;
        char    *linux_md_sync_action;
        double   linux_md_sync_percentage;
        guint64  linux_md_sync_speed;
} DeviceProperties;

static void
collect_props (const char *key, const GValue *value, DeviceProperties *props)
{
        gboolean handled = TRUE;

        if (strcmp (key, "native-path") == 0)
                props->native_path = g_strdup (g_value_get_string (value));

        else if (strcmp (key, "device-file") == 0)
                props->device_file = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "device-file-by-id") == 0)
                props->device_file_by_id = g_strdupv (g_value_get_boxed (value));
        else if (strcmp (key, "device-file-by-path") == 0)
                props->device_file_by_path = g_strdupv (g_value_get_boxed (value));
        else if (strcmp (key, "device-is-partition") == 0)
                props->device_is_partition = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-partition-table") == 0)
                props->device_is_partition_table = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-removable") == 0)
                props->device_is_removable = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-media-available") == 0)
                props->device_is_media_available = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-read-only") == 0)
                props->device_is_read_only = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-drive") == 0)
                props->device_is_drive = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-crypto-cleartext") == 0)
                props->device_is_crypto_cleartext = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-linux-md-component") == 0)
                props->device_is_linux_md_component = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-linux-md") == 0)
                props->device_is_linux_md = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-mounted") == 0)
                props->device_is_mounted = g_value_get_boolean (value);
        else if (strcmp (key, "device-is-busy") == 0)
                props->device_is_busy = g_value_get_boolean (value);
        else if (strcmp (key, "device-mount-path") == 0)
                props->device_mount_path = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "device-size") == 0)
                props->device_size = g_value_get_uint64 (value);
        else if (strcmp (key, "device-block-size") == 0)
                props->device_block_size = g_value_get_uint64 (value);

        else if (strcmp (key, "job-in-progress") == 0)
                props->job_in_progress = g_value_get_boolean (value);
        else if (strcmp (key, "job-id") == 0)
                props->job_id = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "job-is-cancellable") == 0)
                props->job_is_cancellable = g_value_get_boolean (value);
        else if (strcmp (key, "job-num-tasks") == 0)
                props->job_num_tasks = g_value_get_int (value);
        else if (strcmp (key, "job-cur-task") == 0)
                props->job_cur_task = g_value_get_int (value);
        else if (strcmp (key, "job-cur-task-id") == 0)
                props->job_cur_task_id = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "job-cur-task-percentage") == 0)
                props->job_cur_task_percentage = g_value_get_double (value);

        else if (strcmp (key, "id-usage") == 0)
                props->id_usage = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "id-type") == 0)
                props->id_type = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "id-version") == 0)
                props->id_version = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "id-uuid") == 0)
                props->id_uuid = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "id-label") == 0)
                props->id_label = g_strdup (g_value_get_string (value));

        else if (strcmp (key, "partition-slave") == 0)
                props->partition_slave = g_strdup (g_value_get_boxed (value));
        else if (strcmp (key, "partition-scheme") == 0)
                props->partition_scheme = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "partition-number") == 0)
                props->partition_number = g_value_get_int (value);
        else if (strcmp (key, "partition-type") == 0)
                props->partition_type = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "partition-label") == 0)
                props->partition_label = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "partition-uuid") == 0)
                props->partition_uuid = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "partition-flags") == 0)
                props->partition_flags = g_strdupv (g_value_get_boxed (value));
        else if (strcmp (key, "partition-offset") == 0)
                props->partition_offset = g_value_get_uint64 (value);
        else if (strcmp (key, "partition-size") == 0)
                props->partition_size = g_value_get_uint64 (value);

        else if (strcmp (key, "partition-table-scheme") == 0)
                props->partition_table_scheme = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "partition-table-count") == 0)
                props->partition_table_count = g_value_get_int (value);
        else if (strcmp (key, "partition-table-max-number") == 0)
                props->partition_table_max_number = g_value_get_int (value);
        else if (strcmp (key, "partition-table-offsets") == 0) {
                GValue dest_value = {0,};
                g_value_init (&dest_value, dbus_g_type_get_collection ("GArray", G_TYPE_UINT64));
                g_value_copy (value, &dest_value);
                props->partition_table_offsets = g_value_get_boxed (&dest_value);
        } else if (strcmp (key, "partition-table-sizes") == 0) {
                GValue dest_value = {0,};
                g_value_init (&dest_value, dbus_g_type_get_collection ("GArray", G_TYPE_UINT64));
                g_value_copy (value, &dest_value);
                props->partition_table_sizes = g_value_get_boxed (&dest_value);
        }

        else if (strcmp (key, "crypto-cleartext-slave") == 0)
                props->crypto_cleartext_slave = g_strdup (g_value_get_boxed (value));

        else if (strcmp (key, "drive-vendor") == 0)
                props->drive_vendor = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-model") == 0)
                props->drive_model = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-revision") == 0)
                props->drive_revision = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-serial") == 0)
                props->drive_serial = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-connection-interface") == 0)
                props->drive_connection_interface = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-connection-speed") == 0)
                props->drive_connection_speed = g_value_get_uint64 (value);
        else if (strcmp (key, "drive-media-compatibility") == 0)
                props->drive_media_compatibility = g_strdupv (g_value_get_boxed (value));
        else if (strcmp (key, "drive-media") == 0)
                props->drive_media = g_strdup (g_value_get_string (value));

        else if (strcmp (key, "drive-smart-is-capable") == 0)
                props->drive_smart_is_capable = g_value_get_boolean (value);
        else if (strcmp (key, "drive-smart-is-enabled") == 0)
                props->drive_smart_is_enabled = g_value_get_boolean (value);
        else if (strcmp (key, "drive-smart-time-collected") == 0)
                props->drive_smart_time_collected = g_value_get_uint64 (value);
        else if (strcmp (key, "drive-smart-is-failing") == 0)
                props->drive_smart_is_failing = g_value_get_boolean (value);
        else if (strcmp (key, "drive-smart-temperature") == 0)
                props->drive_smart_temperature = g_value_get_double (value);
        else if (strcmp (key, "drive-smart-time-powered-on") == 0)
                props->drive_smart_time_powered_on = g_value_get_uint64 (value);
        else if (strcmp (key, "drive-smart-last-self-test-result") == 0)
                props->drive_smart_last_self_test_result = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "drive-smart-attributes") == 0) {
                g_value_init (&(props->drive_smart_attributes),
                              dbus_g_type_get_collection ("GPtrArray", SMART_DATA_STRUCT_TYPE));
                g_value_copy (value, &(props->drive_smart_attributes));
        }

        else if (strcmp (key, "linux-md-component-level") == 0)
                props->linux_md_component_level = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-component-num-raid-devices") == 0)
                props->linux_md_component_num_raid_devices = g_value_get_int (value);
        else if (strcmp (key, "linux-md-component-uuid") == 0)
                props->linux_md_component_uuid = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-component-name") == 0)
                props->linux_md_component_name = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-component-version") == 0)
                props->linux_md_component_version = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-component-update-time") == 0)
                props->linux_md_component_update_time = g_value_get_uint64 (value);
        else if (strcmp (key, "linux-md-component-events") == 0)
                props->linux_md_component_events = g_value_get_uint64 (value);

        else if (strcmp (key, "linux-md-level") == 0)
                props->linux_md_level = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-num-raid-devices") == 0)
                props->linux_md_num_raid_devices = g_value_get_int (value);
        else if (strcmp (key, "linux-md-version") == 0)
                props->linux_md_version = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-slaves") == 0) {
                int n;
                GPtrArray *object_paths;

                object_paths = g_value_get_boxed (value);

                props->linux_md_slaves = g_new0 (char *, object_paths->len + 1);
                for (n = 0; n < (int) object_paths->len; n++)
                        props->linux_md_slaves[n] = g_strdup (object_paths->pdata[n]);
                props->linux_md_slaves[n] = NULL;
        }
        else if (strcmp (key, "linux-md-slaves-state") == 0)
                props->linux_md_slaves_state = g_strdupv (g_value_get_boxed (value));
        else if (strcmp (key, "linux-md-is-degraded") == 0)
                props->linux_md_is_degraded = g_value_get_boolean (value);
        else if (strcmp (key, "linux-md-sync-action") == 0)
                props->linux_md_sync_action = g_strdup (g_value_get_string (value));
        else if (strcmp (key, "linux-md-sync-percentage") == 0)
                props->linux_md_sync_percentage = g_value_get_double (value);
        else if (strcmp (key, "linux-md-sync-speed") == 0)
                props->linux_md_sync_speed = g_value_get_uint64 (value);

        else
                handled = FALSE;

        if (!handled)
                g_warning ("unhandled property '%s'", key);
}

static DeviceProperties *
device_properties_get (DBusGConnection *bus,
                       const char *object_path)
{
        DeviceProperties *props;
        GError *error;
        GHashTable *hash_table;
        DBusGProxy *prop_proxy;
        const char *ifname = "org.freedesktop.DeviceKit.Disks.Device";

        props = g_new0 (DeviceProperties, 1);

	prop_proxy = dbus_g_proxy_new_for_name (bus,
                                                "org.freedesktop.DeviceKit.Disks",
                                                object_path,
                                                "org.freedesktop.DBus.Properties");
        error = NULL;
        if (!dbus_g_proxy_call (prop_proxy,
                                "GetAll",
                                &error,
                                G_TYPE_STRING,
                                ifname,
                                G_TYPE_INVALID,
                                dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
                                &hash_table,
                                G_TYPE_INVALID)) {
                g_warning ("Couldn't call GetAll() to get properties for %s: %s", object_path, error->message);
                g_error_free (error);
                goto out;
        }

        g_hash_table_foreach (hash_table, (GHFunc) collect_props, props);

        g_hash_table_unref (hash_table);

out:
        g_object_unref (prop_proxy);
        return props;
}

static void
device_properties_free (DeviceProperties *props)
{
        g_free (props->native_path);
        g_free (props->device_file);
        g_strfreev (props->device_file_by_id);
        g_strfreev (props->device_file_by_path);
        g_free (props->device_mount_path);
        g_free (props->job_id);
        g_free (props->job_cur_task_id);
        g_free (props->id_usage);
        g_free (props->id_type);
        g_free (props->id_version);
        g_free (props->id_uuid);
        g_free (props->id_label);
        g_free (props->partition_slave);
        g_free (props->partition_type);
        g_free (props->partition_label);
        g_free (props->partition_uuid);
        g_strfreev (props->partition_flags);
        g_free (props->partition_table_scheme);
        g_array_free (props->partition_table_offsets, TRUE);
        g_array_free (props->partition_table_sizes, TRUE);
        g_free (props->crypto_cleartext_slave);
        g_free (props->drive_model);
        g_free (props->drive_vendor);
        g_free (props->drive_revision);
        g_free (props->drive_serial);
        g_free (props->drive_connection_interface);
        g_strfreev (props->drive_media_compatibility);
        g_free (props->drive_media);
        g_free (props->drive_smart_last_self_test_result);
        g_value_unset (&(props->drive_smart_attributes));
        g_free (props->linux_md_component_level);
        g_free (props->linux_md_component_uuid);
        g_free (props->linux_md_component_name);
        g_free (props->linux_md_component_version);
        g_free (props->linux_md_level);
        g_free (props->linux_md_version);
        g_strfreev (props->linux_md_slaves);
        g_strfreev (props->linux_md_slaves_state);
        g_free (props->linux_md_sync_action);
        g_free (props);
}

/* --- SUCKY CODE END --- */

struct _GduDevicePrivate
{
        DBusGConnection *bus;
        DBusGProxy *proxy;
        GduPool *pool;

        char *object_path;

        DeviceProperties *props;
};

enum {
        JOB_CHANGED,
        CHANGED,
        REMOVED,
        LAST_SIGNAL,
};

static GObjectClass *parent_class = NULL;
static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GduDevice, gdu_device, G_TYPE_OBJECT);

GduPool *
gdu_device_get_pool (GduDevice *device)
{
        return g_object_ref (device->priv->pool);
}

static void
gdu_device_finalize (GduDevice *device)
{
        dbus_g_connection_unref (device->priv->bus);
        g_free (device->priv->object_path);
        if (device->priv->proxy != NULL)
                g_object_unref (device->priv->proxy);
        if (device->priv->pool != NULL)
                g_object_unref (device->priv->pool);
        if (device->priv->props != NULL)
                device_properties_free (device->priv->props);

        if (G_OBJECT_CLASS (parent_class)->finalize)
                (* G_OBJECT_CLASS (parent_class)->finalize) (G_OBJECT (device));
}

static void
gdu_device_class_init (GduDeviceClass *klass)
{
        GObjectClass *obj_class = (GObjectClass *) klass;

        parent_class = g_type_class_peek_parent (klass);

        obj_class->finalize = (GObjectFinalizeFunc) gdu_device_finalize;

        signals[CHANGED] =
                g_signal_new ("changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduDeviceClass, changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);
        signals[JOB_CHANGED] =
                g_signal_new ("job-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduDeviceClass, job_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);
        signals[REMOVED] =
                g_signal_new ("removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduDeviceClass, removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);
}

static void
gdu_device_init (GduDevice *device)
{
        device->priv = g_new0 (GduDevicePrivate, 1);
}

static gboolean
update_info (GduDevice *device)
{
        if (device->priv->props != NULL)
                device_properties_free (device->priv->props);
        device->priv->props = device_properties_get (device->priv->bus, device->priv->object_path);
        return TRUE;
}


GduDevice *
gdu_device_new_from_object_path (GduPool *pool, const char *object_path)
{
        GError *error;
        GduDevice *device;

        device = GDU_DEVICE (g_object_new (GDU_TYPE_DEVICE, NULL));
        device->priv->object_path = g_strdup (object_path);
        device->priv->pool = g_object_ref (pool);

        error = NULL;
        device->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (device->priv->bus == NULL) {
                g_warning ("Couldn't connect to system bus: %s", error->message);
                g_error_free (error);
                goto error;
        }

	device->priv->proxy = dbus_g_proxy_new_for_name (device->priv->bus,
                                                         "org.freedesktop.DeviceKit.Disks",
                                                         device->priv->object_path,
                                                         "org.freedesktop.DeviceKit.Disks.Device");
        dbus_g_proxy_set_default_timeout (device->priv->proxy, INT_MAX);
        dbus_g_proxy_add_signal (device->priv->proxy, "Changed", G_TYPE_INVALID);

        /* TODO: connect signals */

        if (!update_info (device))
                goto error;

        g_print ("%s: %s\n", __FUNCTION__, device->priv->props->device_file);

        return device;
error:
        g_object_unref (device);
        return NULL;
}

void
gdu_device_changed (GduDevice *device)
{
        g_print ("%s: %s\n", __FUNCTION__, device->priv->props->device_file);
        update_info (device);
        g_signal_emit (device, signals[CHANGED], 0);
}

void
gdu_device_job_changed (GduDevice   *device,
                        gboolean     job_in_progress,
                        const char  *job_id,
                        gboolean     job_is_cancellable,
                        int          job_num_tasks,
                        int          job_cur_task,
                        const char  *job_cur_task_id,
                        double       job_cur_task_percentage)
{
        g_print ("%s: %s: %s\n", __FUNCTION__, device->priv->props->device_file, job_id);

        device->priv->props->job_in_progress = job_in_progress;
        g_free (device->priv->props->job_id);
        device->priv->props->job_id = g_strdup (job_id);
        device->priv->props->job_is_cancellable = job_is_cancellable;
        device->priv->props->job_num_tasks = job_num_tasks;
        device->priv->props->job_cur_task = job_cur_task;
        g_free (device->priv->props->job_cur_task_id);
        device->priv->props->job_cur_task_id = g_strdup (job_cur_task_id);
        device->priv->props->job_cur_task_percentage = job_cur_task_percentage;

        g_signal_emit (device, signals[JOB_CHANGED], 0);
}

void
gdu_device_removed (GduDevice *device)
{
        g_print ("%s: %s\n", __FUNCTION__, device->priv->props->device_file);
        g_signal_emit (device, signals[REMOVED], 0);
}

const char *
gdu_device_get_object_path (GduDevice *device)
{
        return device->priv->object_path;
}

/**
 * gdu_device_find_parent:
 * @device: the device
 *
 * Finds a parent device for the given @device. Note that this is only
 * useful for presentation purposes; the device tree may be a lot more
 * complex.
 *
 * Returns: The parent of @device if one could be found, otherwise
 * #NULL. Caller must unref this object using g_object_unref().
 **/
GduDevice *
gdu_device_find_parent (GduDevice *device)
{
        GduDevice *parent;

        parent = NULL;

        /* partitioning relationship */
        if (device->priv->props->device_is_partition &&
            device->priv->props->partition_slave != NULL &&
            strlen (device->priv->props->partition_slave) > 0) {
                parent = gdu_pool_get_by_object_path (device->priv->pool,
                                                      device->priv->props->partition_slave);
        }

        return parent;
}

const char *
gdu_device_get_device_file (GduDevice *device)
{
        return device->priv->props->device_file;
}

guint64
gdu_device_get_size (GduDevice *device)
{
        return device->priv->props->device_size;
}

guint64
gdu_device_get_block_size (GduDevice *device)
{
        return device->priv->props->device_block_size;
}

gboolean
gdu_device_is_removable (GduDevice *device)
{
        return device->priv->props->device_is_removable;
}

gboolean
gdu_device_is_media_available (GduDevice *device)
{
        return device->priv->props->device_is_media_available;
}

gboolean
gdu_device_is_read_only (GduDevice *device)
{
        return device->priv->props->device_is_read_only;
}

gboolean
gdu_device_is_partition (GduDevice *device)
{
        return device->priv->props->device_is_partition;
}

gboolean
gdu_device_is_partition_table (GduDevice *device)
{
        return device->priv->props->device_is_partition_table;
}

gboolean
gdu_device_is_crypto_cleartext (GduDevice *device)
{
        return device->priv->props->device_is_crypto_cleartext;
}

gboolean
gdu_device_is_linux_md_component (GduDevice *device)
{
        return device->priv->props->device_is_linux_md_component;
}

gboolean
gdu_device_is_linux_md (GduDevice *device)
{
        return device->priv->props->device_is_linux_md;
}

gboolean
gdu_device_is_mounted (GduDevice *device)
{
        return device->priv->props->device_is_mounted;
}

gboolean
gdu_device_is_busy (GduDevice *device)
{
        return device->priv->props->device_is_busy;
}

const char *
gdu_device_get_mount_path (GduDevice *device)
{
        return device->priv->props->device_mount_path;
}


const char *
gdu_device_id_get_usage (GduDevice *device)
{
        return device->priv->props->id_usage;
}

const char *
gdu_device_id_get_type (GduDevice *device)
{
        return device->priv->props->id_type;
}

const char *
gdu_device_id_get_version (GduDevice *device)
{
        return device->priv->props->id_version;
}

const char *
gdu_device_id_get_label (GduDevice *device)
{
        return device->priv->props->id_label;
}

const char *
gdu_device_id_get_uuid (GduDevice *device)
{
        return device->priv->props->id_uuid;
}



const char *
gdu_device_partition_get_slave (GduDevice *device)
{
        return device->priv->props->partition_slave;
}

const char *
gdu_device_partition_get_scheme (GduDevice *device)
{
        return device->priv->props->partition_scheme;
}

const char *
gdu_device_partition_get_type (GduDevice *device)
{
        return device->priv->props->partition_type;
}

const char *
gdu_device_partition_get_label (GduDevice *device)
{
        return device->priv->props->partition_label;
}

const char *
gdu_device_partition_get_uuid (GduDevice *device)
{
        return device->priv->props->partition_uuid;
}

char **
gdu_device_partition_get_flags (GduDevice *device)
{
        return device->priv->props->partition_flags;
}

int
gdu_device_partition_get_number (GduDevice *device)
{
        return device->priv->props->partition_number;
}

guint64
gdu_device_partition_get_offset (GduDevice *device)
{
        return device->priv->props->partition_offset;
}

guint64
gdu_device_partition_get_size (GduDevice *device)
{
        return device->priv->props->partition_size;
}



const char *
gdu_device_partition_table_get_scheme (GduDevice *device)
{
        return device->priv->props->partition_table_scheme;
}

int
gdu_device_partition_table_get_count (GduDevice *device)
{
        return device->priv->props->partition_table_count;
}

int
gdu_device_partition_table_get_max_number (GduDevice *device)
{
        return device->priv->props->partition_table_max_number;
}

GArray *
gdu_device_partition_table_get_offsets (GduDevice *device)
{
        return device->priv->props->partition_table_offsets;
}

GArray *
gdu_device_partition_table_get_sizes (GduDevice *device)
{
        return device->priv->props->partition_table_sizes;
}

const char *
gdu_device_crypto_cleartext_get_slave (GduDevice *device)
{
        return device->priv->props->crypto_cleartext_slave;
}

gboolean
gdu_device_is_drive (GduDevice *device)
{
        return device->priv->props->device_is_drive;
}

const char *
gdu_device_drive_get_vendor (GduDevice *device)
{
        return device->priv->props->drive_vendor;
}

const char *
gdu_device_drive_get_model (GduDevice *device)
{
        return device->priv->props->drive_model;
}

const char *
gdu_device_drive_get_revision (GduDevice *device)
{
        return device->priv->props->drive_revision;
}

const char *
gdu_device_drive_get_serial (GduDevice *device)
{
        return device->priv->props->drive_serial;
}

const char *
gdu_device_drive_get_connection_interface (GduDevice *device)
{
        return device->priv->props->drive_connection_interface;
}

guint64
gdu_device_drive_get_connection_speed (GduDevice *device)
{
        return device->priv->props->drive_connection_speed;
}

char **
gdu_device_drive_get_media_compatibility (GduDevice *device)
{
        return device->priv->props->drive_media_compatibility;
}

const char *
gdu_device_drive_get_media (GduDevice *device)
{
        return device->priv->props->drive_media;
}

gboolean
gdu_device_drive_smart_get_is_capable (GduDevice *device)
{
        return device->priv->props->drive_smart_is_capable;
}

gboolean
gdu_device_drive_smart_get_is_enabled (GduDevice *device)
{
        return device->priv->props->drive_smart_is_enabled;
}

GduSmartData *
gdu_device_get_smart_data (GduDevice *device)
{
        return _gdu_smart_data_new_from_values (device->priv->props->drive_smart_time_collected,
                                                device->priv->props->drive_smart_temperature,
                                                device->priv->props->drive_smart_time_powered_on,
                                                device->priv->props->drive_smart_last_self_test_result,
                                                device->priv->props->drive_smart_is_failing,
                                                g_value_get_boxed (&(device->priv->props->drive_smart_attributes)));
}

const char *
gdu_device_linux_md_component_get_level (GduDevice *device)
{
        return device->priv->props->linux_md_component_level;
}

int
gdu_device_linux_md_component_get_num_raid_devices (GduDevice *device)
{
        return device->priv->props->linux_md_component_num_raid_devices;
}

const char *
gdu_device_linux_md_component_get_uuid (GduDevice *device)
{
        return device->priv->props->linux_md_component_uuid;
}

const char *
gdu_device_linux_md_component_get_name (GduDevice *device)
{
        return device->priv->props->linux_md_component_name;
}

const char *
gdu_device_linux_md_component_get_version (GduDevice *device)
{
        return device->priv->props->linux_md_component_version;
}

guint64
gdu_device_linux_md_component_get_update_time (GduDevice *device)
{
        return device->priv->props->linux_md_component_update_time;
}

guint64
gdu_device_linux_md_component_get_events (GduDevice *device)
{
        return device->priv->props->linux_md_component_events;
}

const char *
gdu_device_linux_md_get_level (GduDevice *device)
{
        return device->priv->props->linux_md_level;
}

int
gdu_device_linux_md_get_num_raid_devices (GduDevice *device)
{
        return device->priv->props->linux_md_num_raid_devices;
}

const char *
gdu_device_linux_md_get_version (GduDevice *device)
{
        return device->priv->props->linux_md_version;
}

char **
gdu_device_linux_md_get_slaves (GduDevice *device)
{
        return device->priv->props->linux_md_slaves;
}

char **
gdu_device_linux_md_get_slaves_state (GduDevice *device)
{
        return device->priv->props->linux_md_slaves_state;
}

gboolean
gdu_device_linux_md_is_degraded (GduDevice *device)
{
        return device->priv->props->linux_md_is_degraded;
}

const char *
gdu_device_linux_md_get_sync_action (GduDevice *device)
{
        return device->priv->props->linux_md_sync_action;
}

double
gdu_device_linux_md_get_sync_percentage (GduDevice *device)
{
        return device->priv->props->linux_md_sync_percentage;
}

guint64
gdu_device_linux_md_get_sync_speed (GduDevice *device)
{
        return device->priv->props->linux_md_sync_speed;
}

/* ---------------------------------------------------------------------------------------------------- */

gboolean
gdu_device_job_in_progress (GduDevice *device)
{
        return device->priv->props->job_in_progress;
}

const char *
gdu_device_job_get_id (GduDevice *device)
{
        return device->priv->props->job_id;
}

gboolean
gdu_device_job_is_cancellable (GduDevice *device)
{
        return device->priv->props->job_is_cancellable;
}

int
gdu_device_job_get_num_tasks (GduDevice *device)
{
        return device->priv->props->job_num_tasks;
}

int
gdu_device_job_get_cur_task (GduDevice *device)
{
        return device->priv->props->job_cur_task;
}

const char *
gdu_device_job_get_cur_task_id (GduDevice *device)
{
        return device->priv->props->job_cur_task_id;
}

double
gdu_device_job_get_cur_task_percentage (GduDevice *device)
{
        return device->priv->props->job_cur_task_percentage;
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceFilesystemCreateCompletedFunc callback;
        gpointer user_data;
} FilesystemCreateData;

static void
op_mkfs_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        FilesystemCreateData *data = user_data;

        if (error != NULL) {
                g_warning ("op_mkfs_cb failed: %s", error->message);
        }
        data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_filesystem_create (GduDevice                              *device,
                                 const char                             *fstype,
                                 const char                             *fslabel,
                                 const char                             *fserase,
                                 const char                             *encrypt_passphrase,
                                 GduDeviceFilesystemCreateCompletedFunc  callback,
                                 gpointer                                user_data)
{
        int n;
        FilesystemCreateData *data;
        char *options[16];

        data = g_new0 (FilesystemCreateData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        n = 0;
        if (fslabel != NULL && strlen (fslabel) > 0) {
                options[n++] = g_strdup_printf ("label=%s", fslabel);
        }
        if (fserase != NULL && strlen (fserase) > 0) {
                options[n++] = g_strdup_printf ("erase=%s", fserase);
        }
        if (encrypt_passphrase != NULL && strlen (encrypt_passphrase) > 0) {
                options[n++] = g_strdup_printf ("encrypt=%s", encrypt_passphrase);
        }
        options[n] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_filesystem_create_async (device->priv->proxy,
                                                                        fstype,
                                                                        (const char **) options,
                                                                        op_mkfs_cb,
                                                                        data);
        while (n >= 0)
                g_free (options[n--]);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceFilesystemMountCompletedFunc callback;
        gpointer user_data;
} FilesystemMountData;

static void
op_mount_cb (DBusGProxy *proxy, char *mount_path, GError *error, gpointer user_data)
{
        FilesystemMountData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, mount_path, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_filesystem_mount (GduDevice                   *device,
                                GduDeviceFilesystemMountCompletedFunc  callback,
                                gpointer                     user_data)
{
        const char *fstype;
        char *options[16];
        FilesystemMountData *data;

        data = g_new0 (FilesystemMountData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        options[0] = NULL;
        fstype = NULL;

        org_freedesktop_DeviceKit_Disks_Device_filesystem_mount_async (device->priv->proxy,
                                                                       fstype,
                                                                       (const char **) options,
                                                                       op_mount_cb,
                                                                       data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceFilesystemUnmountCompletedFunc callback;
        gpointer user_data;
} FilesystemUnmountData;

static void
op_unmount_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        FilesystemUnmountData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_filesystem_unmount (GduDevice                     *device,
                                  GduDeviceFilesystemUnmountCompletedFunc  callback,
                                  gpointer                       user_data)
{
        char *options[16];
        FilesystemUnmountData *data;

        data = g_new0 (FilesystemUnmountData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;
        options[0] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_filesystem_unmount_async (device->priv->proxy,
                                                                         (const char **) options,
                                                                         op_unmount_cb,
                                                                         data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDevicePartitionDeleteCompletedFunc callback;
        gpointer user_data;
} PartitionDeleteData;

static void
op_partition_delete_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        PartitionDeleteData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_partition_delete (GduDevice                             *device,
                                const char                            *secure_erase,
                                GduDevicePartitionDeleteCompletedFunc  callback,
                                gpointer                               user_data)
{
        int n;
        char *options[16];
        PartitionDeleteData *data;

        data = g_new0 (PartitionDeleteData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        n = 0;
        if (secure_erase != NULL && strlen (secure_erase) > 0) {
                options[n++] = g_strdup_printf ("erase=%s", secure_erase);
        }
        options[n] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_partition_delete_async (device->priv->proxy,
                                                                       (const char **) options,
                                                                       op_partition_delete_cb,
                                                                       data);

        while (n >= 0)
                g_free (options[n--]);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDevicePartitionCreateCompletedFunc callback;
        gpointer user_data;
} PartitionCreateData;

static void
op_create_partition_cb (DBusGProxy *proxy, char *created_device_object_path, GError *error, gpointer user_data)
{
        PartitionCreateData *data = user_data;

        if (error != NULL) {
                g_warning ("op_create_partition_cb failed: %s", error->message);
                data->callback (data->device, NULL, error, data->user_data);
        } else {
                g_print ("yay objpath='%s'\n", created_device_object_path);
                data->callback (data->device, created_device_object_path, error, data->user_data);
        }
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_partition_create (GduDevice   *device,
                                guint64      offset,
                                guint64      size,
                                const char  *type,
                                const char  *label,
                                char       **flags,
                                const char  *fstype,
                                const char  *fslabel,
                                const char  *fserase,
                                const char  *encrypt_passphrase,
                                GduDevicePartitionCreateCompletedFunc callback,
                                gpointer user_data)
{
        int n;
        char *fsoptions[16];
        char *options[16];
        PartitionCreateData *data;

        data = g_new0 (PartitionCreateData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        options[0] = NULL;

        n = 0;
        if (fslabel != NULL && strlen (fslabel) > 0) {
                fsoptions[n++] = g_strdup_printf ("label=%s", fslabel);
        }
        if (fserase != NULL && strlen (fserase) > 0) {
                fsoptions[n++] = g_strdup_printf ("erase=%s", fserase);
        }
        if (encrypt_passphrase != NULL && strlen (encrypt_passphrase) > 0) {
                fsoptions[n++] = g_strdup_printf ("encrypt=%s", encrypt_passphrase);
        }
        fsoptions[n] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_partition_create_async (device->priv->proxy,
                                                                       offset,
                                                                       size,
                                                                       type,
                                                                       label,
                                                                       (const char **) flags,
                                                                       (const char **) options,
                                                                       fstype,
                                                                       (const char **) fsoptions,
                                                                       op_create_partition_cb,
                                                                       data);

        while (n >= 0)
                g_free (fsoptions[n--]);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDevicePartitionModifyCompletedFunc callback;
        gpointer user_data;
} PartitionModifyData;

static void
op_partition_modify_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        PartitionModifyData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_partition_modify (GduDevice                             *device,
                                const char                            *type,
                                const char                            *label,
                                char                                 **flags,
                                GduDevicePartitionModifyCompletedFunc  callback,
                                gpointer                               user_data)
{
        PartitionModifyData *data;

        data = g_new0 (PartitionModifyData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_partition_modify_async (device->priv->proxy,
                                                                       type,
                                                                       label,
                                                                       (const char **) flags,
                                                                       op_partition_modify_cb,
                                                                       data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDevicePartitionTableCreateCompletedFunc callback;
        gpointer user_data;
} CreatePartitionTableData;

static void
op_create_partition_table_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        CreatePartitionTableData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_partition_table_create (GduDevice                                  *device,
                                      const char                                 *scheme,
                                      const char                                 *secure_erase,
                                      GduDevicePartitionTableCreateCompletedFunc  callback,
                                      gpointer                                    user_data)
{
        int n;
        char *options[16];
        CreatePartitionTableData *data;

        data = g_new0 (CreatePartitionTableData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        n = 0;
        if (secure_erase != NULL && strlen (secure_erase) > 0) {
                options[n++] = g_strdup_printf ("erase=%s", secure_erase);
        }
        options[n] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_partition_table_create_async (device->priv->proxy,
                                                                             scheme,
                                                                             (const char **) options,
                                                                             op_create_partition_table_cb,
                                                                             data);

        while (n >= 0)
                g_free (options[n--]);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceEncryptedUnlockCompletedFunc callback;
        gpointer user_data;
} UnlockData;

static void
op_unlock_encrypted_cb (DBusGProxy *proxy, char *cleartext_object_path, GError *error, gpointer user_data)
{
        UnlockData *data = user_data;

        if (error != NULL) {
                g_warning ("op_unlock_encrypted_cb failed: %s", error->message);
                data->callback (data->device, NULL, error, data->user_data);
        } else {
                g_print ("yay cleartext object is at '%s'\n", cleartext_object_path);
                data->callback (data->device, cleartext_object_path, error, data->user_data);
        }
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_encrypted_unlock (GduDevice *device,
                                const char *secret,
                                GduDeviceEncryptedUnlockCompletedFunc callback,
                                gpointer user_data)
{
        UnlockData *data;
        char *options[16];
        options[0] = NULL;

        data = g_new0 (UnlockData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_encrypted_unlock_async (device->priv->proxy,
                                                                       secret,
                                                                       (const char **) options,
                                                                       op_unlock_encrypted_cb,
                                                                       data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;

        GduDeviceEncryptedChangePassphraseCompletedFunc callback;
        gpointer user_data;
} ChangeSecretData;

static void
op_change_secret_for_encrypted_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        ChangeSecretData *data = user_data;

        if (error != NULL) {
                g_warning ("op_change_secret_for_encrypted_cb failed: %s", error->message);
                data->callback (data->device, error, data->user_data);
        } else {
                data->callback (data->device, error, data->user_data);
        }
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_encrypted_change_passphrase (GduDevice   *device,
                                           const char  *old_secret,
                                           const char  *new_secret,
                                           GduDeviceEncryptedChangePassphraseCompletedFunc callback,
                                           gpointer user_data)
{
        ChangeSecretData *data;

        data = g_new0 (ChangeSecretData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_encrypted_change_passphrase_async (device->priv->proxy,
                                                                                  old_secret,
                                                                                  new_secret,
                                                                                  op_change_secret_for_encrypted_cb,
                                                                                  data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceEncryptedLockCompletedFunc callback;
        gpointer user_data;
} LockEncryptedData;

static void
op_lock_encrypted_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        LockEncryptedData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_encrypted_lock (GduDevice                           *device,
                              GduDeviceEncryptedLockCompletedFunc  callback,
                              gpointer                             user_data)
{
        char *options[16];
        LockEncryptedData *data;

        data = g_new0 (LockEncryptedData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        options[0] = NULL;
        org_freedesktop_DeviceKit_Disks_Device_encrypted_lock_async (device->priv->proxy,
                                                                     (const char **) options,
                                                                     op_lock_encrypted_cb,
                                                                     data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceFilesystemSetLabelCompletedFunc callback;
        gpointer user_data;
} FilesystemSetLabelData;

static void
op_change_filesystem_label_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        FilesystemSetLabelData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_filesystem_set_label (GduDevice                                *device,
                                    const char                               *new_label,
                                    GduDeviceFilesystemSetLabelCompletedFunc  callback,
                                    gpointer                                  user_data)
{
        FilesystemSetLabelData *data;

        data = g_new0 (FilesystemSetLabelData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_filesystem_set_label_async (device->priv->proxy,
                                                                           new_label,
                                                                           op_change_filesystem_label_cb,
                                                                           data);
}
/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceDriveSmartRefreshDataCompletedFunc callback;
        gpointer user_data;
} RetrieveSmartDataData;

static void
op_retrieve_smart_data_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        RetrieveSmartDataData *data = user_data;

        if (error != NULL) {
                /*g_warning ("op_retrieve_smart_data_cb failed: %s", error->message);*/
                data->callback (data->device, error, data->user_data);
        } else {
                data->callback (data->device, NULL, data->user_data);
        }
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_drive_smart_refresh_data (GduDevice                                  *device,
                                     GduDeviceDriveSmartRefreshDataCompletedFunc callback,
                                     gpointer                                    user_data)
{
        RetrieveSmartDataData *data;
        char *options[16];

        options[0] = NULL;

        data = g_new0 (RetrieveSmartDataData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_drive_smart_refresh_data_async (device->priv->proxy,
                                                                               (const char **) options,
                                                                               op_retrieve_smart_data_cb,
                                                                               data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceDriveSmartInitiateSelftestCompletedFunc callback;
        gpointer user_data;
} DriveSmartInitiateSelftestData;

static void
op_run_smart_selftest_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        DriveSmartInitiateSelftestData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_drive_smart_initiate_selftest (GduDevice                                        *device,
                                             const char                                       *test,
                                             gboolean                                          captive,
                                             GduDeviceDriveSmartInitiateSelftestCompletedFunc  callback,
                                             gpointer                                          user_data)
{
        DriveSmartInitiateSelftestData *data;

        data = g_new0 (DriveSmartInitiateSelftestData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_drive_smart_initiate_selftest_async (device->priv->proxy,
                                                                                    test,
                                                                                    captive,
                                                                                    op_run_smart_selftest_cb,
                                                                                    data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceLinuxMdStopCompletedFunc callback;
        gpointer user_data;
} LinuxMdStopData;

static void
op_stop_linux_md_array_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        LinuxMdStopData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_linux_md_stop (GduDevice                         *device,
                             GduDeviceLinuxMdStopCompletedFunc  callback,
                             gpointer                           user_data)
{
        char *options[16];
        LinuxMdStopData *data;

        data = g_new0 (LinuxMdStopData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        options[0] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_linux_md_stop_async (device->priv->proxy,
                                                                    (const char **) options,
                                                                    op_stop_linux_md_array_cb,
                                                                    data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceLinuxMdAddComponentCompletedFunc callback;
        gpointer user_data;
} LinuxMdAddComponentData;

static void
op_add_component_to_linux_md_array_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        LinuxMdAddComponentData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_linux_md_add_component (GduDevice                                 *device,
                                      const char                                *component_objpath,
                                      GduDeviceLinuxMdAddComponentCompletedFunc  callback,
                                      gpointer                                   user_data)
{
        char *options[16];
        LinuxMdAddComponentData *data;

        data = g_new0 (LinuxMdAddComponentData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        options[0] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_linux_md_add_component_async (
                device->priv->proxy,
                component_objpath,
                (const char **) options,
                op_add_component_to_linux_md_array_cb,
                data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceLinuxMdRemoveComponentCompletedFunc callback;
        gpointer user_data;
} LinuxMdRemoveComponentData;

static void
op_remove_component_from_linux_md_array_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        LinuxMdRemoveComponentData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_linux_md_remove_component (GduDevice                                    *device,
                                         const char                                   *component_objpath,
                                         const char                                   *secure_erase,
                                         GduDeviceLinuxMdRemoveComponentCompletedFunc  callback,
                                         gpointer                                      user_data)
{
        int n;
        char *options[16];
        LinuxMdRemoveComponentData *data;

        data = g_new0 (LinuxMdRemoveComponentData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        n = 0;
        if (secure_erase != NULL && strlen (secure_erase) > 0) {
                options[n++] = g_strdup_printf ("erase=%s", secure_erase);
        }
        options[n] = NULL;

        org_freedesktop_DeviceKit_Disks_Device_linux_md_remove_component_async (
                device->priv->proxy,
                component_objpath,
                (const char **) options,
                op_remove_component_from_linux_md_array_cb,
                g_object_ref (device));

        while (n >= 0)
                g_free (options[n--]);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceCancelJobCompletedFunc callback;
        gpointer user_data;
} CancelJobData;

static void
op_cancel_job_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
        CancelJobData *data = user_data;
        if (data->callback != NULL)
                data->callback (data->device, error, data->user_data);
        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_op_cancel_job (GduDevice *device, GduDeviceCancelJobCompletedFunc callback, gpointer user_data)
{
        CancelJobData *data;

        data = g_new0 (CancelJobData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_Device_job_cancel_async (device->priv->proxy,
                                                                 op_cancel_job_cb,
                                                                 data);
}

/* -------------------------------------------------------------------------------- */

typedef struct {
        GduDevice *device;
        GduDeviceDriveSmartGetHistoricalDataCompletedFunc callback;
        gpointer user_data;
} DriveSmartGetHistoricalDataData;

static GList *
op_smart_historical_data_compute_ret (GPtrArray *historical_data)
{
        GList *ret;
        int n;

        ret = NULL;
        for (n = 0; n < (int) historical_data->len; n++) {
                ret = g_list_prepend (ret, _gdu_smart_data_new (historical_data->pdata[n]));
        }
        ret = g_list_reverse (ret);
        return ret;
}

static void
op_smart_historical_data_cb (DBusGProxy *proxy, GPtrArray *historical_data, GError *error, gpointer user_data)
{
        DriveSmartGetHistoricalDataData *data = user_data;
        GList *ret;

        ret = NULL;
        if (historical_data != NULL && error == NULL)
                ret = op_smart_historical_data_compute_ret (historical_data);

        if (data->callback == NULL)
                data->callback (data->device, ret, error, data->user_data);

        g_object_unref (data->device);
        g_free (data);
}

void
gdu_device_drive_smart_get_historical_data (GduDevice                                         *device,
                                            GduDeviceDriveSmartGetHistoricalDataCompletedFunc  callback,
                                            gpointer                                           user_data)
{
        DriveSmartGetHistoricalDataData *data;

        data = g_new0 (DriveSmartGetHistoricalDataData, 1);
        data->device = g_object_ref (device);
        data->callback = callback;
        data->user_data = user_data;

        /* TODO: since, until */
        org_freedesktop_DeviceKit_Disks_Device_drive_smart_get_historical_data_async (device->priv->proxy,
                                                                                      0,
                                                                                      0,
                                                                                      op_smart_historical_data_cb,
                                                                                      data);
}

GList *
gdu_device_drive_smart_get_historical_data_sync (GduDevice  *device,
                                                 GError    **error)
{
        GList *ret;
        GPtrArray *historical_data;

        ret = NULL;
        /* TODO: since, until */
        if (!org_freedesktop_DeviceKit_Disks_Device_drive_smart_get_historical_data (device->priv->proxy,
                                                                                     0,
                                                                                     0,
                                                                                     &historical_data,
                                                                                     error))
                goto out;

        ret = op_smart_historical_data_compute_ret (historical_data);
out:
        return ret;
}
