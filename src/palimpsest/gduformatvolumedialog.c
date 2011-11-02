/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2008-2011 Red Hat, Inc.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gduapplication.h"
#include "gduwindow.h"
#include "gduformatvolumedialog.h"
#include "gduvolumegrid.h"
#include "gduutils.h"

/* ---------------------------------------------------------------------------------------------------- */


typedef struct
{
  GduWindow *window;
  UDisksObject *object;
  UDisksBlock *block;

  GtkBuilder *builder;
  GtkWidget *dialog;
  GtkWidget *type_combobox;
  GtkWidget *name_entry;
  GtkWidget *filesystem_label;
  GtkWidget *filesystem_entry;
} FormatVolumeData;

static void
format_volume_data_free (FormatVolumeData *data)
{
  g_object_unref (data->window);
  g_object_unref (data->object);
  g_object_unref (data->block);
  if (data->dialog != NULL)
    {
      gtk_widget_hide (data->dialog);
      gtk_widget_destroy (data->dialog);
    }
  if (data->builder != NULL)
    g_object_unref (data->builder);
  g_free (data);
}

static void
format_volume_update (FormatVolumeData *data)
{
  gboolean show_filesystem = FALSE;
  gboolean can_proceed = FALSE;

  if (gtk_combo_box_get_active (GTK_COMBO_BOX (data->type_combobox)) == 3)
    {
      /* "Custom" selected */
      show_filesystem = TRUE;
      if (strlen (gtk_entry_get_text (GTK_ENTRY (data->filesystem_entry))) > 0)
        {
          /* TODO: maybe validate we know how to create this FS?
           * And also make "Name" + its entry insensitive if it doesn't support labels?
           */
          can_proceed = TRUE;
        }
    }
  else
    {
      can_proceed = TRUE;
    }

  if (show_filesystem)
    {
      gtk_widget_show (data->filesystem_label);
      gtk_widget_show (data->filesystem_entry);
    }
  else
    {
      gtk_widget_hide (data->filesystem_label);
      gtk_widget_hide (data->filesystem_entry);
    }

  gtk_dialog_set_response_sensitive (GTK_DIALOG (data->dialog), GTK_RESPONSE_OK, can_proceed);
}

static void
format_volume_property_changed (GObject     *object,
                                GParamSpec  *pspec,
                                gpointer     user_data)
{
  FormatVolumeData *data = user_data;
  format_volume_update (data);
}

static void
format_volume_populate (FormatVolumeData *data)
{
  /* Select "Compatible with all systems (FAT)" by default */
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->type_combobox), 1);

  /* Translators: this is the default name for the filesystem */
  gtk_entry_set_text (GTK_ENTRY (data->name_entry), _("New Volume"));

  /* Set 'swap' for the custom filesystem */
  gtk_entry_set_text (GTK_ENTRY (data->filesystem_entry), "swap");
}

void
gdu_format_volume_dialog_show (GduWindow    *window,
                               UDisksObject *object)
{
  FormatVolumeData *data;
  gint response;

  data = g_new0 (FormatVolumeData, 1);
  data->window = g_object_ref (window);
  data->object = g_object_ref (object);
  data->block = udisks_object_get_block (object);
  g_assert (data->block != NULL);

  data->dialog = gdu_application_new_widget (gdu_window_get_application (window),
                                             "format-volume-dialog.ui",
                                             "format-volume-dialog",
                                             &data->builder);
  data->type_combobox = GTK_WIDGET (gtk_builder_get_object (data->builder, "type-combobox"));
  g_signal_connect (data->type_combobox,
                    "notify::active", G_CALLBACK (format_volume_property_changed), data);
  data->name_entry = GTK_WIDGET (gtk_builder_get_object (data->builder, "name-entry"));
  g_signal_connect (data->name_entry,
                    "notify::text", G_CALLBACK (format_volume_property_changed), data);
  data->filesystem_label = GTK_WIDGET (gtk_builder_get_object (data->builder, "filesystem-label"));
  data->filesystem_entry = GTK_WIDGET (gtk_builder_get_object (data->builder, "filesystem-entry"));
  g_signal_connect (data->filesystem_entry,
                    "notify::text", G_CALLBACK (format_volume_property_changed), data);

  gtk_window_set_transient_for (GTK_WINDOW (data->dialog), GTK_WINDOW (window));
  gtk_dialog_set_default_response (GTK_DIALOG (data->dialog), GTK_RESPONSE_OK);

  format_volume_populate (data);
  format_volume_update (data);

  gtk_widget_show_all (data->dialog);
  gtk_widget_grab_focus (data->name_entry);

  /* TODO: do this async */
  response = gtk_dialog_run (GTK_DIALOG (data->dialog));
  if (response == GTK_RESPONSE_OK)
    {
      GVariantBuilder options_builder;
      const gchar *type;
      GError *error;

      gtk_widget_hide (data->dialog);
      if (!gdu_window_show_confirmation (window,
                                         _("Are you sure you want to format the volume?"),
                                         _("All data on the volume will be lost"),
                                         _("_Format")))
        goto out;

      g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
      if (strlen (gtk_entry_get_text (GTK_ENTRY (data->name_entry))) > 0)
      g_variant_builder_add (&options_builder, "{sv}", "label",
                             g_variant_new_string (gtk_entry_get_text (GTK_ENTRY (data->name_entry))));

      switch (gtk_combo_box_get_active (GTK_COMBO_BOX (data->type_combobox)))
        {
        case 0:
          type = "vfat";
          break;
        case 1:
          type = "ext4";
          break;
        case 2:
          /* TODO */
          type = "luks+ext4";
          break;
        case 3:
          type = gtk_entry_get_text (GTK_ENTRY (data->filesystem_entry));
          break;
        }

      error = NULL;
      if (!udisks_block_call_format_sync (data->block,
                                          type,
                                          g_variant_builder_end (&options_builder),
                                          NULL, /* GCancellable */
                                          &error))
        {
          gdu_window_show_error (window, _("Error formatting volume"), error);
          g_error_free (error);
        }
    }

 out:
  format_volume_data_free (data);
}