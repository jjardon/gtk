/* gtkextendedlayout.c
 * Copyright (C) 2007-2010 Openismus GmbH
 *
 * Authors:
 *      Mathias Hasselmann <mathias@openismus.com>
 *      Tristan Van Berkom <tristan.van.berkom@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


/**
 * SECTION:gtkextendedlayout
 * @Short_Description: Height-for-width geometry management
 * @Title: GtkExtendedLayout
 *
 * The extended layout is GTK+'s height-for-width (and width-for-height)
 * geometry management system. Height-for-width means that a widget can
 * change how much vertical space it needs, depending on the amount
 * of horizontal space that it is given (and similar for width-for-height).
 * The most common example is a label that reflows to fill up the available
 * width, wraps to fewer lines, and therefore needs less height.
 *
 * GTK+'s traditional two-pass <link linkend="size-allocation">size-allocation</link>
 * algorithm does not allow this flexibility. #GtkWidget provides a default
 * implementation of the #GtkExtendedLayout interface for existing widgets,
 * which always requests the same height, regardless of the available width.
 *
 * <refsect2>
 * <title>Implementing GtkExtendedLayout</title>
 * <para>
 * Some important things to keep in mind when implementing
 * or using the extended layout.
 *
 * The Extended Layout system will query a logical hierarchy in
 * only one orientation at a time. When widgets are initially queried
 * for their minimum sizes it is generally done in a dual pass
 * in the direction chosen by the toplevel.
 *
 * For instance when queried in the normal height-for-width mode:
 * First the default minimum and natural width for each widget
 * in the interface will computed and collectively returned to
 * the toplevel by way of gtk_extended_layout_get_desired_width().
 * Next, the toplevel will use the minimum width to query for the
 * minimum height contextual to that width using
 * gtk_extended_layout_get_height_for_width(), which will also be a
 * highly recursive operation. This minimum-for-minimum size can be
 * used to set the minimum size constraint on the toplevel.
 *
 * When allocating, each container can use the minimum and natural
 * sizes reported by their children to allocate natural sizes and
 * expose as much content as possible with the given allocation.
 *
 * That means that the request operation at allocation time will
 * usually fire again in contexts of different allocated sizes than
 * the ones originally queried for. #GtkExtendedLayout caches a
 * small number of results to avoid re-querying for the same
 * allocated size in one allocation cycle.
 *
 * A widget that does not actually do height-for-width
 * or width-for-height size negotiations only has to implement
 * get_desired_width() and get_desired_height().
 *
 * If a widget does move content around to smartly use up the
 * allocated size, then it must support the request properly in
 * both orientations; even if the request only makes sense in
 * one orientation.
 *
 * For instance, a GtkLabel that does height-for-width word wrapping
 * will not expect to have get_desired_height() called because that
 * call is specific to a width-for-height request, in this case the
 * label must return the heights contextual to its minimum possible
 * width. By following this rule any widget that handles height-for-width
 * or width-for-height requests will always be allocated at least
 * enough space to fit its own content.
 * </para>
 * </refsect2>
 */


#include <config.h>
#include "gtkextendedlayout.h"
#include "gtksizegroup.h"
#include "gtkprivate.h"
#include "gtkintl.h"
#include "gtkalias.h"

/* With extended layout, a widget may be requested
 * its width for 2 or maximum 3 heights in one resize
 */
#define N_CACHED_SIZES 3

typedef struct
{
  guint  age;
  gint   for_size;
  gint   minimum_size;
  gint   natural_size;
} DesiredSize;

typedef struct {
  DesiredSize desired_widths[N_CACHED_SIZES];
  DesiredSize desired_heights[N_CACHED_SIZES];
  guint8      cached_width_age;
  guint8      cached_height_age;
} ExtendedLayoutCache;

static GQuark quark_cache = 0;


GType
gtk_extended_layout_get_type (void)
{
  static GType extended_layout_type = 0;

  if (G_UNLIKELY(!extended_layout_type))
    {
      extended_layout_type =
        g_type_register_static_simple (G_TYPE_INTERFACE, I_("GtkExtendedLayout"),
                                       sizeof (GtkExtendedLayoutIface),
                                       NULL, 0, NULL, 0);

      g_type_interface_add_prerequisite (extended_layout_type, GTK_TYPE_WIDGET);

      quark_cache = g_quark_from_static_string ("gtk-extended-layout-cache");
    }

  return extended_layout_type;
}

/* looks for a cached size request for this for_size. If not
 * found, returns the oldest entry so it can be overwritten
 *
 * Note that this caching code was directly derived from
 * the Clutter toolkit.
 */
static gboolean
get_cached_desired_size (gint           for_size,
                         DesiredSize   *cached_sizes,
                         DesiredSize  **result)
{
  guint i;

  *result = &cached_sizes[0];

  for (i = 0; i < N_CACHED_SIZES; i++)
    {
      DesiredSize *cs;

      cs = &cached_sizes[i];

      if (cs->age > 0 && cs->for_size == for_size)
        {
          *result = cs;
          return TRUE;
        }
      else if (cs->age < (*result)->age)
        {
          *result = cs;
        }
    }

  return FALSE;
}

static void
destroy_cache (ExtendedLayoutCache *cache)
{
  g_slice_free (ExtendedLayoutCache, cache);
}

static ExtendedLayoutCache *
get_cache (GtkExtendedLayout *layout,
           gboolean           create)
{
  ExtendedLayoutCache *cache;

  cache = g_object_get_qdata (G_OBJECT (layout), quark_cache);
  if (!cache && create)
    {
      cache = g_slice_new0 (ExtendedLayoutCache);

      cache->cached_width_age  = 1;
      cache->cached_height_age = 1;

      g_object_set_qdata_full (G_OBJECT (layout), quark_cache, cache,
                               (GDestroyNotify)destroy_cache);
    }

  return cache;
}


static void
do_size_request (GtkWidget *widget)
{
  if (GTK_WIDGET_REQUEST_NEEDED (widget))
    {
      gtk_widget_ensure_style (widget);
      GTK_PRIVATE_UNSET_FLAG (widget, GTK_REQUEST_NEEDED);
      g_signal_emit_by_name (widget,
                             "size-request",
                             &widget->requisition);
    }
}

static void
compute_size_for_orientation (GtkExtendedLayout *layout,
                              GtkSizeGroupMode   orientation,
                              gint               for_size,
                              gint              *minimum_size,
                              gint              *natural_size)
{
  ExtendedLayoutCache *cache;
  DesiredSize         *cached_size;
  GtkWidget           *widget;
  gboolean             found_in_cache = FALSE;

  g_return_if_fail (GTK_IS_EXTENDED_LAYOUT (layout));
  g_return_if_fail (minimum_size != NULL || natural_size != NULL);

  widget = GTK_WIDGET (layout);
  cache  = get_cache (layout, TRUE);

  if (orientation == GTK_SIZE_GROUP_HORIZONTAL)
    {
      cached_size = &cache->desired_widths[0];

      if (!GTK_WIDGET_WIDTH_REQUEST_NEEDED (layout))
        found_in_cache = get_cached_desired_size (for_size, cache->desired_widths, &cached_size);
      else
        {
          memset (cache->desired_widths, 0, N_CACHED_SIZES * sizeof (DesiredSize));
          cache->cached_width_age = 1;
        }
    }
  else
    {
      cached_size = &cache->desired_heights[0];

      if (!GTK_WIDGET_HEIGHT_REQUEST_NEEDED (layout))
        found_in_cache = get_cached_desired_size (for_size, cache->desired_heights, &cached_size);
      else
        {
          memset (cache->desired_heights, 0, N_CACHED_SIZES * sizeof (DesiredSize));
          cache->cached_height_age = 1;
        }
    }

  if (!found_in_cache)
    {
      gint min_size = 0, nat_size = 0;
      gint group_size, requisition_size;

      /* Unconditional size request runs but is often unhandled. */
      do_size_request (widget);

      if (orientation == GTK_SIZE_GROUP_HORIZONTAL)
        {
          requisition_size = widget->requisition.width;

          if (for_size < 0)
            GTK_EXTENDED_LAYOUT_GET_IFACE (layout)->get_desired_width (layout, &min_size, &nat_size);
          else
            GTK_EXTENDED_LAYOUT_GET_IFACE (layout)->get_width_for_height (layout, for_size, &min_size, &nat_size);
        }
      else
        {
          requisition_size = widget->requisition.height;

          if (for_size < 0)
            GTK_EXTENDED_LAYOUT_GET_IFACE (layout)->get_desired_height (layout, &min_size, &nat_size);
          else
            GTK_EXTENDED_LAYOUT_GET_IFACE (layout)->get_height_for_width (layout, for_size, &min_size, &nat_size);
        }

      /* Support for dangling "size-request" signals and forward derived
       * classes that will not default to a ->get_desired_width() that
       * returns the values in the ->requisition cache.
       */
      min_size = MAX (min_size, requisition_size);
      nat_size = MAX (nat_size, requisition_size);

      cached_size->minimum_size = min_size;
      cached_size->natural_size = nat_size;
      cached_size->for_size     = for_size;

      if (orientation == GTK_SIZE_GROUP_HORIZONTAL)
        {
          cached_size->age = cache->cached_width_age;
          cache->cached_width_age++;

          GTK_PRIVATE_UNSET_FLAG (layout, GTK_WIDTH_REQUEST_NEEDED);
        }
      else
        {
          cached_size->age = cache->cached_height_age;
          cache->cached_height_age++;

          GTK_PRIVATE_UNSET_FLAG (layout, GTK_HEIGHT_REQUEST_NEEDED);
        }

      /* Get size groups to compute the base requisition once one
       * of the values have been cached, then go ahead and update
       * the cache with the sizegroup computed value.
       *
       * Note this is also where values from gtk_widget_set_size_request()
       * are considered.
       */
      group_size =
        _gtk_size_group_bump_requisition (GTK_WIDGET (layout),
                                          orientation, cached_size->minimum_size);

      cached_size->minimum_size = MAX (cached_size->minimum_size, group_size);
      cached_size->natural_size = MAX (cached_size->natural_size, group_size);
    }

  if (minimum_size)
    *minimum_size = cached_size->minimum_size;

  if (natural_size)
    *natural_size = cached_size->natural_size;

  g_assert (cached_size->minimum_size <= cached_size->natural_size);

  GTK_NOTE (EXTENDED_LAYOUT,
            g_print ("[%p] %s\t%s: %d is minimum %d and natural: %d (hit cache: %s)\n",
                     layout, G_OBJECT_TYPE_NAME (layout),
                     orientation == GTK_SIZE_GROUP_HORIZONTAL ?
                     "width for height" : "height for width" ,
                     for_size,
                     cached_size->minimum_size,
                     cached_size->natural_size,
                     found_in_cache ? "yes" : "no"));

}

/**
 * gtk_extended_layout_is_height_for_width:
 * @layout: a #GtkExtendedLayout instance
 *
 * Gets whether the widget prefers a height-for-width layout
 * or a width-for-height layout.
 *
 * <note><para>#GtkBin widgets generally propagate the preference of
 * their child, container widgets need to request something either in
 * context of their children or in context of their allocation
 * capabilities.</para></note>
 *
 * Returns: %TRUE if the widget prefers height-for-width, %FALSE if
 * the widget should be treated with a width-for-height preference.
 *
 * Since: 3.0
 */
gboolean
gtk_extended_layout_is_height_for_width (GtkExtendedLayout *layout)
{
  GtkExtendedLayoutIface *iface;

  g_return_val_if_fail (GTK_IS_EXTENDED_LAYOUT (layout), FALSE);

  iface = GTK_EXTENDED_LAYOUT_GET_IFACE (layout);
  if (iface->is_height_for_width)
    return iface->is_height_for_width (layout);

  /* By default widgets are height-for-width. */
  return TRUE;
}

/**
 * gtk_extended_layout_get_desired_width:
 * @layout: a #GtkExtendedLayout instance
 * @minimum_width: (allow-none): location to store the minimum width, or %NULL
 * @natural_width: (allow-none): location to store the natural width, or %NULL
 *
 * Retrieves a widget's initial minimum and natural width.
 *
 * <note><para>This call is specific to height-for-width
 * requests.</para></note>
 *
 * Since: 3.0
 */
void
gtk_extended_layout_get_desired_width (GtkExtendedLayout *layout,
                                       gint              *minimum_width,
                                       gint              *natural_width)
{
  compute_size_for_orientation (layout, GTK_SIZE_GROUP_HORIZONTAL,
                                -1, minimum_width, natural_width);
}


/**
 * gtk_extended_layout_get_desired_height:
 * @layout: a #GtkExtendedLayout instance
 * @minimum_height: (allow-none): location to store the minimum height, or %NULL
 * @natural_height: (allow-none): location to store the natural height, or %NULL
 *
 * Retrieves a widget's initial minimum and natural height.
 *
 * <note><para>This call is specific to width-for-height requests.</para></note>
 *
 * Since: 3.0
 */
void
gtk_extended_layout_get_desired_height (GtkExtendedLayout *layout,
                                        gint              *minimum_height,
                                        gint              *natural_height)
{
  compute_size_for_orientation (layout, GTK_SIZE_GROUP_VERTICAL,
                                -1, minimum_height, natural_height);
}



/**
 * gtk_extended_layout_get_width_for_height:
 * @layout: a #GtkExtendedLayout instance
 * @height: the height which is available for allocation
 * @minimum_width: (allow-none): location for storing the minimum width, or %NULL
 * @natural_width: (allow-none): location for storing the natural width, or %NULL
 *
 * Retrieves a widget's desired width if it would be given
 * the specified @height.
 *
 * Since: 3.0
 */
void
gtk_extended_layout_get_width_for_height (GtkExtendedLayout *layout,
                                          gint               height,
                                          gint              *minimum_width,
                                          gint              *natural_width)
{
  compute_size_for_orientation (layout, GTK_SIZE_GROUP_HORIZONTAL,
                                height, minimum_width, natural_width);
}

/**
 * gtk_extended_layout_get_height_for_width:
 * @layout: a #GtkExtendedLayout instance
 * @width: the width which is available for allocation
 * @minimum_height: (allow-none): location for storing the minimum height, or %NULL
 * @natural_height: (allow-none): location for storing the natural height, or %NULL
 *
 * Retrieves a widget's desired height if it would be given
 * the specified @width.
 *
 * Since: 3.0
 */
void
gtk_extended_layout_get_height_for_width (GtkExtendedLayout *layout,
                                          gint               width,
                                          gint              *minimum_height,
                                          gint              *natural_height)
{
  compute_size_for_orientation (layout, GTK_SIZE_GROUP_VERTICAL,
                                width, minimum_height, natural_height);
}

/**
 * gtk_extended_layout_get_desired_size:
 * @layout: a #GtkExtendedLayout instance
 * @width: the size which is available for allocation
 * @request_natural: Whether to base the contextual request off of the
 *     base natural or the base minimum
 * @minimum_size: (allow-none): location for storing the minimum size, or %NULL
 * @natural_size: (allow-none): location for storing the natural size, or %NULL
 *
 * Retrieves the minimum and natural size of a widget taking
 * into account the widget's preference for height-for-width management.
 *
 * If request_natural is specified, the non-contextual natural value will
 * be used to make the contextual request; otherwise the minimum will be used.
 *
 * This is used to retrieve a suitable size by container widgets which do
 * not impose any restrictions on the child placement.
 *
 * Since: 3.0
 */
void
gtk_extended_layout_get_desired_size (GtkExtendedLayout *layout,
                                      gboolean           request_natural,
                                      GtkRequisition    *minimum_size,
                                      GtkRequisition    *natural_size)
{
  gint min_width, nat_width;
  gint min_height, nat_height;

  g_return_if_fail (GTK_IS_EXTENDED_LAYOUT (layout));

  if (gtk_extended_layout_is_height_for_width (layout))
    {
      gtk_extended_layout_get_desired_width (layout, &min_width, &nat_width);
      gtk_extended_layout_get_height_for_width (layout,
                                                request_natural ? nat_width : min_width,
                                                &min_height, &nat_height);
    }
  else
    {
      gtk_extended_layout_get_desired_height (layout, &min_height, &nat_height);
      gtk_extended_layout_get_width_for_height (layout,
                                                request_natural ? nat_height : min_height,
                                                &min_width, &nat_width);
    }

  if (minimum_size)
    {
      minimum_size->width  = min_width;
      minimum_size->height = min_height;
    }

  if (natural_size)
    {
      natural_size->width  = nat_width;
      natural_size->height = nat_height;
    }
}



#define __GTK_EXTENDED_LAYOUT_C__
#include "gtkaliasdef.c"
