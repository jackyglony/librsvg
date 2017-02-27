/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 sts=4 ts=4 expandtab: */
/*
   rsvg-marker.c: Marker loading and rendering

   Copyright (C) 2004, 2005 Caleb Moore <c.moore@student.unsw.edu.au>

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

   Authors: Caleb Moore <c.moore@student.unsw.edu.au>
*/

#include "config.h"

#include "rsvg-marker.h"
#include "rsvg-private.h"
#include "rsvg-styles.h"
#include "rsvg-shapes.h"
#include "rsvg-css.h"
#include "rsvg-defs.h"
#include "rsvg-filter.h"
#include "rsvg-mask.h"
#include "rsvg-image.h"
#include "rsvg-path-builder.h"

#include <string.h>
#include <math.h>
#include <errno.h>
#if 0
typedef struct _RsvgMarker RsvgMarker;

struct _RsvgMarker {
    gboolean bbox;
    RsvgLength refX, refY, width, height;
    double orient;
    guint32 preserve_aspect_ratio;
    gboolean orientAuto;
    RsvgViewBox vbox;
};

static void
rsvg_node_marker_set_atts (RsvgNode *node, gpointer impl, RsvgHandle *handle, RsvgPropertyBag *atts)
{
    RsvgMarker *marker = impl;
    const char *value;

    if ((value = rsvg_property_bag_lookup (atts, "viewBox")))
        marker->vbox = rsvg_css_parse_vbox (value);
    if ((value = rsvg_property_bag_lookup (atts, "refX")))
        marker->refX = rsvg_length_parse (value, LENGTH_DIR_HORIZONTAL);
    if ((value = rsvg_property_bag_lookup (atts, "refY")))
        marker->refY = rsvg_length_parse (value, LENGTH_DIR_VERTICAL);
    if ((value = rsvg_property_bag_lookup (atts, "markerWidth")))
        marker->width = rsvg_length_parse (value, LENGTH_DIR_HORIZONTAL);
    if ((value = rsvg_property_bag_lookup (atts, "markerHeight")))
        marker->height = rsvg_length_parse (value, LENGTH_DIR_VERTICAL);
    if ((value = rsvg_property_bag_lookup (atts, "orient"))) {
        if (!strcmp (value, "auto"))
            marker->orientAuto = TRUE;
        else
            marker->orient = rsvg_css_parse_angle (value);
    }
    if ((value = rsvg_property_bag_lookup (atts, "markerUnits"))) {
        if (!strcmp (value, "userSpaceOnUse"))
            marker->bbox = FALSE;
        if (!strcmp (value, "strokeWidth"))
            marker->bbox = TRUE;
    }
    if ((value = rsvg_property_bag_lookup (atts, "preserveAspectRatio")))
        marker->preserve_aspect_ratio = rsvg_aspect_ratio_parse (value);
}

static void
rsvg_node_marker_draw (RsvgNode *node, gpointer impl, RsvgDrawingCtx *ctx, int dominate)
{
    /* nothing; markers are drawn by their referencing shapes */
}

RsvgNode *
rsvg_new_marker (const char *element_name, RsvgNode *parent)
{
    RsvgMarker *marker;

    marker = g_new0 (RsvgMarker, 1);
    marker->orient = 0;
    marker->orientAuto = FALSE;
    marker->preserve_aspect_ratio = RSVG_ASPECT_RATIO_XMID_YMID;
    marker->refX = marker->refY = rsvg_length_parse ("0", LENGTH_DIR_BOTH);
    marker->width = marker->height = rsvg_length_parse ("3", LENGTH_DIR_BOTH);
    marker->bbox = TRUE;
    marker->vbox.active = FALSE;

    return rsvg_rust_cnode_new (RSVG_NODE_TYPE_MARKER,
                                parent,
                                rsvg_state_new (),
                                marker,
                                rsvg_node_marker_set_atts,
                                rsvg_node_marker_draw,
                                g_free);
}
#endif
static gboolean
draw_child (RsvgNode *node, gpointer data)
{
    RsvgDrawingCtx *ctx;

    ctx = data;

    rsvg_node_draw_from_stack (node, ctx, 0);

    return TRUE;
}

void
rsvg_marker_render (const char * marker_name, gdouble xpos, gdouble ypos, gdouble orient, gdouble linewidth,
                    RsvgDrawingCtx * ctx)
{
    RsvgNode *node;
    RsvgMarker *self;
    cairo_matrix_t affine, taffine;
    gdouble rotation;
    RsvgState *state = rsvg_current_state (ctx);

    node = rsvg_drawing_ctx_acquire_node_of_type (ctx, marker_name, RSVG_NODE_TYPE_MARKER);
    if (node == NULL)
        return;

    self = rsvg_rust_cnode_get_impl (node);

    cairo_matrix_init_translate (&taffine, xpos, ypos);
    cairo_matrix_multiply (&affine, &taffine, &state->affine);

    if (self->orientAuto)
        rotation = orient;
    else
        rotation = self->orient * M_PI / 180.;

    cairo_matrix_init_rotate (&taffine, rotation);
    cairo_matrix_multiply (&affine, &taffine, &affine);

    if (self->bbox) {
        cairo_matrix_init_scale (&taffine, linewidth, linewidth);
        cairo_matrix_multiply (&affine, &taffine, &affine);
    }

    if (self->vbox.active) {
        double w, h, x, y;
        w = rsvg_length_normalize (&self->width, ctx);
        h = rsvg_length_normalize (&self->height, ctx);
        x = 0;
        y = 0;

        rsvg_aspect_ratio_compute (self->preserve_aspect_ratio,
                                   self->vbox.rect.width,
                                   self->vbox.rect.height,
                                   &x, &y, &w, &h);

        cairo_matrix_init_scale (&taffine, w / self->vbox.rect.width, h / self->vbox.rect.height);
        cairo_matrix_multiply (&affine, &taffine, &affine);

        rsvg_drawing_ctx_push_view_box (ctx, self->vbox.rect.width, self->vbox.rect.height);
    }

    cairo_matrix_init_translate (&taffine,
                                 -rsvg_length_normalize (&self->refX, ctx),
                                 -rsvg_length_normalize (&self->refY, ctx));
    cairo_matrix_multiply (&affine, &taffine, &affine);

    rsvg_state_push (ctx);
    state = rsvg_current_state (ctx);

    rsvg_state_reinit (state);

    rsvg_state_reconstruct (state, node);

    state->affine = affine;

    rsvg_push_discrete_layer (ctx);

    state = rsvg_current_state (ctx);

    if (!state->overflow) {
        if (self->vbox.active)
            rsvg_add_clipping_rect (ctx, self->vbox.rect.x, self->vbox.rect.y,
                                    self->vbox.rect.width, self->vbox.rect.height);
        else
            rsvg_add_clipping_rect (ctx, 0, 0,
                                    rsvg_length_normalize (&self->width, ctx),
                                    rsvg_length_normalize (&self->height, ctx));
    }

    rsvg_node_foreach_child (node, draw_child, ctx);

    rsvg_pop_discrete_layer (ctx);

    rsvg_state_pop (ctx);
    if (self->vbox.active)
        rsvg_drawing_ctx_pop_view_box (ctx);

    rsvg_drawing_ctx_release_node (ctx, node);
}
