/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* 
   rsvg-filter.c: Provides filters
 
   Copyright (C) 2004 Caleb Moore
  
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
  
   Author: Caleb Moore <calebmm@tpg.com.au>
*/

#include "rsvg-filter.h"
#include "rsvg-private.h"
#include "rsvg-css.h"
#include <libart_lgpl/art_rgba.h>

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif /*  M_PI  */

#define PERFECTBLUR 4

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterContext RsvgFilterContext;

struct _RsvgFilterContext
{
	gint width, height;
	RsvgFilter *filter;
	GHashTable *results;
	GdkPixbuf *source;
	GdkPixbuf *bg;
	GdkPixbuf *lastresult;
	double affine[6];
	double paffine[6];
};

typedef struct _RsvgFilterPrimitive RsvgFilterPrimitive;

struct _RsvgFilterPrimitive
{
	double x, y, width, height;
	GString *in;
	GString *result;
	gboolean sizedefaults;
	
	void (*free) (RsvgFilterPrimitive * self);
	void (*render) (RsvgFilterPrimitive * self, RsvgFilterContext * ctx);
};

typedef struct
{
	gint x1, y1, x2, y2;
} FPBox;

/*************************************************************/
/*************************************************************/

static void
rsvg_filter_primitive_render (RsvgFilterPrimitive * self,
			      RsvgFilterContext * ctx)
{
	self->render (self, ctx);
}

static void
rsvg_filter_primitive_free (RsvgFilterPrimitive * self)
{
	self->free (self);
}

static FPBox
rsvg_filter_primitive_get_bounds (RsvgFilterPrimitive * self,
				  RsvgFilterContext * ctx)
{
	FPBox output;
	
	if (self->sizedefaults)
		{
			output.x1 = ctx->affine[0] * ctx->filter->x + ctx->affine[4];
			output.y1 = ctx->affine[3] * ctx->filter->y + ctx->affine[5];
			output.x2 =
				ctx->affine[0] * (ctx->filter->x + ctx->filter->width) +
				ctx->affine[4];
			output.y2 =
				ctx->affine[3] * (ctx->filter->y + ctx->filter->height) +
				ctx->affine[5];
			
			if (output.x1 < 0)
				output.x1 = 0;
			if (output.x2 >= ctx->width)
				output.x2 = ctx->width - 1;
			if (output.y1 < 0)
				output.y1 = 0;
			if (output.y2 >= ctx->height)
				output.y2 = ctx->height - 1;
			
			return output;
		}
	
	output.x1 = ctx->paffine[0] * self->x + ctx->paffine[4];
	output.y1 = ctx->paffine[3] * self->y + ctx->paffine[5];
	output.x2 = ctx->paffine[0] * (self->x + self->width) + ctx->paffine[4];
	output.y2 = ctx->paffine[3] * (self->y + self->height) + ctx->paffine[5];
	
	if (output.x1 < ctx->affine[0] * ctx->filter->x + ctx->affine[4])
		output.x1 = ctx->affine[0] * ctx->filter->x + ctx->affine[4];
	if (output.x2 >
		ctx->affine[0] * (ctx->filter->x + ctx->filter->width) + ctx->affine[4])
		output.x2 =
			ctx->affine[0] * (ctx->filter->x + ctx->filter->width) + ctx->affine[4];
	if (output.y1 < ctx->affine[3] * ctx->filter->y + ctx->affine[5])
		output.y1 = ctx->affine[3] * ctx->filter->y + ctx->affine[5];
	if (output.y2 > ctx->affine[3] * (ctx->filter->y + ctx->filter->height) +
		ctx->affine[5])
		output.y2 = ctx->affine[3] * (ctx->filter->y + ctx->filter->height) +
			ctx->affine[5];
	
	if (output.x1 < 0)
		output.x1 = 0;
	if (output.x2 >= ctx->width)
		output.x2 = ctx->width - 1;
	if (output.y1 < 0)
		output.y1 = 0;
	if (output.y2 >= ctx->height)
		output.y2 = ctx->height - 1;
	
	return output;
}

static GdkPixbuf *
gdk_pixbuf_new_cleared (GdkColorspace colorspace, gboolean has_alpha, int bits_per_sample,
						int width, int height)
{
	GdkPixbuf *pb;
	guchar *data;

	pb = gdk_pixbuf_new (colorspace, has_alpha, bits_per_sample, width, height);
	data = gdk_pixbuf_get_pixels (pb);
	memset(data, 0, width * height * 4);

	return pb;
}

static void
alpha_blt (GdkPixbuf * src, gint srcx, gint srcy, gint srcwidth,
		   gint srcheight, GdkPixbuf * dst, gint dstx, gint dsty)
{
	gint rightx;
	gint bottomy;
	gint dstwidth;
	gint dstheight;
	
	gint srcoffsetx;
	gint srcoffsety;
	gint dstoffsetx;
	gint dstoffsety;
	
	gint x, y, srcrowstride, dstrowstride, sx, sy, dx, dy;
	guchar *src_pixels, *dst_pixels;
	
	dstheight = srcheight;
	dstwidth = srcwidth;
	
	rightx = srcx + srcwidth;
	bottomy = srcy + srcheight;
	
	if (rightx > gdk_pixbuf_get_width (src))
		rightx = gdk_pixbuf_get_width (src);
	if (bottomy > gdk_pixbuf_get_height (src))
		bottomy = gdk_pixbuf_get_height (src);
	srcwidth = rightx - srcx;
	srcheight = bottomy - srcy;
	
	rightx = dstx + dstwidth;
	bottomy = dsty + dstheight;
	if (rightx > gdk_pixbuf_get_width (dst))
		rightx = gdk_pixbuf_get_width (dst);
	if (bottomy > gdk_pixbuf_get_height (dst))
		bottomy = gdk_pixbuf_get_height (dst);
	dstwidth = rightx - dstx;
	dstheight = bottomy - dsty;
	
	if (dstwidth < srcwidth)
		srcwidth = dstwidth;
	if (dstheight < srcheight)
		srcheight = dstheight;
	
	if (srcx < 0)
		srcoffsetx = 0 - srcx;
	else
		srcoffsetx = 0;

	if (srcy < 0)
		srcoffsety = 0 - srcy;
	else
		srcoffsety = 0;

	if (dstx < 0)
		dstoffsetx = 0 - dstx;
	else
		dstoffsetx = 0;

	if (dsty < 0)
		dstoffsety = 0 - dsty;
	else
		dstoffsety = 0;
	
	if (dstoffsetx > srcoffsetx)
		srcoffsetx = dstoffsetx;
	if (dstoffsety > srcoffsety)
		srcoffsety = dstoffsety;
	
	srcrowstride = gdk_pixbuf_get_rowstride (src);
	dstrowstride = gdk_pixbuf_get_rowstride (dst);
	
	src_pixels = gdk_pixbuf_get_pixels (src);
	dst_pixels = gdk_pixbuf_get_pixels (dst);
	
	for (y = srcoffsety; y < srcheight; y++)
		for (x = srcoffsetx; x < srcwidth; x++)
			{
				guchar r, g, b, a;

				sx = x + srcx;
				sy = y + srcy;
				dx = x + dstx;
				dy = y + dsty;
				a = src_pixels[4 * sx + sy * srcrowstride + 3];
				if (a)
					{
						r = src_pixels[4 * sx + sy * srcrowstride];
						g = src_pixels[4 * sx + 1 + sy * srcrowstride];
						b = src_pixels[4 * sx + 2 + sy * srcrowstride];
						art_rgba_run_alpha (dst_pixels + 4 * dx +
											dy * dstrowstride, r, g, b, a, 1);
					}
			}
}

static void
rsvg_filter_fix_coordinate_system (RsvgFilterContext * ctx, RsvgState * state)
{
	int i, j;
	int x, y, height, width;
	guchar *pixels;
	int stride;
	int currentindex;
	
	i = j = 0;
	
	x = y = width = height = 0;
	
	/* First for object bounding box coordinates we need to know how much of the 
	   source has been drawn on */
	pixels = gdk_pixbuf_get_pixels (ctx->source);
	stride = gdk_pixbuf_get_rowstride (ctx->source);
	x = y = height = width = -1;
	
	/* move in from the top to find the y value */
	for (i = 0; i < gdk_pixbuf_get_height (ctx->source); i++)
		{
			for (j = 0; j < gdk_pixbuf_get_width (ctx->source); j++)
				{
					currentindex = i * stride + j * 4;
					if (pixels[currentindex + 0] != 0 || pixels[currentindex + 1] != 0
						|| pixels[currentindex + 2] != 0
						|| pixels[currentindex + 3] != 0)
						{
							y = i;
							break;
						}
				}
			if (y != -1)
				break;
		}
		
	/* move in from the bottom to find the height */
	for (i = gdk_pixbuf_get_height (ctx->source) - 1; i >= 0; i--)
		{
			for (j = 0; j < gdk_pixbuf_get_width (ctx->source); j++)
				{
					currentindex = i * stride + j * 4;
					if (pixels[currentindex + 0] != 0 || pixels[currentindex + 1] != 0
						|| pixels[currentindex + 2] != 0
						|| pixels[currentindex + 3] != 0)
						{
							height = i - y;
							break;
						}
					
				}
			if (height != -1)
				break;
		}
	
	/* move in from the left to find the x value */
	for (j = 0; j < gdk_pixbuf_get_width (ctx->source); j++)
		{
			for (i = y; i < (height + y); i++)
				{
					currentindex = i * stride + j * 4;
					if (pixels[currentindex + 0] != 0 || pixels[currentindex + 1] != 0
						|| pixels[currentindex + 2] != 0
						|| pixels[currentindex + 3] != 0)
						{
							x = j;
							break;
						}
				}
			if (x != -1)
				break;
		}
	
	/* move in from the right side to find the width */
	for (j = gdk_pixbuf_get_width (ctx->source) - 1; j >= 0; j--)
		{
			for (i = y; i < (height + y); i++)
				{
					currentindex = i * stride + j * 4;
					if (pixels[currentindex + 0] != 0 || pixels[currentindex + 1] != 0
						|| pixels[currentindex + 2] != 0
						|| pixels[currentindex + 3] != 0)
						{
							width = j - x;
							break;
						}
				}
			if (width != -1)
				break;
		}
	
	ctx->width = gdk_pixbuf_get_width (ctx->source);
	ctx->height = gdk_pixbuf_get_height (ctx->source);
	
	if (ctx->filter->filterunits == userSpaceOnUse)
		{
			for (i = 0; i < 6; i++)
				ctx->affine[i] = state->affine[i];
		}
	else
		{
			ctx->affine[0] = width;
			ctx->affine[1] = 0.;
			ctx->affine[2] = 0.;
			ctx->affine[3] = height;
			ctx->affine[4] = x;
			ctx->affine[5] = y;
		}
	
	if (ctx->filter->primitiveunits == userSpaceOnUse)
		{
			for (i = 0; i < 6; i++)
				ctx->paffine[i] = state->affine[i];
		}
	else
		{
			ctx->paffine[0] = width;
			ctx->paffine[1] = 0.;
			ctx->paffine[2] = 0.;
			ctx->paffine[3] = height;
			ctx->paffine[4] = x;
			ctx->paffine[5] = y;
		}
}

static void
rsvg_filter_free_pair (gpointer key, gpointer value, gpointer user_data)
{
	g_object_unref (G_OBJECT (value));
	g_free ((gchar *) key);
}

/**
 * rsvg_filter_render: Copy the source to the bg using a filter.
 * @self: a pointer to the filter to use
 * @source: a pointer to the source pixbuf
 * @bg: the background pixbuf
 * @context: the context
 *
 * This function will create a context for itself, set up the coordinate systems
 * execute all its little primatives and then clean up its own mess
 **/
void
rsvg_filter_render (RsvgFilter * self, GdkPixbuf * source, GdkPixbuf * bg,
					RsvgHandle * context)
{
	RsvgFilterContext *ctx;
	RsvgFilterPrimitive *current;
	guint i;
	
	ctx = g_new (RsvgFilterContext, 1);
	ctx->filter = self;
	ctx->source = source;
	ctx->bg = bg;
	ctx->results = g_hash_table_new (g_str_hash, g_str_equal);
	
	g_object_ref (G_OBJECT (source));
	ctx->lastresult = source;
	
	rsvg_filter_fix_coordinate_system (ctx, rsvg_state_current (context));
	
	for (i = 0; i < self->primitives->len; i++)
		{
			current = g_ptr_array_index (self->primitives, i);
			rsvg_filter_primitive_render (current, ctx);
		}
	g_hash_table_foreach (ctx->results, rsvg_filter_free_pair, NULL);
	g_hash_table_destroy (ctx->results);
	
	alpha_blt (ctx->lastresult, 0, 0, gdk_pixbuf_get_width (source),
			   gdk_pixbuf_get_height (source), bg, 0, 0);
	g_object_unref (G_OBJECT (ctx->lastresult));
}

/**
 * rsvg_filter_store_result: Files a result into a context.
 * @name: The name of the result
 * @result: The pointer to the result
 * @ctx: the context that this was called in
 *
 * Puts the new result into the hash for easy finding later, also
 * Stores it as the last result
 **/
static void
rsvg_filter_store_result (GString * name, GdkPixbuf * result,
						  RsvgFilterContext * ctx)
{
	g_object_unref (G_OBJECT (ctx->lastresult));
	
	if (strcmp (name->str, ""))
		{
			g_object_ref (G_OBJECT (result));	/* increments the references for the table */
			g_hash_table_insert (ctx->results, g_strdup (name->str), result);
		}
	
	g_object_ref (G_OBJECT (result));	/* increments the references for the last result */
	ctx->lastresult = result;
}

static GdkPixbuf *
pixbuf_get_alpha (GdkPixbuf * pb)
{
	guchar *data;
	guchar *pbdata;
	GdkPixbuf *output;
	
	gsize i, pbsize;

	pbsize = gdk_pixbuf_get_width (pb) * gdk_pixbuf_get_height (pb);

	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8,
									 gdk_pixbuf_get_width (pb),
									 gdk_pixbuf_get_height (pb));
	
	data = gdk_pixbuf_get_pixels (output);
	pbdata = gdk_pixbuf_get_pixels (pb);
	
	for (i = 0; i < pbsize; i++)
		data[i * 4 + 3] = pbdata[i * 4 + 3];
	
	return output;
}

/**
 * rsvg_filter_get_in: Gets a pixbuf for a primative.
 * @name: The name of the pixbuf
 * @ctx: the context that this was called in
 *
 * Returns a pointer to the result that the name refers to, a special
 * Pixbuf if the name is a special keyword or NULL if nothing was found
 **/
static GdkPixbuf *
rsvg_filter_get_in (GString * name, RsvgFilterContext * ctx)
{
	GdkPixbuf *output;

	if (!strcmp (name->str, "SourceGraphic"))
		{
			g_object_ref (G_OBJECT (ctx->source));
			return ctx->source;
		}
	else if (!strcmp (name->str, "BackgroundImage"))
		{
			g_object_ref (G_OBJECT (ctx->bg));
			return ctx->bg;
		}
	else if (!strcmp (name->str, "") || !strcmp (name->str, "none"))
		{
			g_object_ref (G_OBJECT (ctx->lastresult));
			return ctx->lastresult;
		}
	else if (!strcmp (name->str, "SourceAlpha"))
		return pixbuf_get_alpha (ctx->source);
	else if (!strcmp (name->str, "BackgroundAlpha"))
		return pixbuf_get_alpha (ctx->bg);
	
	output = g_hash_table_lookup (ctx->results, name->str);
	g_object_ref (G_OBJECT (output));
	
	if (output != NULL)
			return output;

	g_object_ref (G_OBJECT (ctx->lastresult));
	return ctx->lastresult;
}

/**
 * rsvg_filter_parse: Looks up an allready created filter.
 * @defs: a pointer to the hash of definitions
 * @str: a string with the name of the filter to be looked up
 *
 * Returns a pointer to the filter that the name refers to, or NULL
 * if none was found
 **/
RsvgFilter *
rsvg_filter_parse (const RsvgDefs * defs, const char *str)
{
	if (!strncmp (str, "url(", 4))
		{
			const char *p = str + 4;
			int ix;
			char *name;
			RsvgDefVal *val;
			
			while (g_ascii_isspace (*p))
				p++;

			if (*p == '#')
				{
					p++;
					for (ix = 0; p[ix]; ix++)
						if (p[ix] == ')')
							break;

					if (p[ix] == ')')
						{
							name = g_strndup (p, ix);
							val = rsvg_defs_lookup (defs, name);
							g_free (name);
							
							if (val && val->type == RSVG_DEF_FILTER)
								return (RsvgFilter *) val;
						}
				}
		}
	
	return NULL;
}

/**
 * rsvg_new_filter: Creates a black filter
 *
 * Creates a blank filter and assigns default values to everything
 **/
static RsvgFilter *
rsvg_new_filter (void)
{
	RsvgFilter *filter;

	filter = g_new (RsvgFilter, 1);
	filter->filterunits = objectBoundingBox;
	filter->primitiveunits = userSpaceOnUse;
	filter->x = -0.1;
	filter->y = -0.1;
	filter->width = 1.2;
	filter->height = 1.2;
	filter->primitives = g_ptr_array_new ();

	return filter;
}

/**
 * rsvg_filter_free: Free a filter.
 * @dself: The defval to be freed 
 *
 * Frees a filter and all primatives associated with this filter, this is 
 * to be set as its free function to be used with rsvg defs
 **/
static void
rsvg_filter_free (RsvgDefVal * dself)
{
	RsvgFilterPrimitive *current;
	RsvgFilter *self;
	guint i;
	
	self = (RsvgFilter *) dself;
	
	for (i = 0; i < self->primitives->len; i++)
		{
			current = g_ptr_array_index (self->primitives, i);
			rsvg_filter_primitive_free (current);
		}
}

/**
 * rsvg_start_filter: Create a filter from xml arguments.
 * @ctx: the current rsvg handle
 * @atts: the xml attributes that set the filter's properties
 *
 * Creates a new filter and sets it as a def
 * Also sets the context's current filter pointer to point to the
 * newly created filter so that all subsiquent primatives are
 * added to this filter until the filter is ended
 **/
void
rsvg_start_filter (RsvgHandle * ctx, const xmlChar ** atts)
{
	int i;
	const char *klazz = NULL;
	char *id = NULL;
	RsvgFilter *filter;
	double font_size;
	
	font_size = rsvg_state_current_font_size (ctx);
	filter = rsvg_new_filter ();
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "filterUnits"))
						{
							if (!strcmp ((char *) atts[i], "userSpaceOnUse"))
								filter->filterunits = userSpaceOnUse;
							else
								filter->filterunits = objectBoundingBox;
						}
					else if (!strcmp ((char *) atts[i], "primitiveUnits"))
						{
							if (!strcmp ((char *) atts[i], "objectBoundingBox"))
								filter->primitiveunits = objectBoundingBox;
							else
								filter->primitiveunits = userSpaceOnUse;
						}
					else if (!strcmp ((char *) atts[i], "x"))
						filter->x =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);
					else if (!strcmp ((char *) atts[i], "y"))
						filter->y =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);
					else if (!strcmp ((char *) atts[i], "width"))
						filter->width =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);
					else if (!strcmp ((char *) atts[i], "height"))
						filter->height =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);					
					else if (!strcmp ((char *) atts[i], "filterRes"))
						;
					else if (!strcmp ((char *) atts[i], "xlink::href"))
						;
					else if (!strcmp ((char *) atts[i], "class"))
						klazz = (char *) atts[i + 1];
					else if (!strcmp ((char *) atts[i], "id"))
						id = (char *) atts[i + 1];
				}
		}
	ctx->currentfilter = filter;
	
	/* set up the defval stuff */
	filter->super.type = RSVG_DEF_FILTER;
	filter->super.free = &rsvg_filter_free;
	rsvg_defs_set (ctx->defs, id, &filter->super);
}

/**
 * rsvg_end_filter: Create a filter from xml arguments.
 * @ctx: the current rsvg handle
 *
 * Ends the current filter block by setting the currentfilter ot null
 **/
void
rsvg_end_filter (RsvgHandle * ctx)
{
	ctx->currentfilter = NULL;
}

/*************************************************************/
/*************************************************************/

typedef enum
{
  normal, multiply, screen, darken, lighten
}
RsvgFilterPrimitiveBlendMode;

typedef struct _RsvgFilterPrimitiveBlend RsvgFilterPrimitiveBlend;
struct _RsvgFilterPrimitiveBlend
{
	RsvgFilterPrimitive super;
	RsvgFilterPrimitiveBlendMode mode;
	GString *in2;
};

static void
rsvg_filter_primitive_blend_render (RsvgFilterPrimitive * self,
									RsvgFilterContext * ctx)
{
	guchar i;
	gint x, y;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *in2_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveBlend *bself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	GdkPixbuf *in2;
	
	bself = (RsvgFilterPrimitiveBlend *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	in2 = rsvg_filter_get_in (bself->in2, ctx);
	in2_pixels = gdk_pixbuf_get_pixels (in2);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);
	output_pixels = gdk_pixbuf_get_pixels (output);
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				double qr, cr, qa, qb, ca, cb;

				qa = (double) in_pixels[4 * x + y * rowstride + 3] / 255.0;
				qb = (double) in2_pixels[4 * x + y * rowstride + 3] / 255.0;
				qr = 1 - (1 - qa) * (1 - qb);
				cr = 0;
				for (i = 0; i < 3; i++)
					{
						ca = (double) in_pixels[4 * x + y * rowstride + i] * qa / 255.0;
						cb = (double) in2_pixels[4 * x + y * rowstride + i] * qb / 255.0;
						switch (bself->mode)
							{
							case normal:
								cr = (1 - qa) * cb + ca;
								break;
							case multiply:
								cr = (1 - qa) * cb + (1 - qb) * ca + ca * cb;
								break;
							case screen:
								cr = cb + ca - ca * cb;
								break;
							case darken:
								cr = MIN ((1 - qa) * cb + ca, (1 - qb) * ca + cb);
								break;
							case lighten:
								cr = MAX ((1 - qa) * cb + ca, (1 - qb) * ca + cb);
								break;
							}
						cr *= 255.0 / qr;
						if (cr > 255)
							cr = 255;
						if (cr < 0)
							cr = 0;
						output_pixels[4 * x + y * rowstride + i] = (guchar) cr;
						
					}
				output_pixels[4 * x + y * rowstride + 3] = qr * 255.0;
			}

	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (in2));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_blend_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveBlend *bself;
	
	bself = (RsvgFilterPrimitiveBlend *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_string_free (bself->in2, TRUE);
	g_free (bself);
}

void
rsvg_start_filter_primitive_blend (RsvgHandle * ctx, const xmlChar ** atts)
{
	int i;
	double font_size;
	RsvgFilterPrimitiveBlend *filter;
	
	font_size = rsvg_state_current_font_size (ctx);

	filter = g_new (RsvgFilterPrimitiveBlend, 1);
	filter->mode = normal;
	filter->super.in = g_string_new ("none");
	filter->in2 = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "mode")) 
						{
							if (!strcmp ((char *) atts[i + 1], "multiply"))
								filter->mode = multiply;
							else if (!strcmp ((char *) atts[i + 1], "screen"))
								filter->mode = screen;
							else if (!strcmp ((char *) atts[i + 1], "darken"))
								filter->mode = darken;
							else if (!strcmp ((char *) atts[i + 1], "lighten"))
								filter->mode = lighten;
							else
								filter->mode = normal;
						}
					else if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "in2"))
						g_string_assign (filter->in2, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
				}
		}
	
	filter->super.render = &rsvg_filter_primitive_blend_render;
	filter->super.free = &rsvg_filter_primitive_blend_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveConvolveMatrix RsvgFilterPrimitiveConvolveMatrix;

struct _RsvgFilterPrimitiveConvolveMatrix
{
	RsvgFilterPrimitive super;
	double *KernelMatrix;
	double divisor;
	gint orderx, ordery;
	double dx, dy;
	double bias;
	gint targetx, targety;
	gboolean preservealpha;
	gint edgemode;
};

static void
rsvg_filter_primitive_convolve_matrix_render (RsvgFilterPrimitive * self,
											  RsvgFilterContext * ctx)
{
	guchar ch;
	gint x, y;
	gint i, j;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveConvolveMatrix *cself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	
	gint sx, sy, kx, ky;
	guchar sval;
	double kval, sum, dx, dy, targetx, targety;
	
	gint tempresult;
	
	cself = (RsvgFilterPrimitiveConvolveMatrix *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	targetx = cself->targetx * ctx->paffine[0];
	targety = cself->targety * ctx->paffine[3];

	if (cself->dx != 0 || cself->dy != 0)
		{
			dx = cself->dx * ctx->paffine[0];
			dy = cself->dy * ctx->paffine[3];
		}
	else
		dx = dy = 1;

	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);
	output_pixels = gdk_pixbuf_get_pixels (output);
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				for (ch = 0; ch < 3 + !cself->preservealpha; ch++)
					{
						sum = 0;
						for (i = 0; i < cself->ordery; i++)
							for (j = 0; j < cself->orderx; j++)
								{
									sx = x - targetx + j * dx;
									sy = y - targety + i * dy;
									if (cself->edgemode == 0)
										{
											if (sx < boundarys.x1)
												sx = boundarys.x1;
											if (sx >= boundarys.x2)
												sx = boundarys.x2 - 1;
											if (sy < boundarys.y1)
												sy = boundarys.y1;
											if (sy >= boundarys.y2)
												sy = boundarys.y2 - 1;
										}
									else if (cself->edgemode == 1)
										{
											if (sx < boundarys.x1 || (sx >= boundarys.x2))
												sx = boundarys.x1 + (sx - boundarys.x1) %
													(boundarys.x2 - boundarys.x1);
											if (sy < boundarys.y1 || (sy >= boundarys.y2))
												sy = boundarys.y1 + (sy - boundarys.y1) %
													(boundarys.y2 - boundarys.y1);
										}
									else if (cself->edgemode == 2)
										if (sx < boundarys.x1 || (sx >= boundarys.x2) || 
											sy < boundarys.y1 || (sy >= boundarys.y2))
										continue;

									kx = cself->orderx - j - 1;
									ky = cself->ordery - i - 1;
									sval = in_pixels[4 * sx + sy * rowstride + ch];
									kval = cself->KernelMatrix[kx + ky * cself->orderx];
									sum += (double) sval *kval;
								}
						tempresult = sum / cself->divisor + cself->bias;

						if (tempresult > 255)
							tempresult = 255;
						if (tempresult < 0)
							tempresult = 0;
						
						output_pixels[4 * x + y * rowstride + ch] = tempresult;
					}
				if (cself->preservealpha)
					output_pixels[4 * x + y * rowstride + 3] =
						in_pixels[4 * x + y * rowstride + 3];
			}
	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_convolve_matrix_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveConvolveMatrix *cself;

	cself = (RsvgFilterPrimitiveConvolveMatrix *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_free (cself->KernelMatrix);
	g_free (cself);
}

void
rsvg_start_filter_primitive_convolve_matrix (RsvgHandle * ctx,
											 const xmlChar ** atts)
{
	int i, j, listlen;
	double font_size;
	RsvgFilterPrimitiveConvolveMatrix *filter;
	
	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveConvolveMatrix, 1);
	
	filter->super.in = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;	
	
	filter->divisor = 0;
	filter->bias = 0;
	filter->targetx = 0;
	filter->targety = 0;
	filter->dx = 0;
	filter->dy = 0;
	
	filter->edgemode = 0;

	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "targetX"))
						filter->targetx = atoi ((char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "targetY"))
						filter->targety = atoi ((char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "bias"))
						filter->bias = atof ((char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "preserveAlpha"))
						{
							if (!strcmp ((char *) atts[i + 1], "true"))
								filter->preservealpha = TRUE;
							else
								filter->preservealpha = FALSE;
						}
					else if (!strcmp ((char *) atts[i], "divisor"))
						filter->divisor = atof ((char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "order"))
						{
							double tempx, tempy;
							rsvg_css_parse_number_optional_number ((char *) atts[i + 1],
																   &tempx, &tempy);
							filter->orderx = tempx;
							filter->ordery = tempy;
							
						}
						else if (!strcmp ((char *) atts[i], "kernelUnitLength"))
							rsvg_css_parse_number_optional_number ((char *) atts[i + 1],
																   &filter->dx, &filter->dy);
							
					else if (!strcmp ((char *) atts[i], "kernelMatrix"))
						filter->KernelMatrix =
							rsvg_css_parse_number_list ((char *) atts[i + 1], &listlen);

					if (!strcmp ((char *) atts[i], "edgeMode")) 
						{
							if (!strcmp ((char *) atts[i + 1], "wrap"))
								filter->edgemode = 1;
							else if (!strcmp ((char *) atts[i + 1], "none"))
								filter->edgemode = 2;
							else
								filter->edgemode = 0;
						}
				}			
		}

	if (filter->divisor == 0)
		{
			for (j = 0; j < filter->orderx; j++)
				for (i = 0; i < filter->ordery; i++)
					filter->divisor += filter->KernelMatrix[j + i * filter->orderx];
		}

	if (filter->divisor == 0)
		filter->divisor = 1;
		
	if (listlen < filter->orderx * filter->ordery)
		filter->orderx = filter->ordery = 0;

	filter->super.render = &rsvg_filter_primitive_convolve_matrix_render;
	filter->super.free = &rsvg_filter_primitive_convolve_matrix_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveGaussianBlur
RsvgFilterPrimitiveGaussianBlur;

struct _RsvgFilterPrimitiveGaussianBlur
{
	RsvgFilterPrimitive super;
	double sdx, sdy;
};


#if PERFECTBLUR != 0
static void
true_blur (GdkPixbuf *in, GdkPixbuf *output, gfloat sdx, 
		   gfloat sdy, FPBox boundarys)
{
	guchar ch;
	gint x, y;
	gint i, j;
	gint rowstride, height, width;
	
	guchar *in_pixels;
	guchar *output_pixels;

	gint sx, sy, kx, ky, kw, kh;
	guchar sval;
	double kval, sum;
	
	double *KernelMatrix;
	double divisor;
	
	gint tempresult;

	kw = kh = 0;

	in_pixels = gdk_pixbuf_get_pixels (in);
	output_pixels = gdk_pixbuf_get_pixels (output);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	/* find out the required x size for the kernel matrix */
	
	for (i = 1; i < 20; i++)
		{
			if (exp (-(i * i) / (2 * sdx * sdx)) / sqrt (2 * M_PI * sdx * sdx) <
				0.0001)
				{
					break;
				}
		}
	kw = 2 * (i - 1);

	/* find out the required y size for the kernel matrix */
	for (i = 1; i < 20; i++)
		{
		if (exp (-(i * i) / (2 * sdy * sdy)) / sqrt (2 * M_PI * sdy * sdy) <
			0.0001)
			{
				break;
			}
    }
	
	kh = 2 * (i - 1);

	KernelMatrix = g_new (double, kw * kh);
	
	/* create the kernel matrix */
	for (i = 0; i < kh; i++)
		{
			for (j = 0; j < kw; j++)
				{
					KernelMatrix[j + i * kw] =
						(exp (-((j - kw / 2) * (j - kw / 2)) / (2 * sdx * sdx)) /
						 sqrt (2 * M_PI * sdx * sdx)) *
						(exp (-((i - kh / 2) * (i - kh / 2)) / (2 * sdy * sdy)) /
						 sqrt (2 * M_PI * sdy * sdy));
				}
		}
	
	/* find out the total of the values of the matrix */
	divisor = 0;
	for (j = 0; j < kw; j++)
		for (i = 0; i < kh; i++)
			divisor += KernelMatrix[j + i * kw];
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			for (ch = 0; ch < 4; ch++)
				{
					sum = 0;
					for (i = 0; i < kh; i++)
						for (j = 0; j < kw; j++)
							{
								sx = x + j - kw / 2;
								sy = y + i - kh / 2;

								if (sx < boundarys.x1)
									sx = boundarys.x1;
								if (sx >= boundarys.x2)
									sx = boundarys.x2 - 1;
								if (sy < boundarys.y1)
									sy = boundarys.y1;
								if (sy >= boundarys.y2)
									sy = boundarys.y2 - 1;

								kx = kw - j - 1;
								ky = kh - i - 1;
								sval = in_pixels[4 * sx + sy * rowstride + ch];
								kval = KernelMatrix[kx + ky * kw];
								sum += (double) sval * kval;
							}

					tempresult = sum / divisor;
					if (tempresult > 255)
						tempresult = 255;
					if (tempresult < 0)
						tempresult = 0;
					
					output_pixels[4 * x + y * rowstride + ch] = tempresult;
				}
	g_free (KernelMatrix);
}

#endif

static void
box_blur (GdkPixbuf *in, GdkPixbuf *output, GdkPixbuf *intermediate, gint kw, 
		  gint kh, FPBox boundarys)
{
	guchar ch;
	gint x, y;
	gint rowstride, height, width;
	
	guchar *in_pixels;
	guchar *output_pixels;

	gint sum;	

	gint divisor;

	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);

	in_pixels = gdk_pixbuf_get_pixels (in);
	output_pixels = gdk_pixbuf_get_pixels (intermediate);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	for (ch = 0; ch < 4; ch++)
		{
			for (y = boundarys.y1; y < boundarys.y2; y++)
				{
					sum = 0;
					divisor = 0;
					for (x = boundarys.x1; x < boundarys.x1 + kw; x++)
						{
							divisor++;
							sum += in_pixels[4 * x + y * rowstride + ch];
							if (x - kw / 2 >= 0 && x - kw / 2 < boundarys.x2)
								{
									output_pixels[4 * (x - kw / 2) + y * rowstride + ch] = sum / divisor;
								}
						}
					for (x = boundarys.x1 + kw; x < boundarys.x2; x++)
						{
							sum -= in_pixels[4 * (x - kw) + y * rowstride + ch];
							sum += in_pixels[4 * x + y * rowstride + ch];
							output_pixels[4 * (x - kw / 2) + y * rowstride + ch] = sum / divisor;
						}
					for (x = boundarys.x2; x < boundarys.x2 + kw; x++)
						{
							divisor--;
							sum -= in_pixels[4 * (x - kw) + y * rowstride + ch];
							if (x - kw / 2 >= 0 && x - kw / 2 < boundarys.x2)
								{
									output_pixels[4 * (x - kw / 2) + y * rowstride + ch] = sum / divisor;
								}
						}
				}
		}


	in_pixels = gdk_pixbuf_get_pixels (intermediate);
	output_pixels = gdk_pixbuf_get_pixels (output);

	for (ch = 0; ch < 4; ch++)
		{
			for (x = boundarys.x1; x < boundarys.x2; x++)
				{
					sum = 0;
					divisor = 0;
					
					for (y = boundarys.y1; y < boundarys.y1 + kh; y++)
						{
							divisor++;
							sum += in_pixels[4 * x + y * rowstride + ch];
							if (y - kh / 2 >= 0 && y - kh / 2 < boundarys.y2)
								{
									output_pixels[4 * x + (y - kh / 2) * rowstride + ch] = sum / divisor;
								}
						}
					for (y = boundarys.y1 + kh; y < boundarys.y2; y++)
						{
							sum -= in_pixels[4 * x + (y - kh) * rowstride + ch];
							sum += in_pixels[4 * x + y * rowstride + ch];
							output_pixels[4 * x + (y - kh / 2) * rowstride + ch] = sum / divisor;
						}
					for (y = boundarys.y2; y < boundarys.y2 + kh; y++)
						{
							divisor--;
							sum -= in_pixels[4 * x + (y - kh) * rowstride + ch];
							if (y - kh / 2 >= 0 && y - kh / 2 < boundarys.y2)
								{
									output_pixels[4 * x + (y - kh / 2) * rowstride + ch] = sum / divisor;
								}
						}
				}
		}
}

static void
fast_blur (GdkPixbuf *in, GdkPixbuf *output, gfloat sx, 
		   gfloat sy, FPBox boundarys)
{
	GdkPixbuf *intermediate1;
	GdkPixbuf *intermediate2;
	gint kx, ky;

	kx = floor(sx * 3*sqrt(2*M_PI)/4 + 0.5);
	ky = floor(sy * 3*sqrt(2*M_PI)/4 + 0.5);

	intermediate1 = gdk_pixbuf_new (GDK_COLORSPACE_RGB, 1, 8, 
									gdk_pixbuf_get_width (in),
									gdk_pixbuf_get_height (in));
	intermediate2 = gdk_pixbuf_new (GDK_COLORSPACE_RGB, 1, 8, 
									gdk_pixbuf_get_width (in),
									gdk_pixbuf_get_height (in));

	box_blur (in, intermediate2, intermediate1, kx, 
			  ky, boundarys);
	box_blur (intermediate2, intermediate2, intermediate1, kx, 
			  ky, boundarys);
	box_blur (intermediate2, output, intermediate1, kx, 
			  ky, boundarys);

	g_object_unref (G_OBJECT (intermediate1));
	g_object_unref (G_OBJECT (intermediate2));
}

static void
rsvg_filter_primitive_gaussian_blur_render (RsvgFilterPrimitive * self,
											RsvgFilterContext * ctx)
{
	RsvgFilterPrimitiveGaussianBlur *cself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	FPBox boundarys;
	gfloat sdx, sdy;
	
	cself = (RsvgFilterPrimitiveGaussianBlur *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, 
									 gdk_pixbuf_get_width (in),
									 gdk_pixbuf_get_height (in));
	
	/* scale the SD values */
	sdx = cself->sdx * ctx->paffine[0];
	sdy = cself->sdy * ctx->paffine[3];
	
#if PERFECTBLUR != 0
	if (sdx * sdy <= PERFECTBLUR)
		true_blur (in, output, sdx, 
				   sdy, boundarys);
	else
		fast_blur (in, output, sdx, 
				   sdy, boundarys);
#else
	fast_blur (in, output, sdx, 
				   sdy, boundarys);
#endif

	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_gaussian_blur_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveGaussianBlur *cself;
	
	cself = (RsvgFilterPrimitiveGaussianBlur *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_free (cself);
}

void
rsvg_start_filter_primitive_gaussian_blur (RsvgHandle * ctx,
										   const xmlChar ** atts)
{
	int i;
	
	double font_size;
	RsvgFilterPrimitiveGaussianBlur *filter;

	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveGaussianBlur, 1);

	filter->super.in = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->sdx = 0;
	filter->sdy = 0;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "stdDeviation"))
						rsvg_css_parse_number_optional_number ((char *) atts[i + 1],
															   &filter->sdx,
															   &filter->sdy);
				}
		}

	filter->super.render = &rsvg_filter_primitive_gaussian_blur_render;
	filter->super.free = &rsvg_filter_primitive_gaussian_blur_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveOffset RsvgFilterPrimitiveOffset;

struct _RsvgFilterPrimitiveOffset
{
	RsvgFilterPrimitive super;
	gint dx, dy;
};

static void
rsvg_filter_primitive_offset_render (RsvgFilterPrimitive * self,
									 RsvgFilterContext * ctx)
{
	guchar ch;
	gint x, y;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveOffset *oself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	
	int ox, oy;
	
	oself = (RsvgFilterPrimitiveOffset *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);
	
	output_pixels = gdk_pixbuf_get_pixels (output);
	
	ox = ctx->paffine[0] * oself->dx;
	oy = ctx->paffine[3] * oself->dy;
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				if (x - ox < boundarys.x1 || x - ox >= boundarys.x2)
					continue;
				if (y - oy < boundarys.y1 || y - oy >= boundarys.y2)
					continue;
		
				for (ch = 0; ch < 4; ch++)
					{
						output_pixels[y * rowstride + x * 4 + ch] =
							in_pixels[(y - oy) * rowstride + (x - ox) * 4 + ch];
					}
			}

	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_offset_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveOffset *oself;
	
	oself = (RsvgFilterPrimitiveOffset *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_free (oself);
}

void
rsvg_start_filter_primitive_offset (RsvgHandle * ctx, const xmlChar ** atts)
{
	int i;
	
	double font_size;
	RsvgFilterPrimitiveOffset *filter;
	
	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveOffset, 1);
	
	filter->super.in = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->dy = 0;
	filter->dx = 0;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "dx"))
						filter->dx =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);
					else if (!strcmp ((char *) atts[i], "dy"))
						filter->dy =
							rsvg_css_parse_normalized_length ((char *) atts[i + 1],
															  ctx->dpi,
															  (gdouble) ctx->width,
															  font_size);
				}
		}
	
	filter->super.render = &rsvg_filter_primitive_offset_render;
	filter->super.free = &rsvg_filter_primitive_offset_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveMerge RsvgFilterPrimitiveMerge;

struct _RsvgFilterPrimitiveMerge
{
	RsvgFilterPrimitive super;
	GPtrArray *nodes;
};

static void
rsvg_filter_primitive_merge_render (RsvgFilterPrimitive * self,
									RsvgFilterContext * ctx)
{
	guint i;
	FPBox boundarys;
	
	RsvgFilterPrimitiveMerge *mself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	
	mself = (RsvgFilterPrimitiveMerge *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, ctx->width, ctx->height);
	
	for (i = 0; i < mself->nodes->len; i++)
		{
			in = rsvg_filter_get_in (g_ptr_array_index (mself->nodes, i), ctx);
			alpha_blt (in, boundarys.x1, boundarys.y1, boundarys.x2 - boundarys.x1,
					   boundarys.y2 - boundarys.y1, output, boundarys.x1,
					   boundarys.y1);
			g_object_unref (G_OBJECT (in));
		}
	
	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_merge_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveMerge *mself;
	guint i;
	
	mself = (RsvgFilterPrimitiveMerge *) self;
	g_string_free (self->result, TRUE);
	
	for (i = 0; i < mself->nodes->len; i++)
		g_string_free (g_ptr_array_index (mself->nodes, i), TRUE);
	g_ptr_array_free (mself->nodes, FALSE);
	g_free (mself);
}

void
rsvg_start_filter_primitive_merge (RsvgHandle * ctx, const xmlChar ** atts)
{
	int i;
	
	double font_size;
	RsvgFilterPrimitiveMerge *filter;
	
	font_size = rsvg_state_current_font_size (ctx);

	filter = g_new (RsvgFilterPrimitiveMerge, 1);
	
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->nodes = g_ptr_array_new ();

	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
				}
		}
	
	filter->super.render = &rsvg_filter_primitive_merge_render;
	filter->super.free = &rsvg_filter_primitive_merge_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
	ctx->currentsubfilter = filter;
}

void
rsvg_start_filter_primitive_merge_node (RsvgHandle * ctx,
										const xmlChar ** atts)
{
	int i;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_ptr_array_add (((RsvgFilterPrimitiveMerge *) (ctx->
																		currentsubfilter))->
										 nodes, g_string_new ((char *) atts[i + 1]));
				}
		}
}

/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveColourMatrix
RsvgFilterPrimitiveColourMatrix;

struct _RsvgFilterPrimitiveColourMatrix
{
	RsvgFilterPrimitive super;
	double *KernelMatrix;
	double divisor;
	gint orderx, ordery;
	double bias;
	gint targetx, targety;
	gboolean preservealpha;
};

static void
rsvg_filter_primitive_colour_matrix_render (RsvgFilterPrimitive * self,
											RsvgFilterContext * ctx)
{
	guchar ch;
	gint x, y;
	gint i;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveColourMatrix *cself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	
	double sum;
	
	gint tempresult;

	cself = (RsvgFilterPrimitiveColourMatrix *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);	
	output_pixels = gdk_pixbuf_get_pixels (output);   
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				for (ch = 0; ch < 4; ch++)
					{
						sum = 0;
						for (i = 0; i < 4; i++)
							{
								sum += cself->KernelMatrix[ch * 5 + i] *
									in_pixels[4 * x + y * rowstride + i];
							}
						sum += cself->KernelMatrix[ch * 5 + 4];
						
						tempresult = sum;
						if (tempresult > 255)
							tempresult = 255;
						if (tempresult < 0)
							tempresult = 0;
						output_pixels[4 * x + y * rowstride + ch] = tempresult;
					}
			}

	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_colour_matrix_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveColourMatrix *cself;

	cself = (RsvgFilterPrimitiveColourMatrix *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_free (cself->KernelMatrix);
	g_free (cself);
}

void
rsvg_start_filter_primitive_colour_matrix (RsvgHandle * ctx,
										   const xmlChar ** atts)
{
	gint i, type, listlen;
	double font_size;
	RsvgFilterPrimitiveColourMatrix *filter;
	
	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveColourMatrix, 1);
	
	filter->super.in = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	
	type = 0;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "values"))
						filter->KernelMatrix =
							rsvg_css_parse_number_list ((char *) atts[i + 1], &listlen);
					else if (!strcmp ((char *) atts[i], "type"))
						{
							if (!strcmp ((char *) atts[i + 1], "matrix"))
								type = 0;
							else if (!strcmp ((char *) atts[i + 1], "saturate"))
								type = 1;
							else if (!strcmp ((char *) atts[i + 1], "hueRotate"))
								type = 2;
							else if (!strcmp ((char *) atts[i + 1], "luminanceToAlpha"))
								type = 3;
							else
								type = 0;
						}
				}			
		}

	if (type == 0)
		{
			if (listlen != 20)
				{
					if (filter->KernelMatrix != NULL)
						g_free (filter->KernelMatrix);
					filter->KernelMatrix = g_new0 (double, 20);
				}
		}
	else if (type == 1)
		{
			float s;
			if (listlen != 0)
				{
					s = filter->KernelMatrix[0];
					g_free (filter->KernelMatrix);
				}
			else
				s = 1;
			filter->KernelMatrix = g_new0 (double, 20);

			filter->KernelMatrix[0] = 0.213 + 0.787 * s;
			filter->KernelMatrix[1] = 0.715 - 0.715 * s;
			filter->KernelMatrix[2] = 0.072 - 0.072 * s;
			filter->KernelMatrix[5] = 0.213 - 0.213 * s;
			filter->KernelMatrix[6] = 0.715 + 0.285 * s;
			filter->KernelMatrix[7] = 0.072 - 0.072 * s;
			filter->KernelMatrix[10] = 0.213 - 0.213 * s;
			filter->KernelMatrix[11] = 0.715 - 0.715 * s;
			filter->KernelMatrix[12] = 0.072 + 0.928 * s;
			filter->KernelMatrix[18] = 1;
		}
	else if (type == 2)
		{
			double cosval, sinval, arg;

			if (listlen != 0)
				{
					arg = filter->KernelMatrix[0];
					g_free (filter->KernelMatrix);
				}
			else
				arg = 0;

			cosval = cos (arg);
			sinval = sin (arg);

			filter->KernelMatrix = g_new0 (double, 20);
			
			filter->KernelMatrix[0] = 0.213 + cosval * 0.787 + sinval * -0.213;
			filter->KernelMatrix[1] = 0.715 + cosval * -0.715 + sinval * -0.715;
			filter->KernelMatrix[2] = 0.072 + cosval * -0.072 + sinval * 0.928;
			filter->KernelMatrix[5] = 0.213 + cosval * -0.213 + sinval * 0.143;
			filter->KernelMatrix[6] = 0.715 + cosval * 0.285 + sinval * 0.140;
			filter->KernelMatrix[7] = 0.072 + cosval * -0.072 + sinval * -0.283;
			filter->KernelMatrix[10] = 0.213 + cosval * -0.213 + sinval * -0.787;
			filter->KernelMatrix[11] = 0.715 + cosval * -0.715 + sinval * 0.715;
			filter->KernelMatrix[12] = 0.072 + cosval * 0.928 + sinval * 0.072;
			filter->KernelMatrix[18] = 1;
		}
	else if (type == 3)
		{
			if (filter->KernelMatrix != NULL)
				g_free (filter->KernelMatrix);

			filter->KernelMatrix = g_new0 (double, 20);

			filter->KernelMatrix[15] = 0.2125;
			filter->KernelMatrix[16] = 0.7154;
			filter->KernelMatrix[17] = 0.0721;
		}
	else 
		{
			g_assert_not_reached();
		}

	filter->super.render = &rsvg_filter_primitive_colour_matrix_render;
	filter->super.free = &rsvg_filter_primitive_colour_matrix_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

struct ComponentTransferData
{
	gdouble *tableValues;
	guint nbTableValues;
	
	gdouble slope;
	gdouble intercept;
	gdouble amplitude;
	gdouble exponent;
	gdouble offset;
};

typedef gdouble (*ComponentTransferFunc) (gdouble C,
										 struct ComponentTransferData *
										 user_data);

typedef struct _RsvgFilterPrimitiveComponentTransfer
RsvgFilterPrimitiveComponentTransfer;


struct _RsvgFilterPrimitiveComponentTransfer
{
	RsvgFilterPrimitive super;
	ComponentTransferFunc Rfunction;
	struct ComponentTransferData Rdata;
	ComponentTransferFunc Gfunction;
	struct ComponentTransferData Gdata;
	ComponentTransferFunc Bfunction;
	struct ComponentTransferData Bdata;
	ComponentTransferFunc Afunction;
	struct ComponentTransferData Adata;
};

static gint
get_component_transfer_table_value (gdouble C,
									struct ComponentTransferData *user_data)
{
	gdouble N;
	gint k;
	N = user_data->nbTableValues;	

	k = floor(C * N);
	k -= 1;
	if (k < 0)
		k = 0;
	return k;
}

static gdouble
identity_component_transfer_func (gdouble C,
								  struct ComponentTransferData *user_data)
{
	return C;
}

static gdouble
table_component_transfer_func (gdouble C,
							   struct ComponentTransferData *user_data)
{
	guint k;
	gdouble vk, vk1;
	gfloat distancefromlast;
	
	if (!user_data->nbTableValues)
		return C;
	
	k = get_component_transfer_table_value (C, user_data);

	if (k == user_data->nbTableValues - 1)
		return user_data->tableValues[k - 1];

	vk = user_data->tableValues[k];
	vk1 = user_data->tableValues[k + 1];
	
	distancefromlast = (C - ((double)k + 1) / (double)user_data->nbTableValues) * (double)user_data->nbTableValues; 

	return (vk + distancefromlast * (vk1 - vk));
}

static gdouble
discrete_component_transfer_func (gdouble C,
								  struct ComponentTransferData *user_data)
{
	gint k;
	
	if (!user_data->nbTableValues)
		return C;
	
	k = get_component_transfer_table_value (C, user_data);
	
	return user_data->tableValues[k];
}

static gdouble
linear_component_transfer_func (gdouble C,
								struct ComponentTransferData *user_data)
{
	return (user_data->slope * C) + user_data->intercept;
}

static gdouble
gamma_component_transfer_func (gdouble C,
							   struct ComponentTransferData *user_data)
{
	return user_data->amplitude * pow (C,
									   user_data->exponent) + user_data->offset;
}

static void 
rsvg_filter_primitive_component_transfer_render (RsvgFilterPrimitive *
												self,
												RsvgFilterContext * ctx)
{
	gint x, y;
	gint temp;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveComponentTransfer *cself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	cself = (RsvgFilterPrimitiveComponentTransfer *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);
	
	output_pixels = gdk_pixbuf_get_pixels (output);

	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				temp = cself->Rfunction((double)in_pixels[y * rowstride + x * 4] / 255.0, &cself->Rdata) * 255.0;
				if (temp > 255)
					temp = 255;
				else if (temp < 0)
					temp = 0;
				output_pixels[y * rowstride + x * 4] = temp;
		
				temp = cself->Gfunction((double)in_pixels[y * rowstride + x * 4 + 1] / 255.0, &cself->Gdata) * 255.0;
				if (temp > 255)
					temp = 255;
				else if (temp < 0)
					temp = 0;
				output_pixels[y * rowstride + x * 4 + 1] = temp;

				temp = cself->Bfunction((double)in_pixels[y * rowstride + x * 4 + 2] / 255.0, &cself->Bdata) * 255.0;
				if (temp > 255)
					temp = 255;
				else if (temp < 0)
					temp = 0;				
				output_pixels[y * rowstride + x * 4 + 2] = temp;

				temp = cself->Afunction((double)in_pixels[y * rowstride + x * 4 + 3] / 255.0, &cself->Adata) * 255.0;
				if (temp > 255)
					temp = 255;
				else if (temp < 0)
					temp = 0;
				output_pixels[y * rowstride + x * 4 + 3] = temp;		
			}
	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void 
rsvg_filter_primitive_component_transfer_free (RsvgFilterPrimitive *
											   self)
{
	RsvgFilterPrimitiveComponentTransfer *cself;

	cself = (RsvgFilterPrimitiveComponentTransfer *) self;
	g_string_free (self->result, TRUE);
	if (cself->Rdata.nbTableValues)
		g_free (cself->Rdata.tableValues);
	if (cself->Gdata.nbTableValues)
		g_free (cself->Gdata.tableValues);
	if (cself->Bdata.nbTableValues)
		g_free (cself->Bdata.tableValues);
	if (cself->Adata.nbTableValues)
		g_free (cself->Adata.tableValues);
	g_free (cself);
}


void 
rsvg_start_filter_primitive_component_transfer (RsvgHandle * ctx,
												const xmlChar ** atts)
{
	int i;
		double font_size;
	RsvgFilterPrimitiveComponentTransfer *filter;
	
	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveComponentTransfer, 1);
	
	filter->super.result = g_string_new ("none");
	filter->super.in = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->Rfunction = identity_component_transfer_func;
	filter->Gfunction = identity_component_transfer_func;
	filter->Bfunction = identity_component_transfer_func;
	filter->Afunction = identity_component_transfer_func;

	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
				}
		}
	
	filter->super.render = &rsvg_filter_primitive_component_transfer_render;
	filter->super.free = &rsvg_filter_primitive_component_transfer_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);

	ctx->currentsubfilter = filter;
}

void 
rsvg_start_filter_primitive_component_transfer_function (RsvgHandle * ctx,
														 const xmlChar ** atts, char channel)
{
	int i;

	ComponentTransferFunc * function;
	struct ComponentTransferData * data;
	
	function = NULL;
	data = NULL;

	if (channel == 'r')
		{
			function = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Rfunction;
			data = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Rdata;
		}
	else if (channel == 'g')
		{
			function = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Gfunction;
			data = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Gdata;
		}
	else if (channel == 'b')
		{
			function = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Bfunction;
			data = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Bdata;
		}
	else if (channel == 'a')
		{
			function = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Afunction;
			data = &((RsvgFilterPrimitiveComponentTransfer *)(ctx->currentsubfilter))->Adata;
		}
	else
		{
			g_assert_not_reached();
		}

	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "type"))
						{
							if (!strcmp ((char *) atts[i + 1], "identity"))
								*function = identity_component_transfer_func;
							else if (!strcmp ((char *) atts[i + 1], "table"))
								*function = table_component_transfer_func;
							else if (!strcmp ((char *) atts[i + 1], "discrete"))
								*function = discrete_component_transfer_func;
							else if (!strcmp ((char *) atts[i + 1], "linear"))
								*function = linear_component_transfer_func;
							else if (!strcmp ((char *) atts[i + 1], "gamma"))
								*function = gamma_component_transfer_func;
						}
					else if (!strcmp ((char *) atts[i], "tableValues"))
						{
							data->tableValues = 
								rsvg_css_parse_number_list ((char *) atts[i + 1], 
															&data->nbTableValues);
						}
					else if (!strcmp ((char *) atts[i], "slope"))
						{
							data->slope = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "intercept"))
						{
							data->intercept = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "amplitude"))
						{
							data->amplitude = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "exponent"))
						{
							data->exponent = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "offset"))
						{
							data->offset = g_ascii_strtod(atts[i + 1], NULL); 
						}
				}
		}
}


/*************************************************************/
/*************************************************************/

typedef struct _RsvgFilterPrimitiveErode
RsvgFilterPrimitiveErode;

struct _RsvgFilterPrimitiveErode
{
	RsvgFilterPrimitive super;
	double rx, ry;
	int mode;
};

static void
rsvg_filter_primitive_erode_render (RsvgFilterPrimitive * self,
									RsvgFilterContext * ctx)
{
	guchar ch, extreme;
	gint x, y;
	gint i, j;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveErode *cself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	
	gint kx, ky;
	guchar val;
	
	cself = (RsvgFilterPrimitiveErode *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	/* scale the radius values */
	kx = cself->rx * ctx->paffine[0];
	ky = cself->ry * ctx->paffine[3];

	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);

	output_pixels = gdk_pixbuf_get_pixels (output);
	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			for (ch = 0; ch < 4; ch++)
				{
					if (cself->mode == 0)
						extreme = 255;
					else
						extreme = 0;
					for (i = -ky; i < ky + 1; i++)
						for (j = -kx; j < kx + 1; j++)
							{
								if (y + i >= height || y + i < 0 || 
									x + j >= width || x + j < 0)
									continue;
								
								val = in_pixels[(y + i) * rowstride 
												+ (x + j) * 4 + ch];
							   

								if (cself->mode == 0)
									{	
										if (extreme > val)
											extreme = val;
									}
								else
									{
										if (extreme < val)
											extreme = val;
									}
								
							}
					output_pixels[y * rowstride + x * 4 + ch] = extreme;
				}
	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_erode_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveErode *cself;
	
	cself = (RsvgFilterPrimitiveErode *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_free (cself);
}

void
rsvg_start_filter_primitive_erode (RsvgHandle * ctx,
								   const xmlChar ** atts)
{
	int i;
	
	double font_size;
	RsvgFilterPrimitiveErode *filter;

	font_size = rsvg_state_current_font_size (ctx);
	
	filter = g_new (RsvgFilterPrimitiveErode, 1);

	filter->super.in = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->rx = 0;
	filter->ry = 0;
	filter->mode = 0;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "radius"))
						{
						rsvg_css_parse_number_optional_number ((char *) atts[i + 1],
															   &filter->rx,
															   &filter->ry);
						}
					else if (!strcmp ((char *) atts[i], "operator"))
						{
							if (!strcmp ((char *) atts[i + 1], "erode"))
								filter->mode = 0;
							else if (!strcmp ((char *) atts[i + 1], "dilate"))
								filter->mode = 1;
						}
				}
		}

	filter->super.render = &rsvg_filter_primitive_erode_render;
	filter->super.free = &rsvg_filter_primitive_erode_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}

/*************************************************************/
/*************************************************************/

typedef enum
{
	COMPOSITE_MODE_OVER, COMPOSITE_MODE_IN, COMPOSITE_MODE_OUT, 
	COMPOSITE_MODE_ATOP, COMPOSITE_MODE_XOR, COMPOSITE_MODE_ARITHMETIC
}
RsvgFilterPrimitiveCompositeMode;

typedef struct _RsvgFilterPrimitiveComposite RsvgFilterPrimitiveComposite;
struct _RsvgFilterPrimitiveComposite
{
	RsvgFilterPrimitive super;
	RsvgFilterPrimitiveCompositeMode mode;
	GString *in2;

	gdouble k1, k2, k3, k4;
};

static void
rsvg_filter_primitive_composite_render (RsvgFilterPrimitive * self,
									RsvgFilterContext * ctx)
{
	guchar i;
	gint x, y;
	gint rowstride, height, width;
	FPBox boundarys;
	
	guchar *in_pixels;
	guchar *in2_pixels;
	guchar *output_pixels;
	
	RsvgFilterPrimitiveComposite *bself;
	
	GdkPixbuf *output;
	GdkPixbuf *in;
	GdkPixbuf *in2;
	
	bself = (RsvgFilterPrimitiveComposite *) self;
	boundarys = rsvg_filter_primitive_get_bounds (self, ctx);
	
	in = rsvg_filter_get_in (self->in, ctx);
	in_pixels = gdk_pixbuf_get_pixels (in);
	in2 = rsvg_filter_get_in (bself->in2, ctx);
	in2_pixels = gdk_pixbuf_get_pixels (in2);
	
	height = gdk_pixbuf_get_height (in);
	width = gdk_pixbuf_get_width (in);
	
	rowstride = gdk_pixbuf_get_rowstride (in);
	
	output = gdk_pixbuf_new_cleared (GDK_COLORSPACE_RGB, 1, 8, width, height);
	output_pixels = gdk_pixbuf_get_pixels (output);
	
	if (bself->mode == COMPOSITE_MODE_ARITHMETIC)
		{
			for (y = boundarys.y1; y < boundarys.y2; y++)
				for (x = boundarys.x1; x < boundarys.x2; x++)
					for (i = 0; i < 4; i++)
						{
							gdouble ca, cb, cr;
							ca = (double) in_pixels[4 * x + y * rowstride + i] / 255.0;
							cb = (double) in2_pixels[4 * x + y * rowstride + i] / 255.0;
							
							cr = bself->k1*ca*cb + bself->k2*ca + bself->k3*cb + bself->k4;

							if (cr > 1)
								cr = 1;
							if (cr < 0)
								cr = 0;
							output_pixels[4 * x + y * rowstride + i] = (guchar)(cr * 255.0);
						}
			
			rsvg_filter_store_result (self->result, output, ctx);
			
			g_object_unref (G_OBJECT (in));
			g_object_unref (G_OBJECT (in2));
			g_object_unref (G_OBJECT (output));
			return;
		}

	
	for (y = boundarys.y1; y < boundarys.y2; y++)
		for (x = boundarys.x1; x < boundarys.x2; x++)
			{
				double qr, cr, qa, qb, ca, cb, Fa, Fb;

				qa = (double) in_pixels[4 * x + y * rowstride + 3] / 255.0;
				qb = (double) in2_pixels[4 * x + y * rowstride + 3] / 255.0;
				cr = 0;
				Fa = Fb = 0;
				switch (bself->mode)
					{
					case COMPOSITE_MODE_OVER:
						Fa = 1;
						Fb = 1 - qa;
						break;
					case COMPOSITE_MODE_IN:
						Fa = qb;
						Fb = 0;
						break;
					case COMPOSITE_MODE_OUT:
						Fa = 1 - qb;
						Fb = 0;
						break;
					case COMPOSITE_MODE_ATOP:
						Fa = qb;
						Fb = 1 - qa;
						break;
					case COMPOSITE_MODE_XOR:
						Fa = 1 - qb;
						Fb = 1 - qa;
						break;
					case COMPOSITE_MODE_ARITHMETIC:
						break;
					}
				
				qr = Fa * qa + Fb * qb;

				for (i = 0; i < 3; i++)
					{
						ca = (double) in_pixels[4 * x + y * rowstride + i] / 255.0 * qa;
						cb = (double) in2_pixels[4 * x + y * rowstride + i] / 255.0 * qb;
					
						cr = (ca * Fa + cb * Fb) / qr;
						if (cr > 1)
							cr = 1;
						if (cr < 0)
							cr = 0;
						output_pixels[4 * x + y * rowstride + i] = (guchar)(cr * 255.0);
						
					}
				if (qr > 1)
					qr = 1;
				if (qr < 0)
					qr = 0;
				output_pixels[4 * x + y * rowstride + 3] = (guchar)(qr * 255.0);
			}

	rsvg_filter_store_result (self->result, output, ctx);
	
	g_object_unref (G_OBJECT (in));
	g_object_unref (G_OBJECT (in2));
	g_object_unref (G_OBJECT (output));
}

static void
rsvg_filter_primitive_composite_free (RsvgFilterPrimitive * self)
{
	RsvgFilterPrimitiveComposite *bself;
	
	bself = (RsvgFilterPrimitiveComposite *) self;
	g_string_free (self->result, TRUE);
	g_string_free (self->in, TRUE);
	g_string_free (bself->in2, TRUE);
	g_free (bself);
}

void
rsvg_start_filter_primitive_composite (RsvgHandle * ctx, const xmlChar ** atts)
{
	int i;
	double font_size;
	RsvgFilterPrimitiveComposite *filter;
	
	font_size = rsvg_state_current_font_size (ctx);

	filter = g_new (RsvgFilterPrimitiveComposite, 1);
	filter->mode = COMPOSITE_MODE_OVER;
	filter->super.in = g_string_new ("none");
	filter->in2 = g_string_new ("none");
	filter->super.result = g_string_new ("none");
	filter->super.sizedefaults = 1;
	filter->k1 = 0;
	filter->k2 = 0;
	filter->k3 = 0;
	filter->k4 = 0;
	
	if (atts != NULL)
		{
			for (i = 0; atts[i] != NULL; i += 2)
				{
					if (!strcmp ((char *) atts[i], "operator")) 
						{
							if (!strcmp ((char *) atts[i + 1], "in"))
								filter->mode = COMPOSITE_MODE_IN;
							else if (!strcmp ((char *) atts[i + 1], "out"))
								filter->mode = COMPOSITE_MODE_OUT;
							else if (!strcmp ((char *) atts[i + 1], "atop"))
								filter->mode = COMPOSITE_MODE_ATOP;
							else if (!strcmp ((char *) atts[i + 1], "xor"))
								filter->mode = COMPOSITE_MODE_XOR;
							else if (!strcmp ((char *) atts[i + 1], 
											  "arithmetic"))
								filter->mode = COMPOSITE_MODE_ARITHMETIC;
							else
								filter->mode = COMPOSITE_MODE_OVER;
						}
					else if (!strcmp ((char *) atts[i], "in"))
						g_string_assign (filter->super.in, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "in2"))
						g_string_assign (filter->in2, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "result"))
						g_string_assign (filter->super.result, (char *) atts[i + 1]);					
					else if (!strcmp ((char *) atts[i], "x"))
						{
							filter->super.x =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "y"))
						{
							filter->super.y =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "width"))
						{
							filter->super.width =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "height"))
						{
							filter->super.height =
								rsvg_css_parse_normalized_length ((char *) atts[i + 1],
																  ctx->dpi,
																  (gdouble) ctx->width,
																  font_size);
							filter->super.sizedefaults = 0;
						}
					else if (!strcmp ((char *) atts[i], "k1"))
						{
							filter->k1 = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "k2"))
						{
							filter->k2 = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "k3"))
						{
							filter->k3 = g_ascii_strtod(atts[i + 1], NULL); 
						}
					else if (!strcmp ((char *) atts[i], "k4"))
						{
							filter->k4 = g_ascii_strtod(atts[i + 1], NULL); 
						}
				}
		}
	
	filter->super.render = &rsvg_filter_primitive_composite_render;
	filter->super.free = &rsvg_filter_primitive_composite_free;
	
	g_ptr_array_add (((RsvgFilter *) (ctx->currentfilter))->primitives,
					 &filter->super);
}