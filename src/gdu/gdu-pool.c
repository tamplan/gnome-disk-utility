/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-pool.c
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
#include <dbus/dbus-glib.h>
#include <string.h>
#include <stdlib.h>

#include "gdu-pool.h"
#include "gdu-drive.h"
#include "gdu-activatable-drive.h"
#include "gdu-volume.h"
#include "gdu-volume-hole.h"
#include "gdu-private.h"

#include "devkit-disks-daemon-glue.h"
#include "gdu-marshal.h"

/**
 * SECTION:gdu-pool
 * @title: GduPool
 * @short_description: Enumerate and monitor storage devices
 *
 * The #GduPool object represents a connection to the DeviceKit-disks daemon.
 */

enum {
        DEVICE_ADDED,
        DEVICE_REMOVED,
        DEVICE_CHANGED,
        DEVICE_JOB_CHANGED,
        PRESENTABLE_ADDED,
        PRESENTABLE_REMOVED,
        PRESENTABLE_CHANGED,
        PRESENTABLE_JOB_CHANGED,
        LAST_SIGNAL,
};

static GObjectClass *parent_class = NULL;
static guint signals[LAST_SIGNAL] = { 0 };

struct _GduPoolPrivate
{
        DBusGConnection *bus;
        DBusGProxy *proxy;

        char *daemon_version;
        gboolean supports_luks_devices;
        GList *known_filesystems;

        GHashTable *devices;            /* object path -> GduDevice* */
        GHashTable *volumes;            /* object path -> GduVolume* */
        GHashTable *drives;             /* object path -> GduDrive* */

        GList *activatable_drives;

        GHashTable *drive_holes;         /* object path -> GList of GduVolumeHole* */
        GHashTable *drive_unrecognized;  /* object path -> GduVolume* */

        GList *presentables;
};

G_DEFINE_TYPE (GduPool, gdu_pool, G_TYPE_OBJECT);

static void
remove_holes (GduPool *pool, const char *drive_object_path)
{
        GList *l;
        GList *holes;

        holes = g_hash_table_lookup (pool->priv->drive_holes, drive_object_path);
        for (l = holes; l != NULL; l = l->next) {
                GduPresentable *presentable = l->data;
                pool->priv->presentables = g_list_remove (pool->priv->presentables, presentable);
                g_signal_emit (pool, signals[PRESENTABLE_REMOVED], 0, presentable);
                g_object_unref (presentable);
        }
        g_hash_table_remove (pool->priv->drive_holes, drive_object_path);
}

static void
free_list_of_holes (gpointer data)
{
        GList *l = data;
        g_list_foreach (l, (GFunc) g_object_unref, NULL);
        g_list_free (l);
}

static void
gdu_pool_finalize (GduPool *pool)
{
        dbus_g_connection_unref (pool->priv->bus);
        g_object_unref (pool->priv->proxy);
        g_hash_table_unref (pool->priv->devices);
        g_hash_table_unref (pool->priv->volumes);
        g_hash_table_unref (pool->priv->drives);

        g_list_foreach (pool->priv->activatable_drives, (GFunc) g_object_unref, NULL);
        g_list_free (pool->priv->activatable_drives);

        g_hash_table_unref (pool->priv->drive_holes);
        g_hash_table_unref (pool->priv->drive_unrecognized);

        g_list_foreach (pool->priv->presentables, (GFunc) g_object_unref, NULL);
        g_list_free (pool->priv->presentables);

        g_free (pool->priv->daemon_version);

        g_list_foreach (pool->priv->known_filesystems, (GFunc) g_object_unref, NULL);
        g_list_free (pool->priv->known_filesystems);

        if (G_OBJECT_CLASS (parent_class)->finalize)
                (* G_OBJECT_CLASS (parent_class)->finalize) (G_OBJECT (pool));
}

static void
gdu_pool_class_init (GduPoolClass *klass)
{
        GObjectClass *obj_class = (GObjectClass *) klass;

        parent_class = g_type_class_peek_parent (klass);

        obj_class->finalize = (GObjectFinalizeFunc) gdu_pool_finalize;

        /**
         * GduPool::device-added
         * @pool: The #GduPool emitting the signal.
         * @device: The #GduDevice that was added.
         *
         * Emitted when @device is added to @pool.
         **/
        signals[DEVICE_ADDED] =
                g_signal_new ("device-added",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, device_added),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_DEVICE);

        /**
         * GduPool::device-removed
         * @pool: The #GduPool emitting the signal.
         * @device: The #GduDevice that was removed.
         *
         * Emitted when @device is removed from @pool. Recipients
         * should release references to @device.
         **/
        signals[DEVICE_REMOVED] =
                g_signal_new ("device-removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, device_removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_DEVICE);

        /**
         * GduPool::device-changed
         * @pool: The #GduPool emitting the signal.
         * @device: A #GduDevice.
         *
         * Emitted when @device is changed.
         **/
        signals[DEVICE_CHANGED] =
                g_signal_new ("device-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, device_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_DEVICE);

        /**
         * GduPool::device-job-changed
         * @pool: The #GduPool emitting the signal.
         * @device: A #GduDevice.
         *
         * Emitted when job status on @device changes.
         **/
        signals[DEVICE_JOB_CHANGED] =
                g_signal_new ("device-job-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, device_job_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_DEVICE);

        /**
         * GduPool::presentable-added
         * @pool: The #GduPool emitting the signal.
         * @presentable: The #GduPresentable that was added.
         *
         * Emitted when @presentable is added to @pool.
         **/
        signals[PRESENTABLE_ADDED] =
                g_signal_new ("presentable-added",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, presentable_added),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_PRESENTABLE);

        /**
         * GduPool::presentable-removed
         * @pool: The #GduPool emitting the signal.
         * @presentable: The #GduPresentable that was removed.
         *
         * Emitted when @presentable is removed from @pool. Recipients
         * should release references to @presentable.
         **/
        signals[PRESENTABLE_REMOVED] =
                g_signal_new ("presentable-removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, presentable_removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_PRESENTABLE);

        /**
         * GduPool::presentable-changed
         * @pool: The #GduPool emitting the signal.
         * @presentable: A #GduPresentable.
         *
         * Emitted when @presentable changes.
         **/
        signals[PRESENTABLE_CHANGED] =
                g_signal_new ("presentable-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, presentable_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_PRESENTABLE);

        /**
         * GduPool::presentable-job-changed
         * @pool: The #GduPool emitting the signal.
         * @presentable: A #GduPresentable.
         *
         * Emitted when job status on @presentable changes.
         **/
        signals[PRESENTABLE_CHANGED] =
                g_signal_new ("presentable-job-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GduPoolClass, presentable_job_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1,
                              GDU_TYPE_PRESENTABLE);
}

static void
gdu_pool_init (GduPool *pool)
{
        pool->priv = g_new0 (GduPoolPrivate, 1);
}

typedef struct {
        int number;
        guint64 offset;
        guint64 size;
} PartEntry;

static int
part_entry_compare (PartEntry *pa, PartEntry *pb, gpointer user_data)
{
        if (pa->offset > pb->offset)
                return 1;
        if (pa->offset < pb->offset)
                return -1;
        return 0;
}

static void
add_holes (GduPool *pool,
           GduDrive *drive,
           GduPresentable *presentable,
           gboolean ignore_logical,
           guint64 start,
           guint64 size)
{
        int n, num_entries;
        int max_number;
        guint64 *offsets;
        guint64 *sizes;
        GduDevice *drive_device;
        GduVolumeHole *hole;
        PartEntry *entries;
        guint64 cursor;
        guint64 gap_size;
        guint64 gap_position;
        const char *scheme;

        drive_device = gdu_presentable_get_device (GDU_PRESENTABLE (drive));

        /* may be an activatable drive that isn't activated yet */
        if (drive_device == NULL)
                goto out;

        /* no point if adding holes if there's no media */
        if (!gdu_device_is_media_available (drive_device))
                goto out;

        /* neither if the media isn't partitioned */
        if (!gdu_device_is_partition_table (drive_device))
                goto out;

        /*g_print ("Adding holes for %s between %lld and %lld (ignore_logical=%d)\n",
                 gdu_device_get_device_file (drive_device),
                 start,
                 start + size,
                 ignore_logical);*/

        offsets = (guint64*) ((gdu_device_partition_table_get_offsets (drive_device))->data);
        sizes = (guint64*) ((gdu_device_partition_table_get_sizes (drive_device))->data);
        max_number = gdu_device_partition_table_get_max_number (drive_device);
        scheme = gdu_device_partition_table_get_scheme (drive_device);

        entries = g_new0 (PartEntry, max_number);
        for (n = 0, num_entries = 0; n < max_number; n++) {
                /* ignore unused partition table entries */
                if (offsets[n] == 0)
                        continue;

                /* only consider partitions in the given space */
                if (offsets[n] <= start)
                        continue;
                if (offsets[n] >= start + size)
                        continue;

                /* ignore logical partitions if requested */
                if (ignore_logical) {
                        if (strcmp (scheme, "mbr") == 0 && n >= 4)
                                continue;
                }

                entries[num_entries].number = n + 1;
                entries[num_entries].offset = offsets[n];
                entries[num_entries].size = sizes[n];
                num_entries++;
                //g_print ("%d: offset=%lld size=%lld\n", entries[n].number, entries[n].offset, entries[n].size);
        }
        entries = g_realloc (entries, num_entries * sizeof (PartEntry));

        g_qsort_with_data (entries, num_entries, sizeof (PartEntry), (GCompareDataFunc) part_entry_compare, NULL);

        for (n = 0, cursor = start; n <= num_entries; n++) {
                if (n < num_entries) {

                        /*g_print (" %d: offset=%lldMB size=%lldMB\n",
                                 entries[n].number,
                                 entries[n].offset / (1000 * 1000),
                                 entries[n].size / (1000 * 1000));*/


                        gap_size = entries[n].offset - cursor;
                        gap_position = entries[n].offset - gap_size;
                        cursor = entries[n].offset + entries[n].size;
                } else {
                        /* trailing free space */
                        gap_size = start + size - cursor;
                        gap_position = start + size - gap_size;
                }

                /* ignore unallocated space that is less than 1% of the drive */
                if (gap_size >= gdu_device_get_size (drive_device) / 100) {
                        GList *hole_list;
                        char *orig_key;

                        /*g_print ("  -> adding gap=%lldMB @ %lldMB\n",
                                 gap_size / (1000 * 1000),
                                 gap_position  / (1000 * 1000));*/

                        hole = _gdu_volume_hole_new (pool, gap_position, gap_size, presentable);
                        hole_list = NULL;
                        if (g_hash_table_lookup_extended (pool->priv->drive_holes,
                                                          gdu_device_get_object_path (drive_device),
                                                          (gpointer *) &orig_key,
                                                          (gpointer *) &hole_list)) {
                                g_hash_table_steal (pool->priv->drive_holes, orig_key);
                                g_free (orig_key);
                        }
                        hole_list = g_list_prepend (hole_list, g_object_ref (hole));
                        /*g_print ("hole list now len=%d for %s\n",
                                   g_list_length (hole_list),
                                   gdu_device_get_object_path (drive_device));*/
                        g_hash_table_insert (pool->priv->drive_holes,
                                             g_strdup (gdu_device_get_object_path (drive_device)),
                                             hole_list);
                        pool->priv->presentables = g_list_prepend (pool->priv->presentables, GDU_PRESENTABLE (hole));
                        g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (hole));
                }

        }

        g_free (entries);
out:
        g_object_unref (drive_device);
}

/* typically called on 'change' event for a drive */
static void
update_holes (GduPool *pool, const char *drive_object_path)
{
        GduDrive *drive;
        GduDevice *drive_device;
        GList *l;

        /* first remove all existing holes */
        remove_holes (pool, drive_object_path);

        /* then add new holes */
        drive = g_hash_table_lookup (pool->priv->drives, drive_object_path);
        drive_device = gdu_presentable_get_device (GDU_PRESENTABLE (drive));

        /* may be an activatable drive that isn't activated yet */
        if (drive_device == NULL)
                goto out;

        /* add holes between primary partitions */
        add_holes (pool,
                   drive,
                   GDU_PRESENTABLE (drive),
                   TRUE,
                   0,
                   gdu_device_get_size (drive_device));

        /* add holes between logical partitions residing in extended partitions */
        for (l = pool->priv->presentables; l != NULL; l = l->next) {
                GduPresentable *presentable = l->data;

                if (gdu_presentable_get_enclosing_presentable (presentable) == GDU_PRESENTABLE (drive)) {
                        GduDevice *partition_device;

                        partition_device = gdu_presentable_get_device (presentable);
                        if (partition_device != NULL &&
                            gdu_device_is_partition (partition_device)) {
                                int partition_type;
                                partition_type = strtol (gdu_device_partition_get_type (partition_device), NULL, 0);
                                if (partition_type == 0x05 ||
                                    partition_type == 0x0f ||
                                    partition_type == 0x85) {
                                        add_holes (pool,
                                                   drive,
                                                   presentable,
                                                   FALSE,
                                                   gdu_device_partition_get_offset (partition_device),
                                                   gdu_device_partition_get_size (partition_device));

                                }
                        }
                        if (partition_device != NULL)
                                g_object_unref (partition_device);
                }
        }

out:
        if (drive_device != NULL)
                g_object_unref (drive_device);
}

static void
update_whole_disk (GduPool *pool, GduDrive *drive, GduDevice *device, const char *object_path)
{
        const char *usage;

        usage = gdu_device_id_get_usage (device);

        /* for whole disk devices we may need to remove volumes if
         *  - media is unavailable; or
         *  - if the media is now partitioned
         */
        if (!gdu_device_is_media_available (device) ||
            gdu_device_is_partition_table (device)) {
                GduVolume *volume;

                /* remove volumes we may have */
                volume = g_hash_table_lookup (pool->priv->volumes, object_path);
                if (volume != NULL) {
                        g_hash_table_remove (pool->priv->volumes, object_path);
                        pool->priv->presentables = g_list_remove (pool->priv->presentables,
                                                                  GDU_PRESENTABLE (volume));
                        g_signal_emit (pool, signals[PRESENTABLE_REMOVED], 0, GDU_PRESENTABLE (volume));
                        g_object_unref (GDU_PRESENTABLE (volume));
                }
        }

        /* conversely, we need to add a volume (unless it's added already) if
         *  - media is available; and
         *  - it's not partitioned; and
         */
        if (gdu_device_is_media_available (device) &&
            !gdu_device_is_partition_table (device)) {
                GduVolume *volume;

                /* add volume for whole disk device if it's not there already */
                volume = g_hash_table_lookup (pool->priv->volumes, object_path);
                if (volume == NULL) {
                        volume = _gdu_volume_new_from_device (pool, device, GDU_PRESENTABLE (drive));
                        g_hash_table_insert (pool->priv->volumes,
                                             g_strdup (object_path),
                                             g_object_ref (volume));
                        pool->priv->presentables = g_list_prepend (pool->priv->presentables,
                                                                   GDU_PRESENTABLE (volume));
                        g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (volume));
                }
        }
}


static GduActivatableDrive *
find_activatable_drive_for_linux_md_component (GduPool *pool, GduDevice *device)
{
        GList *l;
        GduActivatableDrive *ad;
        const char *uuid;

        ad = NULL;

        if (!gdu_device_is_linux_md_component (device)) {
                g_warning ("not linux md component");
                goto out;
        }

        uuid = gdu_device_linux_md_component_get_uuid (device);

        for (l = pool->priv->activatable_drives; l != NULL; l = l->next) {
                ad = l->data;

                if (_gdu_activatable_drive_has_uuid (ad, uuid))
                        goto out;

                if (_gdu_activatable_drive_device_references_slave (ad, device))
                        goto out;
        }

        ad = NULL;

out:
        return ad;
}

static void
ensure_activatable_drive_for_linux_md_component (GduPool *pool, GduDevice *device)
{
        GduActivatableDrive *activatable_drive;

        activatable_drive = find_activatable_drive_for_linux_md_component (pool, device);

        /* create it unless we have it already */
        if (activatable_drive == NULL) {
                activatable_drive = _gdu_activatable_drive_new (pool,
                                                                GDU_ACTIVATABLE_DRIVE_KIND_LINUX_MD);

                pool->priv->activatable_drives = g_list_prepend (pool->priv->activatable_drives,
                                                                 g_object_ref (activatable_drive));

                /* and we're part of the gang */
                pool->priv->presentables = g_list_prepend (pool->priv->presentables,
                                                           GDU_PRESENTABLE (activatable_drive));
                g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (activatable_drive));
        }

        /* add ourselves to the drive if we're not already part of it */
        if (!gdu_activatable_drive_has_slave (activatable_drive, device))
                _gdu_activatable_drive_add_slave (activatable_drive, device);
}

static GduActivatableDrive *
find_activatable_drive_for_linux_md_array (GduPool *pool, GduDevice *device)
{
        int n;
        GList *l;
        char **slaves;
        GduActivatableDrive *ad;

        ad = NULL;

        if (!gdu_device_is_linux_md (device)) {
                g_warning ("not linux md array");
                goto out;
        }

        /* TODO: if this is too slow we can optimize by having back-links on GduDevice */

        slaves = gdu_device_linux_md_get_slaves (device);
        for (n = 0; slaves[n] != NULL; n++) {
                GduDevice *slave;

                slave = g_hash_table_lookup (pool->priv->devices, slaves[n]);
                if (slave == NULL)
                        continue;

                for (l = pool->priv->activatable_drives; l != NULL; l = l->next) {
                        ad = l->data;
                        if (gdu_activatable_drive_has_slave (ad, slave))
                                goto out;
                }
        }

        ad = NULL;

out:
        return ad;
}

static GduDevice *
gdu_pool_add_device_by_object_path (GduPool *pool, const char *object_path)
{
        GduDevice *device;

        //g_print ("object path = %s\n", object_path);

        device = _gdu_device_new_from_object_path (pool, object_path);
        if (device != NULL) {
                g_hash_table_insert (pool->priv->devices, g_strdup (object_path), device);
                g_signal_emit (pool, signals[DEVICE_ADDED], 0, device);

                if (gdu_device_is_drive (device)) {
                        GduDrive *drive;

                        drive = NULL;

                        /* we may have a GduActivatableDrive for this sucker already */
                        if (gdu_device_is_linux_md (device)) {
                                GduActivatableDrive *activatable_drive;

                                activatable_drive = find_activatable_drive_for_linux_md_array (pool, device);
                                if (activatable_drive == NULL) {
                                        /* No activatable drive.. this can indeed happen when a new array is
                                         * created on the command line; then we get an add for the array
                                         * before the components.
                                         *
                                         * (The components will follow shortly though; this is because
                                         * DeviceKit-disks triggers a change even on all components of
                                         * an array on 'change', 'add' and 'remove' of said array).
                                         */

                                        activatable_drive = _gdu_activatable_drive_new (
                                                pool, GDU_ACTIVATABLE_DRIVE_KIND_LINUX_MD);

                                        pool->priv->activatable_drives = g_list_prepend (
                                                pool->priv->activatable_drives, g_object_ref (activatable_drive));

                                }

                                /* we've got a backing device */
                                _gdu_activatable_drive_set_device (activatable_drive, device);

                                drive = GDU_DRIVE (activatable_drive);

                                /* and now that we have a backing device.. we can part of the drives hash */
                                g_hash_table_insert (pool->priv->drives, g_strdup (object_path),
                                                     g_object_ref (drive));

                        }

                        /* otherwise do create a new GduDrive object */
                        if (drive == NULL) {
                                drive = _gdu_drive_new_from_device (pool, device);
                                g_hash_table_insert (pool->priv->drives, g_strdup (object_path), g_object_ref (drive));
                                pool->priv->presentables = g_list_prepend (pool->priv->presentables, GDU_PRESENTABLE (drive));
                                g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (drive));
                        }

                        if (gdu_device_is_partition_table (device)) {
                                /* Now create and add GVolumeHole objects representing space on the partitioned
                                 * device space not yet claimed by any partition. We don't add holes for empty
                                 * space on the extended partition; that's handled below.
                                 */
                                add_holes (pool,
                                           drive,
                                           GDU_PRESENTABLE (drive),
                                           TRUE,
                                           0,
                                           gdu_device_get_size (device));
                        } else {
                                /* it's a whole disk device */
                                update_whole_disk (pool, drive, device, object_path);
                        }
                }

                if (gdu_device_is_partition (device)) {
                        GduVolume *volume;
                        GduPresentable *enclosing_presentable;

                        /* make sure logical partitions are enclosed by the volume representing
                         * the extended partition
                         */
                        enclosing_presentable = NULL;
                        if (strcmp (gdu_device_partition_get_scheme (device), "mbr") == 0 &&
                            gdu_device_partition_get_number (device) > 4) {
                                GHashTableIter iter;
                                GduVolume *sibling;

                                /* Iterate over all volumes that also has the same drive and check
                                 * for partition type 0x05, 0x0f, 0x85
                                 *
                                 * TODO: would be nice to have DeviceKit-disks properties to avoid
                                 *       harcoding this for msdos only.
                                 */
                                g_hash_table_iter_init (&iter, pool->priv->volumes);
                                while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &sibling)) {
                                        int sibling_type;
                                        GduDevice *sibling_device;

                                        sibling_device = gdu_presentable_get_device (GDU_PRESENTABLE (sibling));
                                        if (!gdu_device_is_partition (sibling_device))
                                                continue;
                                        sibling_type = strtol (gdu_device_partition_get_type (sibling_device), NULL, 0);
                                        if (sibling_type == 0x05 ||
                                            sibling_type == 0x0f ||
                                            sibling_type == 0x85) {
                                                enclosing_presentable = GDU_PRESENTABLE (sibling);
                                                break;
                                        }
                                }

                                if (enclosing_presentable == NULL) {
                                        g_warning ("TODO: FIXME: handle logical partition %s arriving "
                                                   "before extended", gdu_device_get_device_file (device));
                                        /* .. at least we'll fall back to the drive for now ... */
                                }
                        }

                        if (enclosing_presentable == NULL) {
                                GduDrive *enclosing_drive;
                                enclosing_drive = g_hash_table_lookup (pool->priv->drives,
                                                                       gdu_device_partition_get_slave (device));

                                if (enclosing_drive != NULL)
                                        enclosing_presentable = GDU_PRESENTABLE (enclosing_drive);
                        }

                        /* add the partition */
                        volume = _gdu_volume_new_from_device (pool, device, enclosing_presentable);
                        g_hash_table_insert (pool->priv->volumes, g_strdup (object_path), g_object_ref (volume));
                        pool->priv->presentables = g_list_prepend (pool->priv->presentables, GDU_PRESENTABLE (volume));
                        g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (volume));

                        /* add holes for the extended partition */
                        if (strcmp (gdu_device_partition_get_scheme (device), "mbr") == 0) {
                                int partition_type;
                                partition_type = strtol (gdu_device_partition_get_type (device), NULL, 0);
                                if (partition_type == 0x05 ||
                                    partition_type == 0x0f ||
                                    partition_type == 0x85) {
                                        GduDrive *enclosing_drive;

                                        enclosing_drive = g_hash_table_lookup (
                                                pool->priv->drives,
                                                gdu_device_partition_get_slave (device));
                                        if (enclosing_drive != NULL) {
                                                /* Now create and add GVolumeHole objects representing space on
                                                 * the extended partition not yet claimed by any partition.
                                                 */
                                                add_holes (pool,
                                                           enclosing_drive,
                                                           GDU_PRESENTABLE (volume),
                                                           FALSE,
                                                           gdu_device_partition_get_offset (device),
                                                           gdu_device_partition_get_size (device));
                                        }
                                }
                        }
                }

                if (gdu_device_is_luks_cleartext (device)) {
                        const char *luks_cleartext_slave;
                        GduVolume *enclosing_volume;

                        luks_cleartext_slave = gdu_device_luks_cleartext_get_slave (device);
                        enclosing_volume = g_hash_table_lookup (pool->priv->volumes,
                                                                luks_cleartext_slave);
                        if (enclosing_volume != NULL) {
                                GduVolume *volume;

                                volume = _gdu_volume_new_from_device (pool, device, GDU_PRESENTABLE (enclosing_volume));
                                g_hash_table_insert (pool->priv->volumes,
                                                     g_strdup (object_path),
                                                     g_object_ref (volume));
                                pool->priv->presentables = g_list_prepend (pool->priv->presentables,
                                                                           GDU_PRESENTABLE (volume));
                                g_signal_emit (pool, signals[PRESENTABLE_ADDED], 0, GDU_PRESENTABLE (volume));
                        }
                }
        }

        /* create activatable drives for
         *  - Linux md RAID components
         *  - TODO: LVM Physical Volumes
         */
        if (gdu_device_is_linux_md_component (device)) {
                ensure_activatable_drive_for_linux_md_component (pool, device);
        }

        return device;
}

static void
device_added_signal_handler (DBusGProxy *proxy, const char *object_path, gpointer user_data)
{
        GduPool *pool = GDU_POOL (user_data);

        gdu_pool_add_device_by_object_path (pool, object_path);
}

static void
remove_activatable_drive_if_empty (GduPool *pool, GduActivatableDrive *activatable_drive)
{
        if (!_gdu_activatable_drive_is_device_set (activatable_drive) &&
            gdu_activatable_drive_get_num_slaves (activatable_drive) == 0) {
                /* no device and no slaves -> remove */
                pool->priv->activatable_drives= g_list_remove (
                        pool->priv->activatable_drives, activatable_drive);
                g_object_unref (activatable_drive);

                pool->priv->presentables = g_list_remove (
                        pool->priv->presentables,
                        GDU_PRESENTABLE (activatable_drive));

                g_signal_emit (pool, signals[PRESENTABLE_REMOVED], 0,
                               GDU_PRESENTABLE (activatable_drive));

                g_object_unref (activatable_drive);
        }
}

static void
device_removed_signal_handler (DBusGProxy *proxy, const char *object_path, gpointer user_data)
{
        GduPool *pool = GDU_POOL (user_data);
        GduDevice *device;

        if ((device = gdu_pool_get_by_object_path (pool, object_path)) != NULL) {
                GList *l;
                GList *ll;

                _gdu_device_removed (device);

                g_signal_emit (pool, signals[DEVICE_REMOVED], 0, device);
                g_hash_table_remove (pool->priv->devices, object_path);
                g_hash_table_remove (pool->priv->volumes, object_path);
                if (g_hash_table_remove (pool->priv->drives, object_path))
                        remove_holes (pool, object_path);

                /* set device to NULL for activatable drive if we go away */
                if (gdu_device_is_linux_md (device)) {
                        GduActivatableDrive *activatable_drive;

                        activatable_drive = find_activatable_drive_for_linux_md_array (pool, device);
                        if (activatable_drive != NULL) {
                                _gdu_activatable_drive_set_device (activatable_drive, NULL);
                                remove_activatable_drive_if_empty (pool, activatable_drive);
                        }
                }

                for (l = pool->priv->presentables; l != NULL; l = ll) {
                        GduPresentable *presentable = GDU_PRESENTABLE (l->data);
                        GduDevice *d;

                        ll = l->next;

                        d = gdu_presentable_get_device (presentable);
                        if (d == device) {
                                pool->priv->presentables = g_list_remove (pool->priv->presentables, presentable);
                                g_signal_emit (pool, signals[PRESENTABLE_REMOVED], 0, presentable);
                                g_object_unref (presentable);
                        }
                        if (d != NULL)
                                g_object_unref (d);
                }

                /* remove activatable drive if the last component goes away */
                if (gdu_device_is_linux_md_component (device)) {
                        GduActivatableDrive *activatable_drive;

                        activatable_drive = find_activatable_drive_for_linux_md_component (pool, device);
                        if (activatable_drive != NULL) {
                                _gdu_activatable_drive_remove_slave (activatable_drive, device);
                                remove_activatable_drive_if_empty (pool, activatable_drive);
                        }
                }

        } else {
                g_warning ("unknown device to remove, object_path='%s'", object_path);
        }
}

static void
device_changed_signal_handler (DBusGProxy *proxy, const char *object_path, gpointer user_data)
{
        GduPool *pool = GDU_POOL (user_data);
        GduDevice *device;

        if ((device = gdu_pool_get_by_object_path (pool, object_path)) != NULL) {
                GduDrive *drive;
                GduActivatableDrive *ad_before;

                ad_before = NULL;
                if (gdu_device_is_linux_md_component (device))
                        ad_before = find_activatable_drive_for_linux_md_component (pool, device);

                _gdu_device_changed (device);
                g_signal_emit_by_name (pool, "device-changed", device);

                if (ad_before != NULL) {
                        GduActivatableDrive *ad;

                        ad = find_activatable_drive_for_linux_md_component (pool, device);
                        if (ad != ad_before) {
                                /* no longer part of ad_before; remove */
                                _gdu_activatable_drive_remove_slave (ad_before, device);
                                remove_activatable_drive_if_empty (pool, ad_before);
                        }
                }

                /* device may have become a linux md component; make sure it's
                 * added to an Activatable Drive if so
                 */
                if (gdu_device_is_linux_md_component (device)) {
                        ensure_activatable_drive_for_linux_md_component (pool, device);
                }

                drive = g_hash_table_lookup (pool->priv->drives, object_path);
                if (drive != NULL)
                        update_holes (pool, object_path);

                if (drive != NULL) {
                        update_whole_disk (pool, drive, device, object_path);
                }

        } else {
                g_warning ("unknown device to on change, object_path='%s'", object_path);
        }

}

static void
device_job_changed_signal_handler (DBusGProxy *proxy,
                                   const char *object_path,
                                   gboolean    job_in_progress,
                                   const char *job_id,
                                   guint32     job_initiated_by_uid,
                                   gboolean    job_is_cancellable,
                                   int         job_num_tasks,
                                   int         job_cur_task,
                                   const char *job_cur_task_id,
                                   double      job_cur_task_percentage,
                                   gpointer user_data)
{
        GduPool *pool = GDU_POOL (user_data);
        GduDevice *device;

        if ((device = gdu_pool_get_by_object_path (pool, object_path)) != NULL) {
                _gdu_device_job_changed (device,
                                         job_in_progress,
                                         job_id,
                                         job_initiated_by_uid,
                                         job_is_cancellable,
                                         job_num_tasks,
                                         job_cur_task,
                                         job_cur_task_id,
                                         job_cur_task_percentage);
                g_signal_emit_by_name (pool, "device-job-changed", device);
        } else {
                g_warning ("unknown device to on job-change, object_path='%s'", object_path);
        }
}

static int
ptr_array_strcmp (const char **a, const char **b)
{
        /* kludge to get dm and md devices last */
        if (g_str_has_prefix (*a, "/devices/dm_") || g_str_has_prefix (*a, "/devices/md"))
                return 1;
        else if (g_str_has_prefix (*b, "/devices/dm_") || g_str_has_prefix (*b, "/devices/md"))
                return -1;
        else
                return strcmp (*a, *b);
}

static gboolean
get_properties (GduPool *pool)
{
        gboolean ret;
        GError *error;
        GHashTable *hash_table;
        DBusGProxy *prop_proxy;
        GValue *value;
        GPtrArray *known_filesystems_array;
        int n;

        ret = FALSE;

	prop_proxy = dbus_g_proxy_new_for_name (pool->priv->bus,
                                                "org.freedesktop.DeviceKit.Disks",
                                                "/",
                                                "org.freedesktop.DBus.Properties");
        error = NULL;
        if (!dbus_g_proxy_call (prop_proxy,
                                "GetAll",
                                &error,
                                G_TYPE_STRING,
                                "org.freedesktop.DeviceKit.Disks",
                                G_TYPE_INVALID,
                                dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
                                &hash_table,
                                G_TYPE_INVALID)) {
                g_warning ("Couldn't call GetAll() to get properties for /: %s", error->message);
                g_error_free (error);
                goto out;
        }

        value = g_hash_table_lookup (hash_table, "daemon-version");
        if (value == NULL) {
                g_warning ("No property 'daemon-version'");
                goto out;
        }
        pool->priv->daemon_version = g_strdup (g_value_get_string (value));

        value = g_hash_table_lookup (hash_table, "supports-luks-devices");
        if (value == NULL) {
                g_warning ("No property 'supports-luks-devices'");
                goto out;
        }
        pool->priv->supports_luks_devices = g_value_get_boolean (value);

        value = g_hash_table_lookup (hash_table, "known-filesystems");
        if (value == NULL) {
                g_warning ("No property 'known-filesystems'");
                goto out;
        }
        known_filesystems_array = g_value_get_boxed (value);
        pool->priv->known_filesystems = NULL;
        for (n = 0; n < (int) known_filesystems_array->len; n++) {
                pool->priv->known_filesystems = g_list_prepend (
                        pool->priv->known_filesystems,
                        _gdu_known_filesystem_new (known_filesystems_array->pdata[n]));
        }
        pool->priv->known_filesystems = g_list_reverse (pool->priv->known_filesystems);

        g_hash_table_unref (hash_table);

        ret = TRUE;
out:
        g_object_unref (prop_proxy);
        return ret;
}

/**
 * gdu_pool_new:
 *
 * Create a new #GduPool object.
 *
 * Returns: A #GduPool object. Caller must free this object using g_object_unref().
 **/
GduPool *
gdu_pool_new (void)
{
        int n;
        GPtrArray *devices;
        GduPool *pool;
        GError *error;

        pool = GDU_POOL (g_object_new (GDU_TYPE_POOL, NULL));

        error = NULL;
        pool->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (pool->priv->bus == NULL) {
                g_warning ("Couldn't connect to system bus: %s", error->message);
                g_error_free (error);
                goto error;
        }

        pool->priv->devices = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
        pool->priv->volumes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
        pool->priv->drives = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
        pool->priv->drive_holes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_list_of_holes);
        pool->priv->drive_unrecognized = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

        dbus_g_object_register_marshaller (
                gdu_marshal_VOID__STRING_BOOLEAN_STRING_UINT_BOOLEAN_INT_INT_STRING_DOUBLE,
                G_TYPE_NONE,
                G_TYPE_STRING,
                G_TYPE_BOOLEAN,
                G_TYPE_STRING,
                G_TYPE_UINT,
                G_TYPE_BOOLEAN,
                G_TYPE_INT,
                G_TYPE_INT,
                G_TYPE_STRING,
                G_TYPE_DOUBLE,
                G_TYPE_INVALID);

	pool->priv->proxy = dbus_g_proxy_new_for_name (pool->priv->bus,
                                                       "org.freedesktop.DeviceKit.Disks",
                                                       "/",
                                                       "org.freedesktop.DeviceKit.Disks");
        dbus_g_proxy_add_signal (pool->priv->proxy, "DeviceAdded", DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
        dbus_g_proxy_add_signal (pool->priv->proxy, "DeviceRemoved", DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
        dbus_g_proxy_add_signal (pool->priv->proxy, "DeviceChanged", DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
        dbus_g_proxy_add_signal (pool->priv->proxy,
                                 "DeviceJobChanged",
                                 DBUS_TYPE_G_OBJECT_PATH,
                                 G_TYPE_BOOLEAN,
                                 G_TYPE_STRING,
                                 G_TYPE_UINT,
                                 G_TYPE_BOOLEAN,
                                 G_TYPE_INT,
                                 G_TYPE_INT,
                                 G_TYPE_STRING,
                                 G_TYPE_DOUBLE,
                                 G_TYPE_INVALID);

        dbus_g_proxy_connect_signal (pool->priv->proxy, "DeviceAdded",
                                     G_CALLBACK (device_added_signal_handler), pool, NULL);
        dbus_g_proxy_connect_signal (pool->priv->proxy, "DeviceRemoved",
                                     G_CALLBACK (device_removed_signal_handler), pool, NULL);
        dbus_g_proxy_connect_signal (pool->priv->proxy, "DeviceChanged",
                                     G_CALLBACK (device_changed_signal_handler), pool, NULL);
        dbus_g_proxy_connect_signal (pool->priv->proxy, "DeviceJobChanged",
                                     G_CALLBACK (device_job_changed_signal_handler), pool, NULL);

        /* get the properties on the daemon object at / */
        if (!get_properties (pool)) {
                g_warning ("Couldn't get daemon properties");
                goto error;
        }

        /* prime the list of devices */
        error = NULL;
        if (!org_freedesktop_DeviceKit_Disks_enumerate_devices (pool->priv->proxy, &devices, &error)) {
                g_warning ("Couldn't enumerate devices: %s", error->message);
                g_error_free (error);
                goto error;
        }

        /* TODO: enumerate should return the tree order.. for now we just sort the list */
        g_ptr_array_sort (devices, (GCompareFunc) ptr_array_strcmp);

        for (n = 0; n < (int) devices->len; n++) {
                const char *object_path;
                GduDevice *device;

                object_path = devices->pdata[n];
                device = gdu_pool_add_device_by_object_path (pool, object_path);
        }
        g_ptr_array_foreach (devices, (GFunc) g_free, NULL);
        g_ptr_array_free (devices, TRUE);

        return pool;

error:
        g_object_unref (pool);
        return NULL;
}

/**
 * gdu_pool_get_by_object_path:
 * @pool: the device pool
 * @object_path: the D-Bus object path
 *
 * Looks up #GduDevice object for @object_path.
 *
 * Returns: A #GduDevice object for @object_path, otherwise
 * #NULL. Caller must unref this object using g_object_unref().
 **/
GduDevice *
gdu_pool_get_by_object_path (GduPool *pool, const char *object_path)
{
        GduDevice *device;

        device = g_hash_table_lookup (pool->priv->devices, object_path);
        if (device != NULL)
                return g_object_ref (device);
        else
                return NULL;
}

static void
get_devices_cb (gpointer key, gpointer value, gpointer user_data)
{
        GList **l = user_data;
        *l = g_list_prepend (*l, g_object_ref (GDU_DEVICE (value)));
}

/**
 * gdu_pool_get_devices:
 * @pool: A #GduPool.
 *
 * Get a list of all devices.
 *
 * Returns: A #GList of #GduDevice objects. Caller must free this
 * (unref all objects, then use g_list_free()).
 **/
GList *
gdu_pool_get_devices (GduPool *pool)
{
        GList *ret;
        ret = NULL;
        g_hash_table_foreach (pool->priv->devices, get_devices_cb, &ret);
        return ret;
}

/**
 * gdu_pool_get_presentables:
 * @pool: A #GduPool
 *
 * Get a list of all presentables.
 *
 * Returns: A #GList of objects implementing the #GduPresentable
 * interface. Caller must free this (unref all objects, then use
 * g_list_free()).
 **/
GList *
gdu_pool_get_presentables (GduPool *pool)
{
        GList *ret;
        ret = g_list_copy (pool->priv->presentables);
        g_list_foreach (ret, (GFunc) g_object_ref, NULL);
        return ret;
}

GList *
gdu_pool_get_enclosed_presentables (GduPool *pool, GduPresentable *presentable)
{
        GList *l;
        GList *ret;

        ret = NULL;
        for (l = pool->priv->presentables; l != NULL; l = l->next) {
                GduPresentable *p = l->data;
                GduPresentable *e;

                e = gdu_presentable_get_enclosing_presentable (p);
                if (e != NULL) {
                        if (e == presentable)
                                ret = g_list_prepend (ret, g_object_ref (p));

                        g_object_unref (e);
                }
        }

        return ret;
}

/**
 * gdu_pool_get_volume_by_device:
 * @pool: A #GduPool.
 * @device: A #GduDevice.
 *
 * Given @device, find the #GduVolume object for it.
 *
 * Returns: A #GduVolume object or #NULL if no @device isn't a
 * volume. Caller must free this object with g_object_unref().
 **/
GduPresentable *
gdu_pool_get_volume_by_device (GduPool *pool, GduDevice *device)
{
        GduVolume *volume;

        volume = g_hash_table_lookup (pool->priv->volumes, gdu_device_get_object_path (device));
        if (volume != NULL)
                return g_object_ref (volume);
        else
                return NULL;
}


/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
        GduPool *pool;
        GduPoolLinuxMdStartCompletedFunc callback;
        gpointer user_data;
} LinuxMdStartData;

static void
op_linux_md_start_cb (DBusGProxy *proxy, char *assembled_array_object_path, GError *error, gpointer user_data)
{
        LinuxMdStartData *data = user_data;
        _gdu_error_fixup (error);
        if (data->callback != NULL)
                data->callback (data->pool, assembled_array_object_path, error, data->user_data);
        g_object_unref (data->pool);
        g_free (data);
}

/**
 * gdu_pool_op_linux_md_start:
 * @pool: A #GduPool.
 * @component_objpaths: A #GPtrArray of object paths.
 * @callback: Callback function.
 * @user_data: User data to pass to @callback.
 *
 * Starts a Linux md Software Array.
 **/
void
gdu_pool_op_linux_md_start (GduPool *pool,
                            GPtrArray *component_objpaths,
                            GduPoolLinuxMdStartCompletedFunc callback,
                            gpointer user_data)
{
        LinuxMdStartData *data;
        char *options[16];

        options[0] = NULL;

        data = g_new0 (LinuxMdStartData, 1);
        data->pool = g_object_ref (pool);
        data->callback = callback;
        data->user_data = user_data;

        org_freedesktop_DeviceKit_Disks_linux_md_start_async (pool->priv->proxy,
                                                              component_objpaths,
                                                              (const char **) options,
                                                              op_linux_md_start_cb,
                                                              data);
}

/**
 * gdu_pool_get_daemon_version:
 * @pool: A #GduPool.
 *
 * Get the version of the DeviceKit-daemon on the system.
 *
 * Returns: The version of DeviceKit-disks daemon. Caller must free
 * this string using g_free().
 **/
char *
gdu_pool_get_daemon_version (GduPool *pool)
{
        return g_strdup (pool->priv->daemon_version);
}

/**
 * gdu_pool_get_known_filesystems:
 * @pool: A #GduPool.
 *
 * Get a list of file systems known to the DeviceKit-disks daemon.
 *
 * Returns: A #GList of #GduKnownFilesystem objects. Caller must free
 * this (unref all objects, then use g_list_free()).
 **/
GList *
gdu_pool_get_known_filesystems (GduPool *pool)
{
        GList *ret;
        ret = g_list_copy (pool->priv->known_filesystems);
        g_list_foreach (ret, (GFunc) g_object_ref, NULL);
        return ret;
}

/**
 * gdu_pool_get_known_filesystem_by_id:
 * @pool: A #GduPool.
 * @id: Identifier for the file system, e.g. <literal>ext3</literal> or <literal>vfat</literal>.
 *
 * Looks up a known file system by id.
 *
 * Returns: A #GduKnownFilesystem object or #NULL if file system
 * corresponding to @id exists. Caller must free this object using
 * g_object_unref().
 **/
GduKnownFilesystem *
gdu_pool_get_known_filesystem_by_id (GduPool *pool, const char *id)
{
        GList *l;
        GduKnownFilesystem *ret;

        ret = NULL;
        for (l = pool->priv->known_filesystems; l != NULL; l = l->next) {
                GduKnownFilesystem *kfs = GDU_KNOWN_FILESYSTEM (l->data);
                if (strcmp (gdu_known_filesystem_get_id (kfs), id) == 0) {
                        ret = g_object_ref (kfs);
                        goto out;
                }
        }
out:
        return ret;
}

/**
 * gdu_pool_supports_luks_devices:
 * @pool: A #GduPool.
 *
 * Determine if the DeviceKit-disks daemon supports LUKS encrypted
 * devices.
 *
 * Returns: #TRUE only if the daemon supports LUKS encrypted devices.
 **/
gboolean
gdu_pool_supports_luks_devices (GduPool *pool)
{
        return pool->priv->supports_luks_devices;
}

