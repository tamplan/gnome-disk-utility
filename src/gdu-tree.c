/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-tree.c
 *
 * Copyright (C) 2007 David Zeuthen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <config.h>
#include <glib/gi18n.h>
#include <string.h>

#include "gdu-main.h"
#include "gdu-tree.h"

enum
{
        ICON_COLUMN,
        TITLE_COLUMN,
        PRESENTABLE_OBJ_COLUMN,
        SORTNAME_COLUMN,
        N_COLUMNS
};

static gint
sort_iter_compare_func (GtkTreeModel *model,
                        GtkTreeIter  *a,
                        GtkTreeIter  *b,
                        gpointer      userdata)
{
        char *s1;
        char *s2;
        int result;

        gtk_tree_model_get (model, a, SORTNAME_COLUMN, &s1, -1);
        gtk_tree_model_get (model, b, SORTNAME_COLUMN, &s2, -1);
        if (s1 == NULL || s2 == NULL)
                result = 0;
        else
                result = g_ascii_strcasecmp (s1, s2);
        g_free (s2);
        g_free (s1);

        return result;
}

typedef struct {
        const char *udi;
        GduPresentable *presentable;
        gboolean found;
        GtkTreeIter iter;
} FIBDData;

static gboolean
find_iter_by_presentable_foreach (GtkTreeModel *model,
                             GtkTreePath *path,
                             GtkTreeIter *iter,
                             gpointer data)
{
        gboolean ret;
        GduPresentable *presentable = NULL;
        FIBDData *fibd_data = (FIBDData *) data;

        ret = FALSE;
        gtk_tree_model_get (model, iter, PRESENTABLE_OBJ_COLUMN, &presentable, -1);
        if (presentable == fibd_data->presentable) {
                fibd_data->found = TRUE;
                fibd_data->iter = *iter;
                ret = TRUE;
        }
        if (presentable != NULL)
                g_object_unref (presentable);

        return ret;
}


static gboolean
find_iter_by_presentable (GtkTreeStore *store, GduPresentable *presentable, GtkTreeIter *iter)
{
        FIBDData fibd_data;
        gboolean ret;

        fibd_data.presentable = presentable;
        fibd_data.found = FALSE;
        gtk_tree_model_foreach (GTK_TREE_MODEL (store), find_iter_by_presentable_foreach, &fibd_data);
        if (fibd_data.found) {
                if (iter != NULL)
                        *iter = fibd_data.iter;
                ret = TRUE;
        } else {
                ret = FALSE;
        }

        return ret;
}

static void
add_presentable_to_tree (GtkTreeView *tree_view, GduPresentable *presentable, GtkTreeIter *iter_out)
{
        GtkTreeIter  iter;
        GtkTreeIter  iter2;
        GtkTreeIter *parent_iter;
        GdkPixbuf   *pixbuf;
        const char  *object_path;
        char        *name;
        char        *icon_name;
        GtkTreeStore *store;
        GduDevice *device;
        GduPresentable *enclosing_presentable;

        device = NULL;

        store = GTK_TREE_STORE (gtk_tree_view_get_model (tree_view));

        /* check to see if presentable is already added */
        if (find_iter_by_presentable (store, presentable, NULL))
                goto out;

        /* set up parent relationship */
        parent_iter = NULL;
        enclosing_presentable = gdu_presentable_get_enclosing_presentable (presentable);
        if (enclosing_presentable != NULL) {
                if (find_iter_by_presentable (store, enclosing_presentable, &iter2)) {
                        parent_iter = &iter2;
                } else {
                        /* add parent if it's not already added */
                        add_presentable_to_tree (tree_view, enclosing_presentable, &iter2);
                        parent_iter = &iter2;
                }
                g_object_unref (enclosing_presentable);
        }

        device = gdu_presentable_get_device (presentable);

        object_path = gdu_device_get_object_path (device);

        name = g_strdup (object_path);
        icon_name = g_strdup ("drive-harddisk"); //gdu_info_provider_get_icon_name (device);

        /* compute the name */
        if (gdu_device_is_drive (device)) {
                const char *vendor;
                const char *model;
                guint64 size;
                gboolean is_removable;
                char *strsize;

                vendor = gdu_device_drive_get_vendor (device);
                model = gdu_device_drive_get_model (device);
                size = gdu_device_get_size (device);
                is_removable = gdu_device_is_removable (device);
                g_free (name);

                strsize = NULL;
                if (!is_removable && size > 0) {
                        strsize = gdu_util_get_size_for_display (size, FALSE);
                }

                if (strsize != NULL) {
                        name = g_strdup_printf ("%s %s %s",
                                                strsize,
                                                vendor != NULL ? vendor : "",
                                                model != NULL ? model : "");
                } else {
                        name = g_strdup_printf ("%s %s",
                                                vendor != NULL ? vendor : "",
                                                model != NULL ? model : "");
                }
                g_free (strsize);
        } if (gdu_device_is_partition (device)) {
                const char *label;

                label = gdu_device_id_get_label (device);
                if (label != NULL && strlen (label) > 0) {
                        name = g_strdup (label);
                } else {
                        char *strsize;
                        guint64 size;
                        size = gdu_device_get_size (device);
                        strsize = gdu_util_get_size_for_display (size, FALSE);
                        name = g_strdup_printf (_("%s Partition"), strsize);
                        g_free (strsize);
                }
        }


        pixbuf = NULL;
        if (icon_name != NULL) {
                pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
                                                   icon_name,
                                                   24,
                                                   0,
                                                   NULL);
        }

        gtk_tree_store_append (store, &iter, parent_iter);
        gtk_tree_store_set (store, &iter,
                            ICON_COLUMN, pixbuf,
                            TITLE_COLUMN, name,
                            PRESENTABLE_OBJ_COLUMN, presentable,
                            SORTNAME_COLUMN, object_path,
                            -1);

        if (iter_out != NULL)
                *iter_out = iter;

        g_free (name);
        g_free (icon_name);
        if (pixbuf != NULL)
                g_object_unref (pixbuf);

        if (parent_iter != NULL) {
                GtkTreePath *path;
                path = gtk_tree_model_get_path (GTK_TREE_MODEL (store), parent_iter);
                if (tree_view != NULL && path != NULL) {
                        gtk_tree_view_expand_row (tree_view, path, TRUE);
                        gtk_tree_path_free (path);
                }
        }
out:
        if (device != NULL)
                g_object_unref (device);
}

static void
device_tree_presentable_added (GduPool *pool, GduPresentable *presentable, gpointer user_data)
{
        GtkTreeView *tree_view = GTK_TREE_VIEW (user_data);
        add_presentable_to_tree (tree_view, presentable, NULL);
}

static void
device_tree_presentable_removed (GduPool *pool, GduPresentable *presentable, gpointer user_data)
{
        GtkTreeIter iter;
        GtkTreeStore *store;

        store = GTK_TREE_STORE (gtk_tree_view_get_model (GTK_TREE_VIEW (user_data)));
        if (find_iter_by_presentable (store, presentable, &iter)) {
                gtk_tree_store_remove (store, &iter);
        }
}

GtkTreeView *
gdu_tree_new (GduPool *pool)
{
        GtkCellRenderer *renderer;
        GtkTreeViewColumn *column;
        GtkTreeView *tree_view;
        GtkTreeStore *store;
        GList *presentables;
        GList *l;

        store = gtk_tree_store_new (N_COLUMNS,
                                    GDK_TYPE_PIXBUF,
                                    G_TYPE_STRING,
                                    GDU_TYPE_PRESENTABLE,
                                    G_TYPE_STRING);

        gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (store), SORTNAME_COLUMN, sort_iter_compare_func,
                                         NULL, NULL);
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store), SORTNAME_COLUMN, GTK_SORT_ASCENDING);

        tree_view = GTK_TREE_VIEW (gtk_tree_view_new_with_model (GTK_TREE_MODEL (store)));
        /* TODO: when GTK 2.12 is available... we can do this */
        /*gtk_tree_view_set_show_expanders (GTK_TREE_VIEW (tree), FALSE);*/

        column = gtk_tree_view_column_new ();
        gtk_tree_view_column_set_title (column, "Title");
        renderer = gtk_cell_renderer_pixbuf_new ();
        gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_column_set_attributes (column, renderer,
                                             "pixbuf", ICON_COLUMN,
                                             NULL);
        renderer = gtk_cell_renderer_text_new ();
        gtk_tree_view_column_pack_start (column, renderer, TRUE);
        gtk_tree_view_column_set_attributes (column, renderer,
                                             "text", TITLE_COLUMN,
                                             NULL);
        gtk_tree_view_append_column (tree_view, column);

        gtk_tree_view_set_headers_visible (tree_view, FALSE);

        presentables = gdu_pool_get_presentables (pool);
        for (l = presentables; l != NULL; l = l->next) {
                GduPresentable *presentable = GDU_PRESENTABLE (l->data);
                add_presentable_to_tree (tree_view, presentable, NULL);
                g_object_unref (presentable);
        }
        g_list_free (presentables);

        /* expand all rows after the treeview widget has been realized */
        g_signal_connect (tree_view, "realize", G_CALLBACK (gtk_tree_view_expand_all), NULL);

        /* add / remove rows when hal reports presentable add / remove */
        g_signal_connect (pool, "presentable_added", (GCallback) device_tree_presentable_added, tree_view);
        g_signal_connect (pool, "presentable_removed", (GCallback) device_tree_presentable_removed, tree_view);
        /* TODO: changed */

        return tree_view;
}

GduPresentable *
gdu_tree_get_selected_presentable (GtkTreeView *tree_view)
{
        GduPresentable *presentable;
        GtkTreePath *path;
        GtkTreeModel *presentable_tree_model;

        presentable = NULL;

        presentable_tree_model = gtk_tree_view_get_model (tree_view);
        gtk_tree_view_get_cursor (tree_view, &path, NULL);
        if (path != NULL) {
                GtkTreeIter iter;

                if (gtk_tree_model_get_iter (presentable_tree_model, &iter, path)) {

                        gtk_tree_model_get (presentable_tree_model, &iter,
                                            PRESENTABLE_OBJ_COLUMN,
                                            &presentable,
                                            -1);

                        if (presentable != NULL)
                                g_object_unref (presentable);
                }

                gtk_tree_path_free (path);
        }

        return presentable;
}

void
gdu_tree_select_presentable (GtkTreeView *tree_view, GduPresentable *presentable)
{
        GtkTreePath *path;
        GtkTreeModel *tree_model;
        GtkTreeIter iter;

        if (presentable == NULL)
                goto out;

        tree_model = gtk_tree_view_get_model (tree_view);
        if (!find_iter_by_presentable (GTK_TREE_STORE (tree_model), presentable, &iter))
                goto out;

        path = gtk_tree_model_get_path (tree_model, &iter);
        if (path == NULL)
                goto out;

        gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
        gtk_tree_path_free (path);
out:
        ;
}
