/* GTK - The GIMP Toolkit
 * Copyright (C) 2010 Carlos Garnacho <carlosg@gnome.org>
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gdk/gdk.h>

#include "gtkstylecontext.h"
#include "gtktypebuiltins.h"
#include "gtkintl.h"

#include "gtkalias.h"

typedef struct GtkStyleContextPrivate GtkStyleContextPrivate;
typedef struct GtkStyleProviderData GtkStyleProviderData;

struct GtkStyleProviderData
{
  GtkStyleProvider *provider;
  guint priority;
};

struct GtkStyleContextPrivate
{
  GList *providers;
  GtkStyleSet *store;
};

#define GTK_STYLE_CONTEXT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GTK_TYPE_STYLE_CONTEXT, GtkStyleContextPrivate))

static void gtk_style_context_finalize (GObject *object);


G_DEFINE_TYPE (GtkStyleContext, gtk_style_context, G_TYPE_OBJECT)

static void
gtk_style_context_class_init (GtkStyleContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gtk_style_context_finalize;

  g_type_class_add_private (object_class, sizeof (GtkStyleContextPrivate));
}

static void
gtk_style_context_init (GtkStyleContext *style_context)
{
  GtkStyleContextPrivate *priv;

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (style_context);
  priv->store = gtk_style_set_new ();
}

static GtkStyleProviderData *
style_provider_data_new (GtkStyleProvider *provider,
                         guint             priority)
{
  GtkStyleProviderData *data;

  data = g_slice_new (GtkStyleProviderData);
  data->provider = g_object_ref (provider);
  data->priority = priority;

  return data;
}

static void
style_provider_data_free (GtkStyleProviderData *data)
{
  g_object_unref (data->provider);
  g_slice_free (GtkStyleProviderData, data);
}

static void
gtk_style_context_finalize (GObject *object)
{
  GtkStyleContextPrivate *priv;

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (object);

  g_list_foreach (priv->providers, (GFunc) style_provider_data_free, NULL);
  g_list_free (priv->providers);

  G_OBJECT_CLASS (gtk_style_context_parent_class)->finalize (object);
}

static void
rebuild_properties (GtkStyleContext *context)
{
  GtkStyleContextPrivate *priv;
  GList *list;

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);
  list = priv->providers;

  gtk_style_set_clear (priv->store);

  while (list)
    {
      GtkStyleProviderData *data;
      GtkStyleSet *provider_style;

      data = list->data;
      list = list->next;

      provider_style = gtk_style_provider_get_style (data->provider);
      gtk_style_set_merge (priv->store, provider_style, TRUE);
      g_object_unref (provider_style);
    }
}

void
gtk_style_context_add_provider (GtkStyleContext  *context,
                                GtkStyleProvider *provider,
                                guint             priority)
{
  GtkStyleContextPrivate *priv;
  GtkStyleProviderData *new_data;
  gboolean added = FALSE;
  GList *list;

  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));
  g_return_if_fail (GTK_IS_STYLE_PROVIDER (provider));

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);
  new_data = style_provider_data_new (provider, priority);
  list = priv->providers;

  while (list)
    {
      GtkStyleProviderData *data;

      data = list->data;

      /* Provider was already attached to the style
       * context, remove in order to add the new data
       */
      if (data->provider == provider)
        {
          GList *link;

          link = list;
          list = list->next;

          /* Remove and free link */
          priv->providers = g_list_remove_link (priv->providers, link);
          style_provider_data_free (link->data);
          g_list_free_1 (link);

          continue;
        }

      if (!added &&
          data->priority > priority)
        {
          priv->providers = g_list_insert_before (priv->providers, list, new_data);
          added = TRUE;
        }

      list = list->next;
    }

  if (!added)
    priv->providers = g_list_append (priv->providers, new_data);

  rebuild_properties (context);
}

void
gtk_style_context_remove_provider (GtkStyleContext  *context,
                                   GtkStyleProvider *provider)
{
  GtkStyleContextPrivate *priv;
  gboolean removed = FALSE;
  GList *list;

  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));
  g_return_if_fail (GTK_IS_STYLE_PROVIDER (provider));

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);
  list = priv->providers;

  while (list)
    {
      GtkStyleProviderData *data;

      data = list->data;

      if (data->provider == provider)
        {
          priv->providers = g_list_remove_link (priv->providers, list);
          style_provider_data_free (list->data);
          g_list_free_1 (list);

          removed = TRUE;

          break;
        }

      list = list->next;
    }

  if (removed)
    rebuild_properties (context);
}

void
gtk_style_context_get_property (GtkStyleContext *context,
                                const gchar     *property,
                                GtkStateType     state,
                                GValue          *value)
{
  GtkStyleContextPrivate *priv;

  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));
  g_return_if_fail (property != NULL);
  g_return_if_fail (state < GTK_STATE_LAST);
  g_return_if_fail (value != NULL);

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);
  gtk_style_set_get_property (priv->store, property, state, value);
}

void
gtk_style_context_get_valist (GtkStyleContext *context,
                              GtkStateType     state,
                              va_list          args)
{
  GtkStyleContextPrivate *priv;

  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));
  g_return_if_fail (state < GTK_STATE_LAST);

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);
  gtk_style_set_get_valist (priv->store, state, args);
}

void
gtk_style_context_get (GtkStyleContext *context,
                       GtkStateType     state,
                       ...)
{
  GtkStyleContextPrivate *priv;
  va_list args;

  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));
  g_return_if_fail (state < GTK_STATE_LAST);

  priv = GTK_STYLE_CONTEXT_GET_PRIVATE (context);

  va_start (args, state);
  gtk_style_set_get_valist (priv->store, state, args);
  va_end (args);
}

#define __GTK_STYLE_CONTEXT_C__
#include "gtkaliasdef.c"