/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg.c: SAX-based renderer for SVG files into a GdkPixbuf.

   Copyright (C) 2000 Eazel, Inc.
   Copyright (C) 2002-2005 Dom Lachowicz <cinamod@hotmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Raph Levien <raph@artofcode.com>
*/

#include "config.h"

#include "rsvg.h"
#include "rsvg-private.h"
#include "rsvg-css.h"
#include "rsvg-styles.h"
#include "rsvg-shapes.h"
#include "rsvg-image.h"
#include "rsvg-text.h"
#include "rsvg-filter.h"
#include "rsvg-mask.h"
#include "rsvg-marker.h"

#include <math.h>
#include <string.h>
#include <stdarg.h>

#if defined(WITH_LIBART_BACKEND)

#include "rsvg-bpath-util.h"
#include "rsvg-path.h"
#include "rsvg-paint-server.h"

#include "rsvg-art-render.h"
#include "rsvg-art-draw.h"

static RsvgDrawingCtx * 
rsvg_new_drawing_ctx(RsvgHandle * handle)
{
	RsvgDimensionData data;
	RsvgDrawingCtx * draw;
	RsvgState * state;
	double affine[6];

	rsvg_handle_get_dimensions(handle, &data);
	if(data.width == 0 || data.height == 0)
		return NULL;

	draw = g_new(RsvgDrawingCtx, 1);

	draw->render = (RsvgRender *) rsvg_art_render_new (data.width, data.height);

	if(!draw->render)
		return NULL;	

	draw->state = NULL;

	/* should this be G_ALLOC_ONLY? */
	draw->state_allocator = g_mem_chunk_create (RsvgState, 256, G_ALLOC_AND_FREE);

	draw->defs = handle->defs;
	draw->base_uri = g_strdup(handle->base_uri);
	draw->dpi_x = handle->dpi_x;
	draw->dpi_y = handle->dpi_y;
	draw->pango_context = NULL;

	rsvg_state_push(draw);

	state = rsvg_state_current(draw);
	affine[0] = data.width / data.em;
	affine[1] = 0;
	affine[2] = 0;
	affine[3] = data.height / data.ex;
	affine[4] = 0;
	affine[5] = 0;

	_rsvg_affine_multiply(state->affine, affine, 
						  state->affine);
	
	return draw;
}

static GdkPixbuf * _rsvg_handle_get_pixbuf (RsvgHandle *handle)
{
	GdkPixbuf * output = NULL;
	RsvgDrawingCtx * draw;

	draw = rsvg_new_drawing_ctx(handle);
	if (!draw)
		return NULL;
	rsvg_state_push(draw);
	rsvg_node_draw((RsvgNode *)handle->treebase, draw, 0);
	rsvg_state_pop(draw);
	output = ((RsvgArtRender *)draw->render)->pixbuf;
	rsvg_drawing_ctx_free(draw);
	
	return output;
}

#elif defined(WITH_CAIRO_BACKEND)

#include "rsvg-cairo.h"

static void
rsvg_pixmap_destroy (gchar *pixels, gpointer data)
{
  g_free (pixels);
}

static GdkPixbuf * _rsvg_handle_get_pixbuf (RsvgHandle *handle)
{
	RsvgDimensionData dimensions;
	GdkPixbuf *output = NULL;
	guint8 *pixels;
	cairo_surface_t *surface;
	cairo_t *cr;
	int row, rowstride;

	rsvg_handle_get_dimensions (handle, &dimensions);
	rowstride = dimensions.width * 4;

	pixels = g_new0(guint8, dimensions.width * dimensions.height * 4);

	surface = cairo_image_surface_create_for_data (pixels,
												   CAIRO_FORMAT_ARGB32,
												   dimensions.width, dimensions.height,
												   rowstride);

	cr = cairo_create (surface);

	rsvg_cairo_render (cr, handle);

	/* un-premultiply data */
	for(row = 0; row < dimensions.height; row++) {
		guint8 *row_data = (pixels + (row * rowstride));
		int i;

		for(i = 0; i < rowstride; i += 4) {
			guint8 *b = &row_data[i];
			guint32 pixel;
			guint8 alpha;

			memcpy(&pixel, b, sizeof(guint32));
			alpha = (pixel & 0xff000000) >> 24;
			if(alpha == 0) {
				b[0] = b[1] = b[2] = b[3] = 0;
			} else {
				b[0] = (((pixel & 0xff0000) >> 16) * 255 + alpha / 2) / alpha;
				b[1] = (((pixel & 0x00ff00) >>  8) * 255 + alpha / 2) / alpha;
				b[2] = (((pixel & 0x0000ff) >>  0) * 255 + alpha / 2) / alpha;
				b[3] = alpha;
			}
		}
	}

	output = gdk_pixbuf_new_from_data (pixels,
									   GDK_COLORSPACE_RGB,
									   TRUE,
									   8,
									   dimensions.width,
									   dimensions.height,
									   rowstride,
									   (GdkPixbufDestroyNotify)rsvg_pixmap_destroy,
									   NULL);

	cairo_destroy (cr);
	cairo_surface_destroy (surface);

	return output;
}

#else

#ifdef __GNUC__
#warning "No backend defined. Needs either Cairo or Libart in order to work."
#endif

static GdkPixbuf * _rsvg_handle_get_pixbuf (RsvgHandle *handle)
{
	g_warning ("No backend defined. Needs either Cairo or Libart in order to work.");
	return NULL;
}


#endif

/**
 * rsvg_handle_get_pixbuf:
 * @handle: An #RsvgHandle
 *
 * Returns the pixbuf loaded by #handle.  The pixbuf returned will be reffed, so
 * the caller of this function must assume that ref.  If insufficient data has
 * been read to create the pixbuf, or an error occurred in loading, then %NULL
 * will be returned.  Note that the pixbuf may not be complete until
 * @rsvg_handle_close has been called.
 *
 * Returns: the pixbuf loaded by #handle, or %NULL.
 **/
GdkPixbuf *
rsvg_handle_get_pixbuf (RsvgHandle *handle)
{
	g_return_val_if_fail (handle != NULL, NULL);

	if (!handle->finished)
		return NULL;

	return _rsvg_handle_get_pixbuf (handle);
}
