/*
 * actor: Abstract base actor
 * 
 * Copyright 2012-2014 Stephan Haller <nomad@froevel.de>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "actor.h"

#include <glib/gi18n-lib.h>

#include "application.h"
#include "stylable.h"
#include "focusable.h"
#include "utils.h"

/* Define this class in GObject system */
static gpointer				xfdashboard_actor_parent_class=NULL;

void xfdashboard_actor_class_init(XfdashboardActorClass *klass);
void xfdashboard_actor_init(XfdashboardActor *self);
void xfdashboard_actor_base_class_finalize(XfdashboardActorClass *klass);

/* Private structure - access only by public API if needed */
#define XFDASHBOARD_ACTOR_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), XFDASHBOARD_TYPE_ACTOR, XfdashboardActorPrivate))

struct _XfdashboardActorPrivate
{
	/* Properties related */
	gboolean		canFocus;

	gchar			*styleClasses;
	gchar			*stylePseudoClasses;

	/* Instance related */
	GHashTable		*lastThemeStyleSet;
};

/* Properties */
enum
{
	PROP_0,

	PROP_CAN_FOCUS,

	/* Overriden properties of interface: XfdashboardStylable */
	PROP_STYLE_CLASSES,
	PROP_STYLE_PSEUDO_CLASSES,

	PROP_LAST
};

static GParamSpec* XfdashboardActorProperties[PROP_LAST]={ 0, };

/* IMPLEMENTATION: Private variables and methods */
static GParamSpecPool		*_xfdashboard_actor_stylable_properties_pool=NULL;

#define XFDASHBOARD_ACTOR_PARAM_SPEC_REF		(_xfdashboard_actor_param_spec_ref_quark())

/* Quark declarations */
static GQuark _xfdashboard_actor_param_spec_ref_quark(void)
{
	return(g_quark_from_static_string("xfdashboard-actor-param-spec-ref-quark"));
}

/* Invalidate all stylable children recursively beginning at given actor */
static void _xfdashboard_actor_invalidate_recursive(ClutterActor *inActor)
{
	ClutterActor			*child;
	ClutterActorIter		actorIter;

	g_return_if_fail(CLUTTER_IS_ACTOR(inActor));

	/* If actor is stylable invalidate it to get its style recomputed */
	if(XFDASHBOARD_IS_STYLABLE(inActor))
	{
		xfdashboard_stylable_invalidate(XFDASHBOARD_STYLABLE(inActor));
	}

	/* Recompute styles for all children recursively */
	clutter_actor_iter_init(&actorIter, inActor);
	while(clutter_actor_iter_next(&actorIter, &child))
	{
		/* Call ourselve recursive with child as top-level actor.
		 * We return immediately if it has no children but invalidate child
		 * before. If it has children it will first invalidated and will be
		 * iterated over its children then. In both cases the child will
		 * be invalidated.
		 */
		_xfdashboard_actor_invalidate_recursive(child);
	}
}

/* Get parameter specification of stylable properties and add them to hashtable.
 * If requested do it recursively over all parent classes.
 */
static void _xfdashboard_actor_hashtable_get_all_stylable_param_specs(GHashTable *ioHashtable, GObjectClass *inClass, gboolean inRecursive)
{
	GList						*paramSpecs, *entry;
	GObjectClass				*parentClass;

	/* Get list of all parameter specification registered for class */
	paramSpecs=g_param_spec_pool_list_owned(_xfdashboard_actor_stylable_properties_pool, G_OBJECT_CLASS_TYPE(inClass));
	for(entry=paramSpecs; entry; entry=g_list_next(entry))
	{
		GParamSpec				*paramSpec=G_PARAM_SPEC(entry->data);

		/* Only add parameter specification which aren't already in hashtable */
		if(paramSpec &&
			!g_hash_table_lookup_extended(ioHashtable, g_param_spec_get_name(paramSpec), NULL, NULL))
		{
			g_hash_table_insert(ioHashtable,
									g_strdup(g_param_spec_get_name(paramSpec)),
									g_param_spec_ref(paramSpec));
		}
	}
	g_list_free(paramSpecs);

	/* Call us recursive for parent class if it exists and requested */
	parentClass=g_type_class_peek_parent(inClass);
	if(inRecursive && parentClass) _xfdashboard_actor_hashtable_get_all_stylable_param_specs(ioHashtable, parentClass, inRecursive);
}

/* Remove entries from hashtable whose key is a duplicate
 * in other hashtable (given in user data)
 */
static gboolean _xfdashboard_actor_hashtable_is_duplicate_key(gpointer inKey,
																gpointer inValue,
																gpointer inUserData)
{
	GHashTable			*otherHashtable=(GHashTable*)inUserData;

	return(g_hash_table_lookup_extended(otherHashtable, inKey, NULL, NULL));
}

/* Actor was mapped or unmapped */
static void _xfdashboard_actor_on_mapped_changed(GObject *inObject,
													GParamSpec *inSpec,
													gpointer inUserData)
{
	XfdashboardActor		*self;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inObject));

	self=XFDASHBOARD_ACTOR(inObject);

	/* Invalide styling to get it recomputed */
	xfdashboard_stylable_invalidate(XFDASHBOARD_STYLABLE(self));
}

/* Actor was (re)named */
static void _xfdashboard_actor_on_name_changed(GObject *inObject,
												GParamSpec *inSpec,
												gpointer inUserData)
{
	XfdashboardActor			*self;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inObject));

	self=XFDASHBOARD_ACTOR(inObject);

	/* Invalide styling to get it recomputed because its ID (from point
	 * of view of css) has changed. Also invalidate children as they
	 * might reference the old, invalid ID or the new, valid one.
	 */
	_xfdashboard_actor_invalidate_recursive(CLUTTER_ACTOR(self));
}

/* IMPLEMENTATION: Interface XfdashboardFocusable */

/* Check if actor can get focus */
static gboolean _xfdashboard_actor_focusable_can_focus(XfdashboardFocusable *inFocusable)
{
	XfdashboardActor			*self;
	XfdashboardActorPrivate		*priv;

	g_return_val_if_fail(XFDASHBOARD_IS_FOCUSABLE(inFocusable), FALSE);
	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(inFocusable), FALSE);

	self=XFDASHBOARD_ACTOR(inFocusable);
	priv=self->priv;

	/* This actor can only be focused if it is mapped, visibl	e and reactive */
	if(priv->canFocus &&
		CLUTTER_ACTOR_IS_MAPPED(self) &&
		CLUTTER_ACTOR_IS_VISIBLE(self) &&
		CLUTTER_ACTOR_IS_REACTIVE(self))
	{
		return(TRUE);
	}

	/* If we get here this actor does not fulfill the requirements to get focus */
	return(FALSE);
}

/* Set focus to actor */
static void _xfdashboard_actor_focusable_set_focus(XfdashboardFocusable *self)
{
	XfdashboardActor	*actor;

	g_return_if_fail(XFDASHBOARD_IS_FOCUSABLE(self));
	g_return_if_fail(XFDASHBOARD_IS_ACTOR(self));

	actor=XFDASHBOARD_ACTOR(self);

	/* Set focus and style for focus */
	xfdashboard_stylable_add_class(XFDASHBOARD_STYLABLE(actor), "focus");
}

/* Unset focus from actor */
static void _xfdashboard_actor_focusable_unset_focus(XfdashboardFocusable *self)
{
	XfdashboardActor	*actor;

	g_return_if_fail(XFDASHBOARD_IS_FOCUSABLE(self));
	g_return_if_fail(XFDASHBOARD_IS_ACTOR(self));

	actor=XFDASHBOARD_ACTOR(self);

	/* Set focus and style for focus */
	xfdashboard_stylable_remove_class(XFDASHBOARD_STYLABLE(actor), "focus");
}

/* Interface initialization
 * Set up default functions
 */
static void _xfdashboard_actor_focusable_iface_init(XfdashboardFocusableInterface *iface)
{
	iface->can_focus=_xfdashboard_actor_focusable_can_focus;
	iface->set_focus=_xfdashboard_actor_focusable_set_focus;
	iface->unset_focus=_xfdashboard_actor_focusable_unset_focus;
}

/* IMPLEMENTATION: Interface XfdashboardStylable */

/* Get stylable properties of actor */
static void _xfdashboard_actor_stylable_get_stylable_properties(XfdashboardStylable *inStylable,
																		GHashTable *ioStylableProperties)
{
	g_return_if_fail(CLUTTER_IS_ACTOR(inStylable));

	/* Set up hashtable of stylable properties for this instance */
	_xfdashboard_actor_hashtable_get_all_stylable_param_specs(ioStylableProperties,
																G_OBJECT_CLASS(inStylable),
																TRUE);
}

/* Get stylable name of actor */
static const gchar* _xfdashboard_actor_stylable_get_name(XfdashboardStylable *inStylable)
{
	g_return_val_if_fail(CLUTTER_IS_ACTOR(inStylable), NULL);

	return(clutter_actor_get_name(CLUTTER_ACTOR(inStylable)));
}

/* Get stylable parent of actor */
static XfdashboardStylable* _xfdashboard_actor_stylable_get_parent(XfdashboardStylable *inStylable)
{
	ClutterActor			*self;
	ClutterActor			*parent;

	g_return_val_if_fail(CLUTTER_IS_ACTOR(inStylable), NULL);

	self=CLUTTER_ACTOR(inStylable);

	/* Get parent and check if stylable. If not return NULL. */
	parent=clutter_actor_get_parent(self);
	if(!XFDASHBOARD_IS_STYLABLE(parent)) return(NULL);

	/* Return stylable parent */
	return(XFDASHBOARD_STYLABLE(parent));
}

/* Get/set style classes of actor */
static const gchar* _xfdashboard_actor_stylable_get_classes(XfdashboardStylable *inStylable)
{
	XfdashboardActor			*self;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(inStylable), NULL);

	self=XFDASHBOARD_ACTOR(inStylable);

	return(self->priv->styleClasses);
}

static void _xfdashboard_actor_stylable_set_classes(XfdashboardStylable *inStylable, const gchar *inStyleClasses)
{
	XfdashboardActor			*self;
	XfdashboardActorPrivate		*priv;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inStylable));

	self=XFDASHBOARD_ACTOR(inStylable);
	priv=self->priv;

	/* Set value if changed */
	if(g_strcmp0(priv->styleClasses, inStyleClasses))
	{
		/* Set value */
		if(priv->styleClasses)
		{
			g_free(priv->styleClasses);
			priv->styleClasses=NULL;
		}

		if(inStyleClasses) priv->styleClasses=g_strdup(inStyleClasses);

		/* Invalidate style to get it restyled and redrawn. Also invalidate
		 * children as they might reference the old, invalid classes or
		 * the new, valid ones.
		 */
		_xfdashboard_actor_invalidate_recursive(CLUTTER_ACTOR(self));

		/* Notify about property change */
		g_object_notify(G_OBJECT(self), "style-classes");
	}
}

/* Get/set style pseudo-classes of actor */
static const gchar* _xfdashboard_actor_stylable_get_pseudo_classes(XfdashboardStylable *inStylable)
{
	XfdashboardActor			*self;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(inStylable), NULL);

	self=XFDASHBOARD_ACTOR(inStylable);

	return(self->priv->stylePseudoClasses);
}

static void _xfdashboard_actor_stylable_set_pseudo_classes(XfdashboardStylable *inStylable, const gchar *inStylePseudoClasses)
{
	XfdashboardActor			*self;
	XfdashboardActorPrivate		*priv;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inStylable));

	self=XFDASHBOARD_ACTOR(inStylable);
	priv=self->priv;

	/* Set value if changed */
	if(g_strcmp0(priv->stylePseudoClasses, inStylePseudoClasses))
	{
		/* Set value */
		if(priv->stylePseudoClasses)
		{
			g_free(priv->stylePseudoClasses);
			priv->stylePseudoClasses=NULL;
		}

		if(inStylePseudoClasses) priv->stylePseudoClasses=g_strdup(inStylePseudoClasses);

		/* Invalidate style to get it restyled and redrawn. Also invalidate
		 * children as they might reference the old, invalid pseudo-classes
		 * or the new, valid ones.
		 */
		_xfdashboard_actor_invalidate_recursive(CLUTTER_ACTOR(self));

		/* Notify about property change */
		g_object_notify(G_OBJECT(self), "style-pseudo-classes");
	}
}

/* Invalidate style to recompute styles */
static void _xfdashboard_actor_stylable_invalidate(XfdashboardStylable *inStylable)
{
	XfdashboardActor			*self;
	XfdashboardActorPrivate		*priv;
	XfdashboardActorClass		*klass;
	XfdashboardTheme			*theme;
	XfdashboardThemeCSS			*themeCSS;
	GHashTable					*possibleStyleSet;
	GParamSpec					*paramSpec;
	GHashTableIter				hashIter;
	GHashTable					*themeStyleSet;
	gchar						*styleName;
	XfdashboardThemeCSSValue	*styleValue;
	gboolean					didChange;
#ifdef DEBUG
	gboolean					doDebug=FALSE;
#endif

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inStylable));

	self=XFDASHBOARD_ACTOR(inStylable);
	priv=self->priv;
	klass=XFDASHBOARD_ACTOR_GET_CLASS(self);
	didChange=FALSE;

	/* Only recompute style for mapped actors */
	if(!CLUTTER_ACTOR_IS_MAPPED(self)) return;

	/* Get theme CSS */
	theme=xfdashboard_application_get_theme();
	themeCSS=xfdashboard_theme_get_css(theme);

	/* First get list of all stylable properties of this and parent classes.
	 * It is used to determine if key in theme style sets are valid.
	 */
	possibleStyleSet=xfdashboard_actor_get_stylable_properties_full(klass);

#ifdef DEBUG
	if(doDebug)
	{
		gint					i=0;
		gchar					*defaultsKey;
		GValue					defaultsVal=G_VALUE_INIT;
		gchar					*defaultsValStr;
		GParamSpec				*realParamSpec;

		g_debug("Got param specs for %p (%s) with class '%s' and pseudo-class '%s'", self, G_OBJECT_TYPE_NAME(self), priv->styleClasses, priv->stylePseudoClasses);

		g_hash_table_iter_init(&hashIter, possibleStyleSet);
		while(g_hash_table_iter_next(&hashIter, (gpointer*)&defaultsKey, (gpointer*)&paramSpec))
		{
			realParamSpec=(GParamSpec*)g_param_spec_get_qdata(paramSpec, XFDASHBOARD_ACTOR_PARAM_SPEC_REF);

			g_value_init(&defaultsVal, G_PARAM_SPEC_VALUE_TYPE(realParamSpec));
			g_param_value_set_default(realParamSpec, &defaultsVal);

			defaultsValStr=g_strdup_value_contents(&defaultsVal);
			g_debug("%d: param spec [%s] %s=%s\n", ++i, G_OBJECT_CLASS_NAME(klass), defaultsKey, defaultsValStr);
			g_free(defaultsValStr);

			g_value_unset(&defaultsVal);
		}

		g_debug("End of param specs");
	}
#endif

	/* Get style information from theme */
	themeStyleSet=xfdashboard_theme_css_get_properties(themeCSS, XFDASHBOARD_STYLABLE(self));

#ifdef DEBUG
	if(doDebug)
	{
		gint					i=0;

		g_debug("Got styles from theme for %p (%s) with class '%s' and pseudo-class '%s'", self, G_OBJECT_TYPE_NAME(self), priv->styleClasses, priv->stylePseudoClasses);

		g_hash_table_iter_init(&hashIter, themeStyleSet);
		while(g_hash_table_iter_next(&hashIter, (gpointer*)&styleName, (gpointer*)&styleValue))
		{
			g_debug("%d: [%s] %s=%s\n", ++i, styleValue->source, (gchar*)styleName, styleValue->string);
		}

		g_debug("End of styles from theme");
	}
#endif

	/* The 'property-changed' notification will be freezed and thawed
	 * (fired at once) after all stylable properties of this instance are set.
	 */
	g_object_freeze_notify(G_OBJECT(self));

	/* Iterate through style information retrieved from theme and
	 * set the corresponding property in object instance if key
	 * is valid.
	 */
	g_hash_table_iter_init(&hashIter, themeStyleSet);
	while(g_hash_table_iter_next(&hashIter, (gpointer*)&styleName, (gpointer*)&styleValue))
	{
		GValue					cssValue=G_VALUE_INIT;
		GValue					propertyValue=G_VALUE_INIT;
		GParamSpec				*realParamSpec;

		/* Check if key is a valid object property name */
		if(!g_hash_table_lookup_extended(possibleStyleSet, styleName, NULL, (gpointer*)&paramSpec)) continue;

		/* Get original referenced parameter specification. It does not need
		 * to be referenced while converting because it is valid as this
		 * value is stored in hashtable.
		 */
		realParamSpec=(GParamSpec*)g_param_spec_get_qdata(paramSpec, XFDASHBOARD_ACTOR_PARAM_SPEC_REF);

		/* Convert style value to type of object property and set value
		 * if conversion was successful. Otherwise do nothing.
		 */
		g_value_init(&cssValue, G_TYPE_STRING);
		g_value_set_string(&cssValue, styleValue->string);

		g_value_init(&propertyValue, G_PARAM_SPEC_VALUE_TYPE(realParamSpec));

		if(g_param_value_convert(realParamSpec, &cssValue, &propertyValue, FALSE))
		{
			g_object_set_property(G_OBJECT(self), styleName, &propertyValue);
			didChange=TRUE;
#ifdef DEBUG
			if(doDebug)
			{
				gchar					*valstr;

				valstr=g_strdup_value_contents(&propertyValue);
				g_debug("Setting theme value of style property [%s] %s=%s\n", G_OBJECT_CLASS_NAME(klass), styleName, valstr);
				g_free(valstr);
			}
#endif
		}
			else
			{
				g_warning(_("Could not transform CSS string value for property '%s' to type %s of class %s"),
							styleName, g_type_name(G_PARAM_SPEC_VALUE_TYPE(realParamSpec)), G_OBJECT_CLASS_NAME(klass));
			}

		/* Release allocated resources */
		g_value_unset(&propertyValue);
		g_value_unset(&cssValue);
	}

	/* Now remove all duplicate keys in set of properties changed we set the last
	 * time. The remaining keys determine the properties which were set the last
	 * time but not this time and should be restored to their default values.
	 */
	if(priv->lastThemeStyleSet)
	{
		/* Remove duplicate keys from set of last changed properties */
		g_hash_table_foreach_remove(priv->lastThemeStyleSet, (GHRFunc)_xfdashboard_actor_hashtable_is_duplicate_key, themeStyleSet);

		/* Iterate through remaining key and restore corresponding object properties
		 * to their default values.
		 */
		g_hash_table_iter_init(&hashIter, priv->lastThemeStyleSet);
		while(g_hash_table_iter_next(&hashIter, (gpointer*)&styleName, (gpointer*)&paramSpec))
		{
			GValue				propertyValue=G_VALUE_INIT;
			GParamSpec			*realParamSpec;

			/* Check if key is a valid object property name */
			if(!g_hash_table_lookup_extended(possibleStyleSet, styleName, NULL, (gpointer*)&paramSpec)) continue;

			/* Get original referenced parameter specification. It does not need
			 * to be referenced while converting because it is valid as this
			 * value is stored in hashtable.
			 */
			realParamSpec=(GParamSpec*)g_param_spec_get_qdata(paramSpec, XFDASHBOARD_ACTOR_PARAM_SPEC_REF);

			/* Initialize property value to its type and default value */
			g_value_init(&propertyValue, G_PARAM_SPEC_VALUE_TYPE(realParamSpec));
			g_param_value_set_default(realParamSpec, &propertyValue);

			/* Set value at object property */
			g_object_set_property(G_OBJECT(self), styleName, &propertyValue);
			didChange=TRUE;
#ifdef DEBUG
			if(doDebug)
			{
				gchar					*valstr;

				valstr=g_strdup_value_contents(&propertyValue);
				g_debug("Restoring default value of style property [%s] %s=%s\n", G_OBJECT_CLASS_NAME(klass), styleName, valstr);
				g_free(valstr);
			}
#endif

			/* Release allocated resources */
			g_value_unset(&propertyValue);
		}

		/* Release resources of set of last changed properties as we do not need
		 * it anymore.
		 */
		g_hash_table_destroy(priv->lastThemeStyleSet);
		priv->lastThemeStyleSet=NULL;
	}

	/* Remember this set of changed properties for next time to determine properties
	 * which need to be restored to their default value.
	 */
	priv->lastThemeStyleSet=themeStyleSet;

	/* Release allocated resources */
	g_hash_table_destroy(possibleStyleSet);

	/* Force a redraw if any change was made at this actor */
	if(didChange) clutter_actor_queue_redraw(CLUTTER_ACTOR(self));

	/* All stylable properties are set now. So thaw 'property-changed'
	 * notification now and fire all notifications at once.
	 */
	g_object_thaw_notify(G_OBJECT(self));
}

/* Interface initialization
 * Set up default functions
 */
static void _xfdashboard_actor_stylable_iface_init(XfdashboardStylableInterface *iface)
{
	iface->get_stylable_properties=_xfdashboard_actor_stylable_get_stylable_properties;
	iface->get_name=_xfdashboard_actor_stylable_get_name;
	iface->get_parent=_xfdashboard_actor_stylable_get_parent;
	iface->get_classes=_xfdashboard_actor_stylable_get_classes;
	iface->set_classes=_xfdashboard_actor_stylable_set_classes;
	iface->get_pseudo_classes=_xfdashboard_actor_stylable_get_pseudo_classes;
	iface->set_pseudo_classes=_xfdashboard_actor_stylable_set_pseudo_classes;
	iface->invalidate=_xfdashboard_actor_stylable_invalidate;
}

/* IMPLEMENTATION: ClutterActor */

/* Pointer left actor */
static gboolean _xfdashboard_actor_leave_event(ClutterActor *inActor, ClutterCrossingEvent *inEvent)
{
	XfdashboardActor		*self;
	ClutterActorClass		*parentClass;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(inActor), CLUTTER_EVENT_PROPAGATE);

	self=XFDASHBOARD_ACTOR(inActor);

	/* Call parent's virtual function */
	parentClass=CLUTTER_ACTOR_CLASS(xfdashboard_actor_parent_class);
	if(parentClass->leave_event)
	{
		parentClass->leave_event(inActor, inEvent);
	}

	/* Remove pseudo-class ":hover" because pointer left actor */
	xfdashboard_stylable_remove_pseudo_class(XFDASHBOARD_STYLABLE(self), "hover");

	return(CLUTTER_EVENT_PROPAGATE);
}

/* Pointer entered actor */
static gboolean _xfdashboard_actor_enter_event(ClutterActor *inActor, ClutterCrossingEvent *inEvent)
{
	XfdashboardActor		*self;
	ClutterActorClass		*parentClass;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(inActor), CLUTTER_EVENT_PROPAGATE);

	self=XFDASHBOARD_ACTOR(inActor);

	/* Call parent's virtual function */
	parentClass=CLUTTER_ACTOR_CLASS(xfdashboard_actor_parent_class);
	if(parentClass->enter_event)
	{
		parentClass->enter_event(inActor, inEvent);
	}

	/* Add pseudo-class ":hover" because pointer entered actor */
	xfdashboard_stylable_add_pseudo_class(XFDASHBOARD_STYLABLE(self), "hover");

	return(CLUTTER_EVENT_PROPAGATE);
}

/* Actor was (re)parented */
static void _xfdashboard_actor_parent_set(ClutterActor *inActor, ClutterActor *inOldParent)
{
	XfdashboardActor		*self;
	ClutterActorClass		*parentClass;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(inActor));

	self=XFDASHBOARD_ACTOR(inActor);

	/* Call parent's virtual function */
	parentClass=CLUTTER_ACTOR_CLASS(xfdashboard_actor_parent_class);
	if(parentClass->parent_set)
	{
		parentClass->parent_set(inActor, inOldParent);
	}

	/* Invalide styling to get it recomputed because its ID (from point
	 * of view of css) has changed. Also invalidate children as they might
	 * reference the old, invalid parent or the new, valid one.
	 */
	_xfdashboard_actor_invalidate_recursive(CLUTTER_ACTOR(self));
}

/* IMPLEMENTATION: GObject */

/* Dispose this object */
static void _xfdashboard_actor_dispose(GObject *inObject)
{
	XfdashboardActor			*self=XFDASHBOARD_ACTOR(inObject);
	XfdashboardActorPrivate		*priv=self->priv;

	/* Release allocated variables */
	if(priv->styleClasses)
	{
		g_free(priv->styleClasses);
		priv->styleClasses=NULL;
	}

	if(priv->stylePseudoClasses)
	{
		g_free(priv->stylePseudoClasses);
		priv->stylePseudoClasses=NULL;
	}

	if(priv->lastThemeStyleSet)
	{
		g_hash_table_destroy(priv->lastThemeStyleSet);
		priv->lastThemeStyleSet=NULL;
	}

	/* Call parent's class dispose method */
	G_OBJECT_CLASS(xfdashboard_actor_parent_class)->dispose(inObject);
}

/* Set/get properties */
static void _xfdashboard_actor_set_property(GObject *inObject,
											guint inPropID,
											const GValue *inValue,
											GParamSpec *inSpec)
{
	XfdashboardActor			*self=XFDASHBOARD_ACTOR(inObject);

	switch(inPropID)
	{
		case PROP_CAN_FOCUS:
			xfdashboard_actor_set_can_focus(self, g_value_get_boolean(inValue));
			break;

		case PROP_STYLE_CLASSES:
			_xfdashboard_actor_stylable_set_classes(XFDASHBOARD_STYLABLE(self),
													g_value_get_string(inValue));
			break;

		case PROP_STYLE_PSEUDO_CLASSES:
			_xfdashboard_actor_stylable_set_pseudo_classes(XFDASHBOARD_STYLABLE(self),
															g_value_get_string(inValue));
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(inObject, inPropID, inSpec);
			break;
	}
}

static void _xfdashboard_actor_get_property(GObject *inObject,
											guint inPropID,
											GValue *outValue,
											GParamSpec *inSpec)
{
	XfdashboardActor			*self=XFDASHBOARD_ACTOR(inObject);
	XfdashboardActorPrivate		*priv=self->priv;

	switch(inPropID)
	{
		case PROP_CAN_FOCUS:
			g_value_set_boolean(outValue, priv->canFocus);
			break;

		case PROP_STYLE_CLASSES:
			g_value_set_string(outValue, priv->styleClasses);
			break;

		case PROP_STYLE_PSEUDO_CLASSES:
			g_value_set_string(outValue, priv->stylePseudoClasses);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(inObject, inPropID, inSpec);
			break;
	}
}

/* Class initialization
 * Override functions in parent classes and define properties
 * and signals
 */
void xfdashboard_actor_class_init(XfdashboardActorClass *klass)
{
	ClutterActorClass			*clutterActorClass=CLUTTER_ACTOR_CLASS(klass);
	GObjectClass				*gobjectClass=G_OBJECT_CLASS(klass);

	/* Get parent class */
	xfdashboard_actor_parent_class=g_type_class_peek_parent(klass);

	/* Override functions */
	gobjectClass->dispose=_xfdashboard_actor_dispose;
	gobjectClass->set_property=_xfdashboard_actor_set_property;
	gobjectClass->get_property=_xfdashboard_actor_get_property;

	clutterActorClass->parent_set=_xfdashboard_actor_parent_set;
	clutterActorClass->enter_event=_xfdashboard_actor_enter_event;
	clutterActorClass->leave_event=_xfdashboard_actor_leave_event;

	/* Set up private structure */
	g_type_class_add_private(klass, sizeof(XfdashboardActorPrivate));

	/* Create param-spec pool for themable properties */
	g_assert(_xfdashboard_actor_stylable_properties_pool==NULL);
	_xfdashboard_actor_stylable_properties_pool=g_param_spec_pool_new(FALSE);

	/* Define properties */
	XfdashboardActorProperties[PROP_CAN_FOCUS]=
		g_param_spec_boolean("can-focus",
								_("Can focus"),
								_("This flag indicates if this actor can be focused"),
								FALSE,
								G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_CAN_FOCUS, XfdashboardActorProperties[PROP_CAN_FOCUS]);

	g_object_class_override_property(gobjectClass, PROP_STYLE_CLASSES, "style-classes");
	g_object_class_override_property(gobjectClass, PROP_STYLE_PSEUDO_CLASSES, "style-pseudo-classes");

	/* Define stylable properties */
	xfdashboard_actor_install_stylable_property_by_name(klass, "x-expand");
	xfdashboard_actor_install_stylable_property_by_name(klass, "y-expand");
	xfdashboard_actor_install_stylable_property_by_name(klass, "x-align");
	xfdashboard_actor_install_stylable_property_by_name(klass, "y-align");
}

/* Object initialization
 * Create private structure and set up default values
 */
void xfdashboard_actor_init(XfdashboardActor *self)
{
	XfdashboardActorPrivate		*priv;

	priv=self->priv=XFDASHBOARD_ACTOR_GET_PRIVATE(self);

	/* Set up default values */
	priv->canFocus=FALSE;
	priv->styleClasses=NULL;
	priv->stylePseudoClasses=NULL;
	priv->lastThemeStyleSet=NULL;

	/* Connect signals */
	g_signal_connect(self, "notify::mapped", G_CALLBACK(_xfdashboard_actor_on_mapped_changed), NULL);
	g_signal_connect(self, "notify::name", G_CALLBACK(_xfdashboard_actor_on_name_changed), NULL);
}

/* Implementation: GType */

/* Base class finalization */
void xfdashboard_actor_base_class_finalize(XfdashboardActorClass *klass)
{
	GList				*paramSpecs, *entry;

	paramSpecs=g_param_spec_pool_list_owned(_xfdashboard_actor_stylable_properties_pool, G_OBJECT_CLASS_TYPE(klass));
	for(entry=paramSpecs; entry; entry=g_list_next(entry))
	{
		GParamSpec		*paramSpec=G_PARAM_SPEC(entry->data);

		if(paramSpec)
		{
			g_param_spec_pool_remove(_xfdashboard_actor_stylable_properties_pool, paramSpec);

			g_debug("Unregistered stylable property named '%s' for class '%s'",
					g_param_spec_get_name(paramSpec), G_OBJECT_CLASS_NAME(klass));

			g_param_spec_unref(paramSpec);
		}
	}
	g_list_free(paramSpecs);
}

/* Register type to GObject system */
GType xfdashboard_actor_get_type(void)
{
	static GType		actorType=0;

	if(G_UNLIKELY(actorType==0))
	{
		/* Class info */
		const GTypeInfo 		actorInfo=
		{
			sizeof(XfdashboardActorClass),
			NULL,
			(GBaseFinalizeFunc)xfdashboard_actor_base_class_finalize,
			(GClassInitFunc)xfdashboard_actor_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof(XfdashboardActor),
			0,    /* n_preallocs */
			(GInstanceInitFunc)xfdashboard_actor_init,
			NULL, /* value_table */
		};

		/* Implemented interfaces info */
		const GInterfaceInfo	actorStylableInterfaceInfo=
		{
			(GInterfaceInitFunc)_xfdashboard_actor_stylable_iface_init,
			NULL,
			NULL
		};

		const GInterfaceInfo	actorFocusableInterfaceInfo=
		{
			(GInterfaceInitFunc)_xfdashboard_actor_focusable_iface_init,
			NULL,
			NULL
		};

		/* Register class */
		actorType=g_type_register_static(CLUTTER_TYPE_ACTOR,
											g_intern_static_string("XfdashboardActor"),
											&actorInfo,
											0);

		/* Add implemented interfaces */
		g_type_add_interface_static(actorType,
									XFDASHBOARD_TYPE_STYLABLE,
									&actorStylableInterfaceInfo);

		g_type_add_interface_static(actorType,
									XFDASHBOARD_TYPE_FOCUSABLE,
									&actorFocusableInterfaceInfo);
	}

	return(actorType);
}

/* Implementation: Public API */

/* Create new actor */
ClutterActor* xfdashboard_actor_new(void)
{
	return(CLUTTER_ACTOR(g_object_new(XFDASHBOARD_TYPE_ACTOR, NULL)));
}

/* Get/set "can-focus" flag of actor to indicate
 * if this actor is focusable or not.
 */
gboolean xfdashboard_actor_get_can_focus(XfdashboardActor *self)
{
	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR(self), FALSE);

	return(self->priv->canFocus);
}

void xfdashboard_actor_set_can_focus(XfdashboardActor *self, gboolean inCanFous)
{
	XfdashboardActorPrivate		*priv;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR(self));

	priv=self->priv;

	/* Set value if changed */
	if(priv->canFocus!=inCanFous)
	{
		/* Set value */
		priv->canFocus=inCanFous;

		/* Notify about property change */
		g_object_notify_by_pspec(G_OBJECT(self), XfdashboardActorProperties[PROP_CAN_FOCUS]);
	}
}

/* Register stylable property of a class */
void xfdashboard_actor_install_stylable_property(XfdashboardActorClass *klass, GParamSpec *inParamSpec)
{
	GParamSpec		*stylableParamSpec;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR_CLASS(klass));
	g_return_if_fail(G_IS_PARAM_SPEC(inParamSpec));
	g_return_if_fail(inParamSpec->flags & G_PARAM_WRITABLE);
	g_return_if_fail(!(inParamSpec->flags & G_PARAM_CONSTRUCT_ONLY));

	/* Check if param-spec is already registered */
	if(g_param_spec_pool_lookup(_xfdashboard_actor_stylable_properties_pool, g_param_spec_get_name(inParamSpec), G_OBJECT_CLASS_TYPE(klass), FALSE))
	{
		g_warning("Class '%s' already contains a stylable property '%s'",
					G_OBJECT_CLASS_NAME(klass),
					g_param_spec_get_name(inParamSpec));
		return;
	}

	/* Add param-spec to pool of themable properties */
	stylableParamSpec=g_param_spec_internal(G_PARAM_SPEC_TYPE(inParamSpec),
												g_param_spec_get_name(inParamSpec),
												NULL,
												NULL,
												0);
	g_param_spec_set_qdata_full(stylableParamSpec,
									XFDASHBOARD_ACTOR_PARAM_SPEC_REF,
									g_param_spec_ref(inParamSpec),
									(GDestroyNotify)g_param_spec_unref);
	g_param_spec_pool_insert(_xfdashboard_actor_stylable_properties_pool, stylableParamSpec, G_OBJECT_CLASS_TYPE(klass));
	g_debug("Registered stylable property '%s' for class '%s'",
				g_param_spec_get_name(inParamSpec), G_OBJECT_CLASS_NAME(klass));
}

void xfdashboard_actor_install_stylable_property_by_name(XfdashboardActorClass *klass, const gchar *inParamName)
{
	GParamSpec		*paramSpec;

	g_return_if_fail(XFDASHBOARD_IS_ACTOR_CLASS(klass));
	g_return_if_fail(inParamName && inParamName[0]);

	/* Find parameter specification for property name and register it as stylable */
	paramSpec=g_object_class_find_property(G_OBJECT_CLASS(klass), inParamName);
	if(paramSpec) xfdashboard_actor_install_stylable_property(klass, paramSpec);
		else
		{
			g_warning("Cannot register non-existent property '%s' of class '%s'",
						inParamName, G_OBJECT_CLASS_NAME(klass));
		}
}

/* Get hash-table with all stylable properties of this class or
 * recursively of this and all parent classes
 */
GHashTable* xfdashboard_actor_get_stylable_properties(XfdashboardActorClass *klass)
{
	GHashTable		*stylableProps;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR_CLASS(klass), NULL);

	/* Create hashtable and insert stylable properties */
	stylableProps=g_hash_table_new_full(g_str_hash,
											g_str_equal,
											g_free,
											(GDestroyNotify)g_param_spec_unref);
	_xfdashboard_actor_hashtable_get_all_stylable_param_specs(stylableProps, G_OBJECT_CLASS(klass), FALSE);

	return(stylableProps);
}

GHashTable* xfdashboard_actor_get_stylable_properties_full(XfdashboardActorClass *klass)
{
	GHashTable		*stylableProps;

	g_return_val_if_fail(XFDASHBOARD_IS_ACTOR_CLASS(klass), NULL);

	/* Create hashtable and insert stylable properties */
	stylableProps=g_hash_table_new_full(g_str_hash,
											g_str_equal,
											g_free,
											(GDestroyNotify)g_param_spec_unref);
	_xfdashboard_actor_hashtable_get_all_stylable_param_specs(stylableProps, G_OBJECT_CLASS(klass), TRUE);

	return(stylableProps);
}