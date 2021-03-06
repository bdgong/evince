/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/*
 * Copyright (C) 2009-2010 Juanjo Marín <juanj.marin@juntadeandalucia.es>
 * Copyright (C) 2005, Teemu Tervo <teemu.tervo@gmx.net>
 * Copyright (C) 2016-2017, Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "comics-document.h"
#include "ev-document-misc.h"
#include "ev-file-helpers.h"
#include "ev-archive.h"

#define BLOCK_SIZE 10240

typedef struct _ComicsDocumentClass ComicsDocumentClass;

struct _ComicsDocumentClass
{
	EvDocumentClass parent_class;
};

struct _ComicsDocument
{
	EvDocument     parent_instance;
	EvArchive     *archive;
	gchar         *archive_path;
	gchar         *archive_uri;
	GPtrArray     *page_names;
};

static GSList* get_supported_image_extensions (void);

EV_BACKEND_REGISTER (ComicsDocument, comics_document)

static char **
comics_document_list (ComicsDocument *comics_document)
{
	char **ret = NULL;
	GPtrArray *array;

	if (!ev_archive_open_filename (comics_document->archive, comics_document->archive_path, NULL))
		goto out;

	array = g_ptr_array_new ();

	while (1) {
		const char *name;
		GError *error = NULL;

		if (!ev_archive_read_next_header (comics_document->archive, &error)) {
			if (error != NULL) {
				g_warning ("Fatal error handling archive: %s", error->message);
				g_error_free (error);
			}
			break;
		}

		name = ev_archive_get_entry_pathname (comics_document->archive);

		g_debug ("Adding '%s' to the list of files in the comics", name);
		g_ptr_array_add (array, g_strdup (name));
	}

	if (array->len == 0) {
		g_ptr_array_free (array, TRUE);
	} else {
		g_ptr_array_add (array, NULL);
		ret = (char **) g_ptr_array_free (array, FALSE);
	}

out:
	ev_archive_reset (comics_document->archive);
	return ret;
}

/* This function chooses the archive decompression support
 * book based on its mime type. */
static gboolean
comics_check_decompress_support	(gchar          *mime_type,
				 ComicsDocument *comics_document,
				 GError         **error)
{
	if (g_content_type_is_a (mime_type, "application/x-cbr") ||
	    g_content_type_is_a (mime_type, "application/x-rar")) {
		if (ev_archive_set_archive_type (comics_document->archive, EV_ARCHIVE_TYPE_RAR))
			return TRUE;
	} else if (g_content_type_is_a (mime_type, "application/x-cbz") ||
		   g_content_type_is_a (mime_type, "application/zip")) {
		if (ev_archive_set_archive_type (comics_document->archive, EV_ARCHIVE_TYPE_ZIP))
			return TRUE;
	} else if (g_content_type_is_a (mime_type, "application/x-cb7") ||
		   g_content_type_is_a (mime_type, "application/x-7z-compressed")) {
		if (ev_archive_set_archive_type (comics_document->archive, EV_ARCHIVE_TYPE_7Z))
			return TRUE;
	} else if (g_content_type_is_a (mime_type, "application/x-cbt") ||
		   g_content_type_is_a (mime_type, "application/x-tar")) {
		if (ev_archive_set_archive_type (comics_document->archive, EV_ARCHIVE_TYPE_TAR))
			return TRUE;
	} else {
		g_set_error (error,
			     EV_DOCUMENT_ERROR,
			     EV_DOCUMENT_ERROR_INVALID,
			     _("Not a comic book MIME type: %s"),
			     mime_type);
			     return FALSE;
	}
	g_set_error_literal (error,
			     EV_DOCUMENT_ERROR,
			     EV_DOCUMENT_ERROR_INVALID,
			     _("libarchive lacks support for this comic book’s "
			     "compression, please contact your distributor"));
	return FALSE;
}

static int
sort_page_names (gconstpointer a,
                 gconstpointer b)
{
  gchar *temp1, *temp2;
  gint ret;

  temp1 = g_utf8_collate_key_for_filename (* (const char **) a, -1);
  temp2 = g_utf8_collate_key_for_filename (* (const char **) b, -1);

  ret = strcmp (temp1, temp2);

  g_free (temp1);
  g_free (temp2);

  return ret;
}

static gboolean
comics_document_load (EvDocument *document,
		      const char *uri,
		      GError    **error)
{
	ComicsDocument *comics_document = COMICS_DOCUMENT (document);
	GSList *supported_extensions;
	gchar *mime_type;
	gchar **cb_files, *cb_file;
	int i;
	GError *err = NULL;
	GFile *file;

	file = g_file_new_for_uri (uri);
	comics_document->archive_path = g_file_get_path (file);
	g_object_unref (file);

	if (!comics_document->archive_path) {
		g_set_error_literal (error,
                                     EV_DOCUMENT_ERROR,
                                     EV_DOCUMENT_ERROR_INVALID,
                                     _("Can not get local path for archive"));
		return FALSE;
	}

	comics_document->archive_uri = g_strdup (uri);

	mime_type = ev_file_get_mime_type (uri, FALSE, &err);
	if (mime_type == NULL)
		return FALSE;

	if (!comics_check_decompress_support (mime_type, comics_document, error)) {
		g_free (mime_type);
		return FALSE;
	}
	g_free (mime_type);

	/* Get list of files in archive */
	cb_files = comics_document_list (comics_document);
	if (!cb_files) {
		g_set_error_literal (error,
                                     EV_DOCUMENT_ERROR,
                                     EV_DOCUMENT_ERROR_INVALID,
                                     _("File corrupted or no files in archive"));
		return FALSE;
	}

        comics_document->page_names = g_ptr_array_sized_new (64);

	supported_extensions = get_supported_image_extensions ();
	for (i = 0; cb_files[i] != NULL; i++) {
		cb_file = cb_files[i];
		gchar *suffix = g_strrstr (cb_file, ".");
		if (!suffix)
			continue;
		suffix = g_ascii_strdown (suffix + 1, -1);
		if (g_slist_find_custom (supported_extensions, suffix,
					 (GCompareFunc) strcmp) != NULL) {
                        g_ptr_array_add (comics_document->page_names,
                                         g_strstrip (g_strdup (cb_file)));
		}
		g_free (suffix);
	}
	g_strfreev (cb_files);
	g_slist_foreach (supported_extensions, (GFunc) g_free, NULL);
	g_slist_free (supported_extensions);

	if (comics_document->page_names->len == 0) {
		g_set_error (error,
			     EV_DOCUMENT_ERROR,
			     EV_DOCUMENT_ERROR_INVALID,
			     _("No images found in archive %s"),
			     uri);
		return FALSE;
	}

        /* Now sort the pages */
        g_ptr_array_sort (comics_document->page_names, sort_page_names);

	return TRUE;
}

static gboolean
comics_document_save (EvDocument *document,
		      const char *uri,
		      GError    **error)
{
	ComicsDocument *comics_document = COMICS_DOCUMENT (document);

	return ev_xfer_uri_simple (comics_document->archive_uri, uri, error);
}

static int
comics_document_get_n_pages (EvDocument *document)
{
	ComicsDocument *comics_document = COMICS_DOCUMENT (document);

        if (comics_document->page_names == NULL)
                return 0;

	return comics_document->page_names->len;
}

typedef struct {
	gboolean got_info;
	int height;
	int width;
} PixbufInfo;

static void
get_page_size_prepared_cb (GdkPixbufLoader *loader,
			   int              width,
			   int              height,
			   PixbufInfo      *info)
{
	info->got_info = TRUE;
	info->height = height;
	info->width = width;
}

static void
comics_document_get_page_size (EvDocument *document,
			       EvPage     *page,
			       double     *width,
			       double     *height)
{
	GdkPixbufLoader *loader;
	ComicsDocument *comics_document = COMICS_DOCUMENT (document);
	const char *page_path;
	PixbufInfo info;
	GError *error = NULL;

	if (!ev_archive_open_filename (comics_document->archive, comics_document->archive_path, &error)) {
		g_warning ("Fatal error opening archive: %s", error->message);
		g_error_free (error);
		goto out;
	}

	loader = gdk_pixbuf_loader_new ();
	info.got_info = FALSE;
	g_signal_connect (loader, "size-prepared",
			  G_CALLBACK (get_page_size_prepared_cb),
			  &info);

	page_path = g_ptr_array_index (comics_document->page_names, page->index);

	while (1) {
		const char *name;
		GError *error = NULL;

		if (!ev_archive_read_next_header (comics_document->archive, &error)) {
			if (error != NULL) {
				g_warning ("Fatal error handling archive: %s", error->message);
				g_error_free (error);
			}
			break;
		}

		name = ev_archive_get_entry_pathname (comics_document->archive);
		if (g_strcmp0 (name, page_path) == 0) {
			char buf[BLOCK_SIZE];
			gssize read;
			gint64 left;

			left = ev_archive_get_entry_size (comics_document->archive);
			read = ev_archive_read_data (comics_document->archive, buf,
						     MIN(BLOCK_SIZE, left), &error);
			while (read > 0 && !info.got_info) {
				if (!gdk_pixbuf_loader_write (loader, (guchar *) buf, read, &error)) {
					read = -1;
					break;
				}
				left -= read;
				read = ev_archive_read_data (comics_document->archive, buf,
							     MIN(BLOCK_SIZE, left), &error);
			}
			if (read < 0) {
				g_warning ("Fatal error reading '%s' in archive: %s", name, error->message);
				g_error_free (error);
			}
			break;
		}
	}

	gdk_pixbuf_loader_close (loader, NULL);
	g_object_unref (loader);

	if (info.got_info) {
		if (width)
			*width = info.width;
		if (height)
			*height = info.height;
	}

out:
	ev_archive_reset (comics_document->archive);
}

static void
render_pixbuf_size_prepared_cb (GdkPixbufLoader *loader,
				gint             width,
				gint             height,
				EvRenderContext *rc)
{
	int scaled_width, scaled_height;

	ev_render_context_compute_scaled_size (rc, width, height, &scaled_width, &scaled_height);
	gdk_pixbuf_loader_set_size (loader, scaled_width, scaled_height);
}

static GdkPixbuf *
comics_document_render_pixbuf (EvDocument      *document,
			       EvRenderContext *rc)
{
	GdkPixbufLoader *loader;
	GdkPixbuf *tmp_pixbuf;
	GdkPixbuf *rotated_pixbuf = NULL;
	ComicsDocument *comics_document = COMICS_DOCUMENT (document);
	const char *page_path;
	GError *error = NULL;

	if (!ev_archive_open_filename (comics_document->archive, comics_document->archive_path, &error)) {
		g_warning ("Fatal error opening archive: %s", error->message);
		g_error_free (error);
		goto out;
	}

	loader = gdk_pixbuf_loader_new ();
	g_signal_connect (loader, "size-prepared",
			  G_CALLBACK (render_pixbuf_size_prepared_cb),
			  rc);

	page_path = g_ptr_array_index (comics_document->page_names, rc->page->index);

	while (1) {
		const char *name;

		if (!ev_archive_read_next_header (comics_document->archive, &error)) {
			if (error != NULL) {
				g_warning ("Fatal error handling archive: %s", error->message);
				g_error_free (error);
			}
			break;
		}

		name = ev_archive_get_entry_pathname (comics_document->archive);
		if (g_strcmp0 (name, page_path) == 0) {
			size_t size = ev_archive_get_entry_size (comics_document->archive);
			char *buf;
			ssize_t read;

			buf = g_malloc (size);
			read = ev_archive_read_data (comics_document->archive, buf, size, &error);
			if (read <= 0) {
				if (read < 0) {
					g_warning ("Fatal error reading '%s' in archive: %s", name, error->message);
					g_error_free (error);
				} else {
					g_warning ("Read an empty file from the archive");
				}
			} else {
				gdk_pixbuf_loader_write (loader, (guchar *) buf, size, NULL);
			}
			g_free (buf);
			gdk_pixbuf_loader_close (loader, NULL);
			break;
		}
	}

	tmp_pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
	if (tmp_pixbuf) {
		if ((rc->rotation % 360) == 0)
			rotated_pixbuf = g_object_ref (tmp_pixbuf);
		else
			rotated_pixbuf = gdk_pixbuf_rotate_simple (tmp_pixbuf,
								   360 - rc->rotation);
	}
	g_object_unref (loader);

out:
	ev_archive_reset (comics_document->archive);
	return rotated_pixbuf;
}

static cairo_surface_t *
comics_document_render (EvDocument      *document,
			EvRenderContext *rc)
{
	GdkPixbuf       *pixbuf;
	cairo_surface_t *surface;

	pixbuf = comics_document_render_pixbuf (document, rc);
	surface = ev_document_misc_surface_from_pixbuf (pixbuf);
	g_object_unref (pixbuf);

	return surface;
}

static void
comics_document_finalize (GObject *object)
{
	ComicsDocument *comics_document = COMICS_DOCUMENT (object);

	if (comics_document->page_names) {
                g_ptr_array_foreach (comics_document->page_names, (GFunc) g_free, NULL);
                g_ptr_array_free (comics_document->page_names, TRUE);
	}

	g_clear_object (&comics_document->archive);
	g_free (comics_document->archive_path);
	g_free (comics_document->archive_uri);

	G_OBJECT_CLASS (comics_document_parent_class)->finalize (object);
}

static void
comics_document_class_init (ComicsDocumentClass *klass)
{
	GObjectClass    *gobject_class = G_OBJECT_CLASS (klass);
	EvDocumentClass *ev_document_class = EV_DOCUMENT_CLASS (klass);

	gobject_class->finalize = comics_document_finalize;

	ev_document_class->load = comics_document_load;
	ev_document_class->save = comics_document_save;
	ev_document_class->get_n_pages = comics_document_get_n_pages;
	ev_document_class->get_page_size = comics_document_get_page_size;
	ev_document_class->render = comics_document_render;
}

static void
comics_document_init (ComicsDocument *comics_document)
{
	comics_document->archive = ev_archive_new ();
}

/* Returns a list of file extensions supported by gdk-pixbuf */
static GSList*
get_supported_image_extensions(void)
{
	GSList *extensions = NULL;
	GSList *formats = gdk_pixbuf_get_formats ();
	GSList *l;

	for (l = formats; l != NULL; l = l->next) {
		int i;
		gchar **ext = gdk_pixbuf_format_get_extensions (l->data);

		for (i = 0; ext[i] != NULL; i++) {
			extensions = g_slist_append (extensions,
						     g_strdup (ext[i]));
		}

		g_strfreev (ext);
	}

	g_slist_free (formats);
	return extensions;
}
