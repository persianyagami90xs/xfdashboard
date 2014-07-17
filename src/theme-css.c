/*
 * theme-css: A theme used for rendering xfdashboard actors with CSS.
 *            The parser and the handling of CSS files is heavily based
 *            on mx-css, mx-style and mx-stylable of library mx
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

#include "theme-css.h"

// TODO: #include <clutter/clutter.h>
#include <glib/gi18n-lib.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>

#include "stylable.h"

/* Define this class in GObject system */
G_DEFINE_TYPE(XfdashboardThemeCSS,
				xfdashboard_theme_css,
				G_TYPE_OBJECT)

/* Private structure - access only by public API if needed */
#define XFDASHBOARD_THEME_CSS_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), XFDASHBOARD_TYPE_THEME_CSS, XfdashboardThemeCSSPrivate))

struct _XfdashboardThemeCSSPrivate
{
	/* Instance related */
	GList		*selectors;
	GList		*styles;
	GSList		*names;
};

/* IMPLEMENTATION: Private variables and methods */
typedef struct _XfdashboardThemeCSSSelector			XfdashboardThemeCSSSelector;
struct _XfdashboardThemeCSSSelector
{
	gchar							*type;
	gchar							*id;
	gchar							*class;
	gchar							*pseudoClass;
	XfdashboardThemeCSSSelector		*parent;
	XfdashboardThemeCSSSelector		*ancestor;
	GHashTable						*style;

	const gchar						*name;
	gint							priority;
	guint							line;
	guint							position;
};

typedef struct _XfdashboardThemeCSSSelectorMatch	XfdashboardThemeCSSSelectorMatch;
struct _XfdashboardThemeCSSSelectorMatch
{
	XfdashboardThemeCSSSelector		*selector;
	gint							score;
};

typedef struct _XfdashboardThemeCSSTableCopyData	XfdashboardThemeCSSTableCopyData;
struct _XfdashboardThemeCSSTableCopyData
{
	GHashTable						*table;
	const gchar						*name;
};

/* Create css value instance */
static XfdashboardThemeCSSValue* _xfdashboard_theme_css_value_new(void)
{
	return(g_slice_new0(XfdashboardThemeCSSValue));
}

/* Destroy css value instance */
static void _xfdashboard_theme_css_value_free(XfdashboardThemeCSSValue *self)
{
	g_slice_free(XfdashboardThemeCSSValue, self);
}


/* Copy a property */
static void _xfdashboard_theme_css_copy_table(gpointer *inKey, gpointer *inValue, XfdashboardThemeCSSTableCopyData *inData)
{
	XfdashboardThemeCSSValue		*value;

	value=_xfdashboard_theme_css_value_new();
	value->source=inData->name;
	value->string=(gchar*)inValue;

	g_hash_table_insert(inData->table, inKey, value);
}

/* Free selector match */
static void _xfdashboard_themes_css_selector_match_free(XfdashboardThemeCSSSelectorMatch *inData)
{
	g_slice_free(XfdashboardThemeCSSSelectorMatch, inData);
}

/* Append strings or characters to string in-place */
static gchar* _xfdashboard_theme_css_append_string(gchar *ioString1, const gchar *inString2)
{
	gchar	*tmp;

	if(!ioString1) return(g_strdup(inString2));

	if(!inString2) return(ioString1);

	tmp=g_strconcat(ioString1, inString2, NULL);
	g_free(ioString1);
	return(tmp);
}

static gchar* _xfdashboard_theme_css_append_char(gchar *ioString, gchar inChar)
{
	gchar	*tmp;
	gint	length;

	if(!ioString)
	{
		tmp=g_malloc(2);
		length=0;
	}
		else
		{
			length=strlen(ioString);
			tmp=g_realloc(ioString, length+2);
		}

	tmp[length]=inChar;
	tmp[length+1]='\0';

	return(tmp);
}

/* Destroy selector */
static void _xfdashboard_theme_css_selector_free(XfdashboardThemeCSSSelector *inSelector)
{
	g_return_if_fail(inSelector);

	/* Free allocated resources */
	if(inSelector->type) g_free(inSelector->type);
	if(inSelector->id) g_free(inSelector->id);
	if(inSelector->class) g_free(inSelector->class);
	if(inSelector->pseudoClass) g_free(inSelector->pseudoClass);

	/* Destroy parent selector */
	if(inSelector->parent) _xfdashboard_theme_css_selector_free(inSelector->parent);

	/* Free selector itself */
	g_slice_free(XfdashboardThemeCSSSelector, inSelector);
}

/* Create selector */
static XfdashboardThemeCSSSelector* _xfdashboard_theme_css_selector_new(const gchar *inName,
																			gint inPriority,
																			guint inLine,
																			guint inPosition)
{
	XfdashboardThemeCSSSelector		*selector;

	selector=g_slice_new0(XfdashboardThemeCSSSelector);
	selector->name=inName;
	selector->priority=inPriority;
	selector->line=inLine;
	selector->position=inPosition;
  
	return(selector);
}

/* Parse CSS from stream */
static GTokenType _xfdashboard_theme_css_parse_css_key_value(XfdashboardThemeCSS *self,
																GScanner *inScanner,
																gchar **outKey,
																gchar **outValue)
{
	GTokenType		token;
	gboolean		propertyStartsWithDash;
	gchar			*oldIDFirst;
	gchar			*oldIDNth;
	guint			oldScanIdentifier1char;
	guint			oldChar2Token;
	gchar			*oldCsetSkipChars;
	guint			oldScanStringSQ;
	guint			oldScanStringDQ;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), G_TOKEN_ERROR);
	g_return_val_if_fail(inScanner, G_TOKEN_ERROR);
	g_return_val_if_fail(outKey && *outKey==NULL, G_TOKEN_ERROR);
	g_return_val_if_fail(outValue && *outValue==NULL, G_TOKEN_ERROR);

	propertyStartsWithDash=FALSE;
	oldIDFirst=inScanner->config->cset_identifier_first;
	oldIDNth=inScanner->config->cset_identifier_nth;
	oldScanIdentifier1char=inScanner->config->scan_identifier_1char;
	oldChar2Token=inScanner->config->char_2_token;
	oldCsetSkipChars=inScanner->config->cset_skip_characters;
	oldScanStringSQ=inScanner->config->scan_string_sq;
	oldScanStringDQ=inScanner->config->scan_string_dq;

	/* Parse property name */
	token=g_scanner_get_next_token(inScanner);

	/* Property names can start with '-' but at least it needs
	 * an identifier
	 */
	if(token=='-')
	{
		token=g_scanner_get_next_token(inScanner);
		propertyStartsWithDash=TRUE;
	}

	if(token!=G_TOKEN_IDENTIFIER) return(G_TOKEN_IDENTIFIER);

	/* Build key */
	if(propertyStartsWithDash) *outKey=g_strconcat("-", inScanner->value.v_identifier, NULL);
		else *outKey=g_strdup(inScanner->value.v_identifier);

	/* Key and value must be seperated by a colon */
	token=g_scanner_get_next_token(inScanner);
	if(token!=':') return(':');

	/* Set parser option to parse property value and parse them */
	inScanner->config->cset_identifier_first=G_CSET_a_2_z "#_-0123456789" G_CSET_A_2_Z G_CSET_LATINS G_CSET_LATINC;
	inScanner->config->cset_identifier_nth=inScanner->config->cset_identifier_first;
	inScanner->config->scan_identifier_1char=1;
	inScanner->config->char_2_token=FALSE;
	inScanner->config->cset_skip_characters="\n";
	inScanner->config->scan_string_sq=TRUE;
	inScanner->config->scan_string_dq=TRUE;

	while(inScanner->next_value.v_char != ';')
	{
		token=g_scanner_get_next_token(inScanner);
		switch(token)
		{
			case G_TOKEN_IDENTIFIER:
				*outValue=_xfdashboard_theme_css_append_string(*outValue, inScanner->value.v_identifier);
				break;

			case G_TOKEN_CHAR:
				*outValue=_xfdashboard_theme_css_append_char(*outValue, inScanner->value.v_char);
				break;

			case G_TOKEN_STRING:
				*outValue=_xfdashboard_theme_css_append_string(*outValue, inScanner->value.v_string);
				break;

			default:
				return(';');
		}

		g_scanner_peek_next_token(inScanner);
	}

	/* Property values must end at a semi-colon */
	g_scanner_get_next_token(inScanner);
	if(inScanner->value.v_char!=';') return(';');

	/* Strip leading and trailing whitespace from value */
	g_strstrip(*outValue);

	/* Set old parser options */
	inScanner->config->cset_identifier_nth=oldIDNth;
	inScanner->config->cset_identifier_first=oldIDFirst;
	inScanner->config->scan_identifier_1char=oldScanIdentifier1char;
	inScanner->config->char_2_token=oldChar2Token;
	inScanner->config->cset_skip_characters=oldCsetSkipChars;
	inScanner->config->scan_string_sq=oldScanStringSQ;
	inScanner->config->scan_string_dq=oldScanStringDQ;

	/* Successfully parsed */
	return(G_TOKEN_NONE);
}

static GTokenType _xfdashboard_theme_css_parse_css_styles(XfdashboardThemeCSS *self,
															GScanner *inScanner,
															GHashTable *ioHashtable)
{
	GTokenType		token;
	gchar			*key;
	gchar			*value;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), G_TOKEN_ERROR);
	g_return_val_if_fail(inScanner, G_TOKEN_ERROR);
	g_return_val_if_fail(ioHashtable, G_TOKEN_ERROR);

	/* Check that style begin with left curly bracket */
	token=g_scanner_get_next_token(inScanner);
	if(token!=G_TOKEN_LEFT_CURLY) return(G_TOKEN_LEFT_CURLY);

	/* Parse styles until closing right curly bracket is reached */
	token=g_scanner_peek_next_token(inScanner);
	while(token!=G_TOKEN_RIGHT_CURLY)
	{
		/* Reset key and value variables */
		key=value=NULL;

		/* Parse key and value */
		token=_xfdashboard_theme_css_parse_css_key_value(self, inScanner, &key, &value);
		if(token!=G_TOKEN_NONE) return(token);

		/* Insert key and value into hashtable */
		g_hash_table_insert(ioHashtable, key, value);

		/* Get next token */
		token=g_scanner_peek_next_token(inScanner);
	}

	/* If we get here we expect the right curly bracket */
	token=g_scanner_get_next_token(inScanner);
	if(token!=G_TOKEN_RIGHT_CURLY) return(G_TOKEN_RIGHT_CURLY);

	/* Successfully parsed */
	return(G_TOKEN_NONE);
}

static GTokenType _xfdashboard_theme_css_parse_css_simple_selector(XfdashboardThemeCSS *self,
																	GScanner *inScanner,
																	XfdashboardThemeCSSSelector *ioSelector)
{
	GTokenType		token;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), G_TOKEN_ERROR);
	g_return_val_if_fail(inScanner, G_TOKEN_ERROR);
	g_return_val_if_fail(ioSelector, G_TOKEN_ERROR);

	/* Parse type of selector. It is optional as '*' can be used as wildcard */
	token=g_scanner_peek_next_token(inScanner);
	switch((guint)token)
	{
		case '*':
			g_scanner_get_next_token(inScanner);
			ioSelector->type=g_strdup("*");

			/* Check if next token follows directly after this identifier.
			 * It is determine by checking if scanner needs to move more than
			 * one (the next) character. If there is a gap then either a new
			 * selector follows or it is a new typeless selector.
			 */
			token=g_scanner_peek_next_token(inScanner);
			if(inScanner->next_line==g_scanner_cur_line(inScanner) &&
				(inScanner->next_position-g_scanner_cur_position(inScanner))>1)
			{
				return(G_TOKEN_NONE);
			}
			break;

		case G_TOKEN_IDENTIFIER:
			g_scanner_get_next_token(inScanner);
			ioSelector->type=g_strdup(inScanner->value.v_identifier);

			/* Check if next token follows directly after this identifier.
			 * It is determine by checking if scanner needs to move more than
			 * one (the next) character. If there is a gap then either a new
			 * selector follows or it is a new typeless selector.
			 */
			token=g_scanner_peek_next_token(inScanner);
			if(inScanner->next_line==g_scanner_cur_line(inScanner) &&
				(inScanner->next_position-g_scanner_cur_position(inScanner))>1)
			{
				return(G_TOKEN_NONE);
			}
			break;

		default:
			break;
	}

	/* Here we look for '#', '.' or ':' and return if we find anything else */
	token=g_scanner_peek_next_token(inScanner);
	while(token!=G_TOKEN_NONE)
	{
		switch((guint)token)
		{
			/* Parse ID (widget name) */
			case '#':
				g_scanner_get_next_token(inScanner);
				token=g_scanner_get_next_token(inScanner);
				if(token!=G_TOKEN_IDENTIFIER) return(G_TOKEN_IDENTIFIER);
				ioSelector->id=g_strdup(inScanner->value.v_identifier);
				break;

			/* Parse class */
			case '.':
				g_scanner_get_next_token(inScanner);
				token=g_scanner_get_next_token(inScanner);
				if(token!=G_TOKEN_IDENTIFIER) return(G_TOKEN_IDENTIFIER);
				ioSelector->class=g_strdup(inScanner->value.v_identifier);
				break;

			/* Parse pseudo-class */
			case ':':
				g_scanner_get_next_token(inScanner);
				token=g_scanner_get_next_token(inScanner);
				if(token!=G_TOKEN_IDENTIFIER) return(G_TOKEN_IDENTIFIER);

				if(ioSelector->pseudoClass)
				{
					/* Remember old pseudo-class as it can only be freed afterwards */
					gchar		*oldPseudoClass=ioSelector->pseudoClass;

					/* Create new pseudo-class */
					ioSelector->pseudoClass=g_strconcat(ioSelector->pseudoClass,
															":",
															inScanner->value.v_identifier,
															NULL);

					/* Now free old pseudo-class */
					g_free(oldPseudoClass);
				}
					else
					{
						ioSelector->pseudoClass=g_strdup(inScanner->value.v_identifier);
					}

				break;

			default:
				return(G_TOKEN_NONE);
		}

		/* Get next token */
		token=g_scanner_peek_next_token(inScanner);
	}

	/* Successfully parsed */
	return(G_TOKEN_NONE);
}

static GTokenType _xfdashboard_theme_css_parse_css_ruleset(XfdashboardThemeCSS *self,
															GScanner *inScanner,
															GList **ioSelectors)
{
	GTokenType						token;
	XfdashboardThemeCSSSelector		*selector, *parent;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), G_TOKEN_ERROR);
	g_return_val_if_fail(inScanner, G_TOKEN_ERROR);
	g_return_val_if_fail(ioSelectors, G_TOKEN_ERROR);

	/* Parse comma-seperated selectors until a left curly bracket is found */
	parent=NULL;
	selector=NULL;

	token=g_scanner_peek_next_token(inScanner);
	while(token!=G_TOKEN_LEFT_CURLY)
	{
		switch((guint)token)
		{
			case G_TOKEN_IDENTIFIER:
			case '*':
			case '#':
			case '.':
			case ':':
				/* Set last selector as parent if available */
				if(selector) parent=selector;
					else parent=NULL;

				/* Check if there was a previous selector and if so, the new one
				 * should use the previous selector to match an ancestor
				 */
				selector=_xfdashboard_theme_css_selector_new(inScanner->input_name,
																GPOINTER_TO_INT(inScanner->user_data),
																g_scanner_cur_line(inScanner),
																g_scanner_cur_position(inScanner));
				*ioSelectors=g_list_prepend(*ioSelectors, selector);

				/* If parent available remove it from list of selectors and
				 * link it to the new selector
				 */
				if(parent)
				{
					*ioSelectors=g_list_remove(*ioSelectors, parent);
					selector->ancestor=parent;
				}

				/* Parse selector */
				token=_xfdashboard_theme_css_parse_css_simple_selector(self, inScanner, selector);
				if(token!=G_TOKEN_NONE) return(token);
				break;

			case '>':
				g_scanner_get_next_token(inScanner);

				/* Set last selector as parent */
				if(!selector) g_warning("No parent when parsing '>'");
				parent=selector;

				/* Create new selector */
				selector=_xfdashboard_theme_css_selector_new(inScanner->input_name,
																GPOINTER_TO_INT(inScanner->user_data),
																g_scanner_cur_line(inScanner),
																g_scanner_cur_position(inScanner));
				*ioSelectors=g_list_prepend(*ioSelectors, selector);

				/* Remove parent from list of selectors and
				 * link it to the new selector
				 */
				selector->parent=parent;
				*ioSelectors=g_list_remove(*ioSelectors, parent);

				/* Parse selector */
				token=_xfdashboard_theme_css_parse_css_simple_selector(self, inScanner, selector);
				if(token!=G_TOKEN_NONE) return(token);
				break;

			case ',':
				g_scanner_get_next_token(inScanner);

				/* Create new selector */
				selector=_xfdashboard_theme_css_selector_new(inScanner->input_name,
																GPOINTER_TO_INT(inScanner->user_data),
																g_scanner_cur_line(inScanner),
																g_scanner_cur_position(inScanner));
				*ioSelectors=g_list_prepend(*ioSelectors, selector);

				/* Parse selector */
				token=_xfdashboard_theme_css_parse_css_simple_selector(self, inScanner, selector);
				if(token!=G_TOKEN_NONE) return(token);
				break;

			default:
				g_scanner_get_next_token(inScanner);
				g_scanner_unexp_token(inScanner,
										G_TOKEN_ERROR,
										NULL,
										NULL,
										NULL,
										_("Unhandled selector"),
										1);
				return('{');
		}

		token=g_scanner_peek_next_token(inScanner);
	}

	/* Successfully parsed */
	return(G_TOKEN_NONE);
}

static GTokenType _xfdashboard_theme_css_parse_css_block(XfdashboardThemeCSS *self,
															GScanner *inScanner,
															GList **ioSelectors,
															GList **ioStyles)
{
	GTokenType						token;
	GList							*selectors;
	GList							*entry;
	GHashTable						*styles;
	XfdashboardThemeCSSSelector		*selector;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), G_TOKEN_ERROR);
	g_return_val_if_fail(inScanner, G_TOKEN_ERROR);
	g_return_val_if_fail(ioSelectors, G_TOKEN_ERROR);
	g_return_val_if_fail(ioStyles, G_TOKEN_ERROR);

	selectors=NULL;
	styles=NULL;

	/* CSS blocks begin with rulesets (list of selectors) - parse them */
	token=_xfdashboard_theme_css_parse_css_ruleset(self, inScanner, &selectors);
	if(token!=G_TOKEN_NONE)
	{
		g_list_foreach(selectors, (GFunc)_xfdashboard_theme_css_selector_free, NULL);
		g_list_free(selectors);

		return(token);
	}

	/* Create a hash table for the properties */
	styles=g_hash_table_new_full(g_str_hash,
									g_direct_equal,
									g_free,
									(GDestroyNotify)g_free);

	token=_xfdashboard_theme_css_parse_css_styles(self, inScanner, styles);

	/* Assign all the selectors to this style */
	for(entry=selectors; entry; entry=g_list_next(entry))
	{
		selector=(XfdashboardThemeCSSSelector*)entry->data;
		selector->style=styles;
	}

	/* Store selectors and styles */
	*ioStyles=g_list_append(*ioStyles, styles);
	*ioSelectors=g_list_concat(*ioSelectors, selectors);

	return(token);
}

static gboolean _xfdashboard_theme_css_parse_css(XfdashboardThemeCSS *self,
													GInputStream *inStream,
													const gchar *inName,
													gint inPriority,
													GError **outError)
{
	XfdashboardThemeCSSPrivate		*priv;
	GScanner						*scanner;
	GTokenType						token;
	gboolean						success;
	GList							*selectors;
	GList							*styles;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), FALSE);
	g_return_val_if_fail(G_IS_INPUT_STREAM(inStream), FALSE);
	g_return_val_if_fail(outError==NULL || *outError==NULL, FALSE);

	priv=self->priv;
	success=TRUE;
	selectors=NULL;
	styles=NULL;

	/* Create scanner object with default settings */
	scanner=g_scanner_new(NULL);
	scanner->input_name=inName;
	scanner->user_data=GINT_TO_POINTER(inPriority);

	/* Set up scanner config
	 * - Identifiers are allowed to contain '-' (minus sign) as non-first characters
	 * - Disallow scanning float values as we need '.' for identifiers
	 * - Set up single comment line not to include '#' as this character is need for identifiers
	 * - Disable parsing HEX values
	 * - Identifiers cannot be single quoted
	 * - Identifiers cannot be double quoted
	 */
	scanner->config->cset_identifier_nth=G_CSET_a_2_z "-_0123456789" G_CSET_A_2_Z G_CSET_LATINS G_CSET_LATINC;
	scanner->config->scan_float=FALSE;
	scanner->config->cpair_comment_single="\1\n";
	scanner->config->scan_hex=FALSE;
	scanner->config->scan_string_sq=FALSE;
	scanner->config->scan_string_dq=FALSE;

	/* Set input stream */
	if(G_IS_FILE_DESCRIPTOR_BASED(inStream))
	{
		g_scanner_input_file(scanner,
								g_file_descriptor_based_get_fd(G_FILE_DESCRIPTOR_BASED(inStream)));
	}
		else
		{
			/* Set error */
			g_set_error(outError,
						XFDASHBOARD_THEME_CSS_ERROR,
						XFDASHBOARD_THEME_CSS_ERROR_UNSUPPORTED_STREAM,
						_("The input stream of type %s is not supported"),
						G_OBJECT_TYPE_NAME(inStream));

			/* Destroy scanner */
			g_scanner_destroy(scanner);

			/* Return failure result */
			return(FALSE);
		}

	/* Parse input stream */
	token=g_scanner_peek_next_token(scanner);
	while(token!=G_TOKEN_EOF)
	{
		token=_xfdashboard_theme_css_parse_css_block(self, scanner, &selectors, &styles);
		if(token!=G_TOKEN_NONE) break;

		/* Get next token of input stream */
		token=g_scanner_peek_next_token(scanner);
	}

	/* Check that we reached end of stream when we stopped parsing */
	if(token!=G_TOKEN_EOF)
	{
		/* It is not the end of stream so print parser error message
		 * and set error
		 */
		g_scanner_unexp_token(scanner,
								G_TOKEN_EOF,
								NULL,
								NULL,
								NULL,
								_("Parser did not reach end of stream"),
								TRUE);

		g_set_error(outError,
					XFDASHBOARD_THEME_CSS_ERROR,
					XFDASHBOARD_THEME_CSS_ERROR_PARSER_ERROR,
					_("Parser did not reach end of stream"));

		success=FALSE;

		/* Free selectors and styles */
		g_list_foreach(selectors, (GFunc)_xfdashboard_theme_css_selector_free, NULL);
		g_list_free(selectors);
		selectors=NULL;

		g_list_foreach(styles, (GFunc)g_hash_table_destroy, NULL);
		g_list_free(styles);
		styles=NULL;
	}

	/* Store selectors and styles */
	if(selectors)
	{
		priv->selectors=g_list_concat(priv->selectors, selectors);
		g_debug("Successfully parsed '%s' and added %d selectors - total %d selectors", inName, g_list_length(selectors), g_list_length(priv->selectors));
	}

	if(styles)
	{
		priv->styles=g_list_concat(priv->styles, styles);
		g_debug("Successfully parsed '%s' and added %d styles - total %d style", inName, g_list_length(styles), g_list_length(priv->styles));
	}

	/* Destroy scanner */
	g_scanner_destroy(scanner);

	/* Return success result */
	return(success);
}

/* Check if haystack contains needle.
 * The haystack is a string representing a list which entries is seperated
 * by a seperator character. This function looks up the haystack if it
 * contains an entry matching the needle and returns TRUE in this case.
 * Otherwise FALSE is returned. A needle length of -1 signals that needle
 * is a NULL-terminated string and length should be determine automatically.
 */
static gboolean _xfdashboard_theme_css_list_contains(const gchar *inNeedle,
														gint inNeedleLength,
														const gchar *inHaystack,
														gchar inSeperator)
{
	const gchar		*start;

	g_return_val_if_fail(inNeedle && *inNeedle!=0, FALSE);
	g_return_val_if_fail(inNeedleLength>0 || inNeedleLength==-1, FALSE);
	g_return_val_if_fail(inHaystack && *inHaystack!=0, FALSE);
	g_return_val_if_fail(inSeperator, FALSE);

	/* If given length of needle is negative it is a NULL-terminated string */
	if(inNeedleLength<0) inNeedleLength=strlen(inNeedle);

	/* Lookup needle in haystack */
	for(start=inHaystack; start; start=strchr(start, inSeperator))
	{
		gint		length;
		gchar		*nextEntry;

		/* Move to character after separator */
		if(start[0]==inSeperator) start++;

		/* Find end of this haystack entry */
		nextEntry=strchr(start, inSeperator);
		if(!nextEntry) length=strlen(start);
			else length=nextEntry-start;

		/* If enrty in haystack is not of same length as needle,
		 * then it is not a match
		 */
		if(length!=inNeedleLength) continue;

		if(!strncmp(inNeedle, start, inNeedleLength)) return(TRUE);
	}

	/* Needle was not found */
	return(FALSE);
}

/* Score given selector againt stylable node */
static gint _xfdashboard_themes_css_score_node_matching_selector(XfdashboardThemeCSSSelector *inSelector,
																	XfdashboardStylable *inStylable)
{
	gint					score;
	gint					a, b, c;
	const gchar				*type="*";
	const gchar				*classes;
	const gchar				*pseudoClasses;
	const gchar				*id;
	// TODO: XfdashboardStylable		*actor;
	XfdashboardStylable		*parent;

	g_return_val_if_fail(XFDASHBOARD_IS_STYLABLE(inStylable), -1);

	/* For information about how the scoring is done, see documentation
	 * "Cascading Style Sheets, level 1" of W3C, section "3.2 Cascading order"
	 * URL: http://www.w3.org/TR/2008/REC-CSS1-20080411/#cascading-order
	 *
	 * 1. Find all declarations that apply to the element/property in question.
	 *    Declarations apply if the selector matches the element in question.
	 *    If no declarations apply, the inherited value is used. If there is
	 *    no inherited value (this is the case for the 'HTML' element and
	 *    for properties that do not inherit), the initial value is used.
	 * 2. Sort the declarations by explicit weight: declarations marked
	 *    '!important' carry more weight than unmarked (normal) declarations.
	 * 3. Sort by origin: the author's style sheets override the reader's
	 *    style sheet which override the UA's default values. An imported
	 *    style sheet has the same origin as the style sheet from which it
	 *    is imported.
	 * 4. Sort by specificity of selector: more specific selectors will
	 *    override more general ones. To find the specificity, count the
	 *    number of ID attributes in the selector (a), the number of CLASS
	 *    attributes in the selector (b), and the number of tag names in
	 *    the selector (c). Concatenating the three numbers (in a number
	 *    system with a large base) gives the specificity.
	 *    Pseudo-elements and pseudo-classes are counted as normal elements
	 *    and classes, respectively.
	 * 5. Sort by order specified: if two rules have the same weight, the
	 *    latter specified wins. Rules in imported style sheets are considered
	 *    to be before any rules in the style sheet itself.
	 *
	 * NOTE: Keyword '!important' is not supported.
	 */
	a=b=c=0;

	/* Get properties for given stylable */
	id=xfdashboard_stylable_get_name(XFDASHBOARD_STYLABLE(inStylable));
	classes=xfdashboard_stylable_get_classes(XFDASHBOARD_STYLABLE(inStylable));
	pseudoClasses=xfdashboard_stylable_get_pseudo_classes(XFDASHBOARD_STYLABLE(inStylable));

	/* Check and score type of selectors but ignore NULL or universal selectors */
	if(inSelector->type && inSelector->type[0]!='*')
	{
		GType				typeID;
		gint				matched;
		gint				depth;

		typeID=G_OBJECT_CLASS_TYPE(G_OBJECT_GET_CLASS(inStylable));
		type=g_type_name(typeID);
		matched=FALSE;

		depth=10;
		while(type)
		{
			if(!strcmp(inSelector->type, type))
			{
				matched=depth;
				break;
			}
				else
				{
					typeID=g_type_parent(typeID);
					type=g_type_name(typeID);
					if(depth>1) depth--;
				}
		}

		if(!matched) return(-1);

		/* Score type of selector */
		c+=depth;
	}

	/* Check and score ID */
	if(inSelector->id)
	{
		/* If node has no ID return immediately */
		if(!id || strcmp(inSelector->id, id)) return(-1);

		/* Score ID */
		a+=10;
	}

	/* Check and score pseudo classes */
	if(inSelector->pseudoClass)
	{
		gchar				*needle;
		gint				numberMatches;

		/* If node has no pseudo class return immediately */
		if(!pseudoClasses) return(-1);

		/* Check that each pseudo-class from the selector appears in the
		 * pseudo-classes from the node, i.e. the selector pseudo-class list
		 * is a subset of the node's pseudo-class list
		 */
		numberMatches=0;
		for(needle=inSelector->pseudoClass; needle; needle=strchr(needle, ':'))
		{
			gint			needleLength;
			gchar			*nextNeedle;

			/* Move pointer of needle beyond pseudo-class seperator ':' */
			if(needle[0]==':') needle++;

			/* Get length of needle */
			nextNeedle=strchr(needle, ':');
			if(nextNeedle) needleLength=nextNeedle-needle;
				else needleLength=strlen(needle);

			/* If pseudo-class from the selector does not appear in the
			 * list of pseudo-classes from the node, then this is not a
			 * match
			 */
			if(!_xfdashboard_theme_css_list_contains(needle, needleLength, pseudoClasses, ':')) return(-1);
			numberMatches++;
		}

		/* Score matching pseudo-class */
		b=b+(10*numberMatches);
	}

	/* Check and score class */
	if(inSelector->class)
	{
		/* If node has no class return immediately */
		if(!classes) return(-1);

		/* Return also if class in selector does not match any node class */
		if(!_xfdashboard_theme_css_list_contains(inSelector->class, strlen(inSelector->class), classes, '.')) return(-1);

		/* Score matching class */
		b=b+10;
	}

	/* Check and score parent */
	parent=xfdashboard_stylable_get_parent(inStylable);
	if(parent && !XFDASHBOARD_IS_STYLABLE(parent)) parent=NULL;

	if(inSelector->parent)
	{
		gint					numberMatches;

		/* If node has no parent, no parent can match ;) so return immediately */
		if(!parent) return(-1);

		/* Check if there are matching parents. If not return immediately. */
		numberMatches=_xfdashboard_themes_css_score_node_matching_selector(inSelector->parent, parent);
		if(numberMatches<0) return(-1);

		/* Score matching parents */
		c+=numberMatches;
	}

	/* Check and score ancestor */
	if(inSelector->ancestor)
	{
		gint					numberMatches;
		XfdashboardStylable		*stylableParent, *ancestor;

		/* If node has no parents, no ancestor can match so return immediately */
		if(!parent) return(-1);

		/* Iterate through ancestors and check and score them */
		ancestor=parent;
		while(ancestor)
		{
			/* Get number of matches for ancestor and if at least one matches,
			 * stop search and score
			 */
			numberMatches=_xfdashboard_themes_css_score_node_matching_selector(inSelector->ancestor, ancestor);
			if(numberMatches>=0)
			{
				c+=numberMatches;
				break;
			}

			/* Get next ancestor to check */
			stylableParent=xfdashboard_stylable_get_parent(ancestor);
			if(stylableParent && !XFDASHBOARD_IS_STYLABLE(stylableParent)) stylableParent=NULL;

			ancestor=stylableParent;
			if(!ancestor || !XFDASHBOARD_IS_STYLABLE(ancestor)) return(-1);
		}
	}

	/* Calculate final score */
	score=(a*10000)+(b*100)+c;
	return(score);
}

/* Callback for sorting selector matches by score */
static gint _xfdashboard_theme_css_sort_by_score(XfdashboardThemeCSSSelectorMatch *inLeft,
													XfdashboardThemeCSSSelectorMatch *inRight)
{
	gint	priority;
	guint	line;
	guint	position;
	gint	score;

	score=inLeft->score-inRight->score;
	if(score!=0) return(score);

	priority=inLeft->selector->priority-inRight->selector->priority;
	if(priority!=0) return(priority);

	line=inLeft->selector->line-inRight->selector->line;
	if(line!=0) return(line);

	position=inLeft->selector->position-inRight->selector->position;
	if(position!=0) return(position);

	return(0);
}

/* IMPLEMENTATION: GObject */

/* Dispose this object */
static void _xfdashboard_theme_css_dispose(GObject *inObject)
{
	XfdashboardThemeCSS			*self=XFDASHBOARD_THEME_CSS(inObject);
	XfdashboardThemeCSSPrivate		*priv=self->priv;

	/* Release allocated resources */
	if(priv->selectors)
	{
		g_list_foreach(priv->selectors, (GFunc)_xfdashboard_theme_css_selector_free, NULL);
		g_list_free(priv->selectors);
		priv->selectors=NULL;
	}

	if(priv->styles)
	{
		g_list_foreach(priv->styles, (GFunc)g_hash_table_destroy, NULL);
		g_list_free(priv->styles);
		priv->styles=NULL;
	}

	if(priv->names)
	{
		g_slist_foreach(priv->names, (GFunc)g_free, NULL);
		g_slist_free(priv->names);
		priv->names=NULL;
	}

	/* Call parent's class dispose method */
	G_OBJECT_CLASS(xfdashboard_theme_css_parent_class)->dispose(inObject);
}

/* Class initialization
 * Override functions in parent classes and define properties
 * and signals
 */
void xfdashboard_theme_css_class_init(XfdashboardThemeCSSClass *klass)
{
	GObjectClass		*gobjectClass=G_OBJECT_CLASS(klass);

	/* Override functions */
	gobjectClass->dispose=_xfdashboard_theme_css_dispose;

	/* Set up private structure */
	g_type_class_add_private(klass, sizeof(XfdashboardThemeCSSPrivate));
}

/* Object initialization
 * Create private structure and set up default values
 */
void xfdashboard_theme_css_init(XfdashboardThemeCSS *self)
{
	XfdashboardThemeCSSPrivate		*priv;

	priv=self->priv=XFDASHBOARD_THEME_CSS_GET_PRIVATE(self);

	/* Set default values */
	priv->selectors=NULL;
	priv->styles=NULL;
	priv->names=NULL;
}

/* Implementation: Errors */

GQuark xfdashboard_theme_css_error_quark(void)
{
	return(g_quark_from_static_string("xfdashboard-theme-css-error-quark"));
}

/* Implementation: Public API */

/* Create new instance */
XfdashboardThemeCSS* xfdashboard_theme_css_new(void)
{
	return(XFDASHBOARD_THEME_CSS(g_object_new(XFDASHBOARD_TYPE_THEME_CSS, NULL)));
}

/* Load a CSS file into theme */
gboolean xfdashboard_theme_css_add_file(XfdashboardThemeCSS *self,
											const gchar *inPath,
											gint inPriority,
											GError **outError)
{
	XfdashboardThemeCSSPrivate		*priv;
	GFile							*file;
	GFileInputStream				*stream;
	GError							*error;

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), FALSE);
	g_return_val_if_fail(inPath!=NULL && *inPath!=0, FALSE);
	g_return_val_if_fail(outError==NULL || *outError==NULL, FALSE);

	priv=self->priv;

	/* Load and parse CSS file */
	file=g_file_new_for_path(inPath);
	if(!file)
	{
		if(outError)
		{
			g_set_error(outError,
							XFDASHBOARD_THEME_CSS_ERROR,
							XFDASHBOARD_THEME_CSS_ERROR_INVALID_ARGUMENT,
							_("Could not get file for path '%s'"),
							inPath);
		}

		return(FALSE);
	}

	error=NULL;
	stream=g_file_read(file, NULL, &error);
	if(error)
	{
		g_propagate_error(outError, error);
		return(FALSE);
	}

	_xfdashboard_theme_css_parse_css(self, G_INPUT_STREAM(stream), inPath, inPriority, &error);
	if(error)
	{
		g_propagate_error(outError, error);
		return(FALSE);
	}

	/* If we get here adding, loading and parsing CSS file was successful
	 * so add filename to list of loaded name and return TRUE
	 */
	priv->names=g_slist_prepend(priv->names, strdup(inPath));
	return(TRUE);
}

/* Return properties for a stylable actor */
GHashTable* xfdashboard_theme_css_get_properties(XfdashboardThemeCSS *self,
													XfdashboardStylable *inStylable)
{
	XfdashboardThemeCSSPrivate			*priv;
	GList								*entry, *matches;
	XfdashboardThemeCSSSelectorMatch	*match;
	GHashTable							*result;
#ifdef DEBUG
	GTimer								*timer=NULL;
	const gchar							*styleID;
	const gchar							*styleClasses;
	const gchar							*stylePseudoClasses;
	const gchar							*styleTypeName;
	gchar								*styleSelector;
#endif

	g_return_val_if_fail(XFDASHBOARD_IS_THEME_CSS(self), NULL);
	g_return_val_if_fail(XFDASHBOARD_IS_STYLABLE(inStylable), NULL);

	priv=self->priv;
	matches=NULL;
	match=NULL;

#ifdef DEBUG
	styleID=xfdashboard_stylable_get_name(inStylable);
	styleClasses=xfdashboard_stylable_get_classes(inStylable);
	stylePseudoClasses=xfdashboard_stylable_get_pseudo_classes(inStylable);
	styleTypeName=G_OBJECT_TYPE_NAME(inStylable);
	styleSelector=g_strdup_printf("%s%s%s%s%s%s%s",
									(styleTypeName) ? styleTypeName : "",
									(styleClasses) ? "." : "",
									(styleClasses) ? styleClasses : "",
									(styleID) ? "#" : "",
									(styleID) ? styleID : "",
									(stylePseudoClasses) ? ":" : "",
									(stylePseudoClasses) ? stylePseudoClasses : "");
	g_debug("Looking up matches for %s ", styleSelector);

	timer=g_timer_new();
#endif

	/* Find and collect matching selectors */
	for(entry=priv->selectors; entry; entry=g_list_next(entry))
	{
		gint							score;

		score=_xfdashboard_themes_css_score_node_matching_selector(entry->data, inStylable);
		if(score>=0)
		{
			match=g_slice_new(XfdashboardThemeCSSSelectorMatch);
			match->selector=entry->data;
			match->score=score;
			matches=g_list_prepend(matches, match);
		}
	}

	/* Sort matching selectors by their score */
	matches=g_list_sort(matches,
						(GCompareFunc)_xfdashboard_theme_css_sort_by_score);

	/* Get properties from matching selectors' styles */
	result=g_hash_table_new_full(g_str_hash,
									g_str_equal,
									NULL,
									(GDestroyNotify)_xfdashboard_theme_css_value_free);
	for(entry=matches; entry; entry=g_list_next(entry))
	{
		XfdashboardThemeCSSTableCopyData	copyData;

		/* Get selector */
		match=entry->data;

		/* Copy selector properties to result set */
		copyData.name=match->selector->name;
		copyData.table=result;

		g_hash_table_foreach(match->selector->style,
								(GHFunc)_xfdashboard_theme_css_copy_table,
								&copyData);
	}

	g_list_foreach(matches, (GFunc)_xfdashboard_themes_css_selector_match_free, NULL);
	g_list_free(matches);

#ifdef DEBUG
	g_debug("Found %u properties for %s in %f seconds" ,
				g_hash_table_size(result),
				styleSelector,
				g_timer_elapsed(timer, NULL));
	g_timer_destroy(timer);
	g_free(styleSelector);
#endif

	/* Return found properties */
	return(result);
}