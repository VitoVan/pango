/* Pango
 * pangoxft-fontmap.h: Font handling
 *
 * Copyright (C) 2000 Red Hat Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "pango-fontmap.h"
#include "pangoxft.h"
#include "pangoxft-private.h"
#include "modules.h"

#include "X11/Xft/XftFreetype.h"

/* Number of freed fonts */
#define MAX_FREED_FONTS 16

typedef struct _PangoXftFontMap      PangoXftFontMap;
typedef struct _PangoXftFamily       PangoXftFamily;
typedef struct _PangoXftFace         PangoXftFace;

#define PANGO_TYPE_XFT_FONT_MAP              (pango_xft_font_map_get_type ())
#define PANGO_XFT_FONT_MAP(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), PANGO_TYPE_XFT_FONT_MAP, PangoXftFontMap))
#define PANGO_XFT_IS_FONT_MAP(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), PANGO_TYPE_XFT_FONT_MAP))

struct _PangoXftFontMap
{
  PangoFontMap parent_instance;

  GHashTable *font_hash;
  GHashTable *coverage_hash;
  GQueue *freed_fonts;

  PangoXftFamily **families;
  int n_families;		/* -1 == uninitialized */

  Display *display;
  int screen;  
};

#define PANGO_XFT_TYPE_FAMILY              (pango_xft_family_get_type ())
#define PANGO_XFT_FAMILY(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), PANGO_XFT_TYPE_FAMILY, PangoXftFamily))
#define PANGO_XFT_IS_FAMILY(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), PANGO_XFT_TYPE_FAMILY))

struct _PangoXftFamily
{
  PangoFontFamily parent_instance;

  PangoXftFontMap *fontmap;
  char *family_name;

  PangoXftFace **faces;
  int n_faces;		/* -1 == uninitialized */
};

#define PANGO_XFT_TYPE_FACE              (pango_xft_face_get_type ())
#define PANGO_XFT_FACE(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), PANGO_XFT_TYPE_FACE, PangoXftFace))
#define PANGO_XFT_IS_FACE(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), PANGO_XFT_TYPE_FACE))

struct _PangoXftFace
{
  PangoFontFace parent_instance;

  PangoXftFamily *family;
  char *style;
};

static GType    pango_xft_font_map_get_type   (void);
GType           pango_xft_family_get_type     (void);
GType           pango_xft_face_get_type       (void);

static void       pango_xft_font_map_init          (PangoXftFontMap              *fontmap);
static void       pango_xft_font_map_class_init    (PangoFontMapClass            *class);
static void       pango_xft_font_map_finalize      (GObject                      *object);
static PangoFont *pango_xft_font_map_load_font     (PangoFontMap                 *fontmap,
						    const PangoFontDescription   *description);
static void       pango_xft_font_map_list_families (PangoFontMap                 *fontmap,
						    PangoFontFamily            ***families,
						    int                          *n_families);

static void pango_xft_font_map_cache_clear  (PangoXftFontMap *xfontmap);
static void pango_xft_font_map_cache_remove (PangoFontMap    *fontmap,
					     PangoXftFont    *xfont);

static PangoFontClass *parent_class;	/* Parent class structure for PangoXftFontMap */

static GType
pango_xft_font_map_get_type (void)
{
  static GType object_type = 0;

  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (PangoFontMapClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) pango_xft_font_map_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (PangoXftFontMap),
        0,              /* n_preallocs */
        (GInstanceInitFunc) pango_xft_font_map_init,
      };
      
      object_type = g_type_register_static (PANGO_TYPE_FONT_MAP,
                                            "PangoXftFontMap",
                                            &object_info, 0);
    }
  
  return object_type;
}

static void 
pango_xft_font_map_init (PangoXftFontMap *xfontmap)
{
  xfontmap->n_families = -1;
}

static void
pango_xft_font_map_class_init (PangoFontMapClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  
  parent_class = g_type_class_peek_parent (class);
  
  object_class->finalize = pango_xft_font_map_finalize;
  class->load_font = pango_xft_font_map_load_font;
  class->list_families = pango_xft_font_map_list_families;
}

static GSList *fontmaps = NULL;

static PangoFontMap *
pango_xft_get_font_map (Display *display,
			int      screen)
{
  PangoXftFontMap *xfontmap;
  GSList *tmp_list = fontmaps;

  g_return_val_if_fail (display != NULL, NULL);
  
  /* Make sure that the type system is initialized */
  g_type_init ();
  
  while (tmp_list)
    {
      xfontmap = tmp_list->data;

      if (xfontmap->display == display &&
	  xfontmap->screen == screen) 
	return PANGO_FONT_MAP (xfontmap);
    }

  xfontmap = (PangoXftFontMap *)g_object_new (PANGO_TYPE_XFT_FONT_MAP, NULL);
  
  xfontmap->display = display;
  xfontmap->screen = screen;

  xfontmap->font_hash = g_hash_table_new ((GHashFunc)pango_font_description_hash,
					  (GEqualFunc)pango_font_description_equal);
  xfontmap->coverage_hash = g_hash_table_new (g_str_hash, g_str_equal);
  xfontmap->freed_fonts = g_queue_new ();

  fontmaps = g_slist_prepend (fontmaps, xfontmap);

  return PANGO_FONT_MAP (xfontmap);
}

/**
 * pango_xft_get_context:
 * @display: an X display.
 * @screen: an X screen.
 *
 * Retrieves a #PangoContext appropriate for rendering with
 * Xft fonts on the given screen of the given display. 
 *
 * Return value: the new #PangoContext. 
 **/
PangoContext *
pango_xft_get_context (Display *display,
		       int      screen)
{
  PangoContext *result;
  int i;
  static gboolean registered_modules = FALSE;

  g_return_val_if_fail (display != NULL, NULL);

  if (!registered_modules)
    {
      registered_modules = TRUE;
      
      for (i = 0; _pango_included_xft_modules[i].list; i++)
        pango_module_register (&_pango_included_xft_modules[i]);
    }
  
  result = pango_context_new ();
  pango_context_add_font_map (result, pango_xft_get_font_map (display, screen));

  return result;
}

static void
coverage_foreach (gpointer key, gpointer value, gpointer data)
{
  PangoCoverage *coverage = value;

  g_free (key);
  pango_coverage_unref (coverage);
}

static void
pango_xft_font_map_finalize (GObject *object)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (object);

  fontmaps = g_slist_remove (fontmaps, object);

  g_queue_free (xfontmap->freed_fonts);
  g_hash_table_destroy (xfontmap->font_hash);

  g_hash_table_foreach (xfontmap->coverage_hash, coverage_foreach, NULL);
  g_hash_table_destroy (xfontmap->coverage_hash);
  
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

void
_pango_xft_font_map_add (PangoFontMap *fontmap,
			PangoXftFont *xfont)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  g_hash_table_insert (xfontmap->font_hash, xfont->description, xfont);
}

void
_pango_xft_font_map_remove (PangoFontMap *fontmap,
			   PangoXftFont *xfont)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  g_hash_table_remove (xfontmap->font_hash, xfont->description);
}

static void
pango_xft_font_map_list_families (PangoFontMap           *fontmap,
				  PangoFontFamily      ***families,
				  int                    *n_families)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);
  XftFontSet *fontset;
  int i;

  if (xfontmap->n_families < 0)
    {
      fontset = XftListFonts (xfontmap->display, xfontmap->screen,
			      XFT_CORE, XftTypeBool, False,
			      XFT_ENCODING, XftTypeString, "iso10646-1",
			      NULL,
			      XFT_FAMILY,
			      NULL);

      xfontmap->n_families = fontset->nfont;
      xfontmap->families = g_new (PangoXftFamily *, xfontmap->n_families);

      for (i = 0; i < fontset->nfont; i++)
	{
	  char *s;
	  XftResult res;
	  
	  res = XftPatternGetString (fontset->fonts[i], XFT_FAMILY, 0, &s);
	  g_assert (res == XftResultMatch);
	  
	  xfontmap->families[i] = g_object_new (PANGO_XFT_TYPE_FAMILY, NULL);
	  xfontmap->families[i]->family_name = g_strdup (s);
	  xfontmap->families[i]->fontmap = xfontmap;
	}

      XftFontSetDestroy (fontset);
    }
  
  if (n_families)
    *n_families = xfontmap->n_families;
  
  if (families)
    *families = g_memdup (xfontmap->families, xfontmap->n_families * sizeof (PangoFontFamily *));
}

static PangoFont *
pango_xft_font_map_load_font (PangoFontMap               *fontmap,
			      const PangoFontDescription *description)
{
  PangoXftFontMap *xfontmap = (PangoXftFontMap *)fontmap;
  PangoXftFont *font;
  PangoStyle pango_style;
  int slant;
  PangoWeight pango_weight;
  int weight;
  XftFont *xft_font;

  font = g_hash_table_lookup (xfontmap->font_hash, description);

  if (font)
    {
      if (font->in_cache)
	pango_xft_font_map_cache_remove (fontmap, font);
      
      return (PangoFont *)g_object_ref (G_OBJECT (font));
    }

  pango_style = pango_font_description_get_style (description);
  
  if (pango_style == PANGO_STYLE_ITALIC)
    slant = XFT_SLANT_ITALIC;
  else if (pango_style == PANGO_STYLE_OBLIQUE)
    slant = XFT_SLANT_OBLIQUE;
  else
    slant = XFT_SLANT_ROMAN;

  pango_weight = pango_font_description_get_weight (description);
  
  if (pango_weight < (PANGO_WEIGHT_NORMAL + PANGO_WEIGHT_LIGHT) / 2)
    weight = XFT_WEIGHT_LIGHT;
  else if (pango_weight < (PANGO_WEIGHT_NORMAL + 600) / 2)
    weight = XFT_WEIGHT_MEDIUM;
  else if (pango_weight < (600 + PANGO_WEIGHT_BOLD) / 2)
    weight = XFT_WEIGHT_DEMIBOLD;
  else if (pango_weight < (PANGO_WEIGHT_BOLD + PANGO_WEIGHT_ULTRABOLD) / 2)
    weight = XFT_WEIGHT_BOLD;
  else
    weight = XFT_WEIGHT_BLACK;

  /* To fool Xft into not munging glyph indices, we open it as glyphs-fontspecific
   * then set the encoding ourself
   */
  xft_font = XftFontOpen (xfontmap->display, xfontmap->screen,
			  XFT_ENCODING, XftTypeString, "glyphs-fontspecific",
			  XFT_CORE, XftTypeBool, False,
			  XFT_FAMILY, XftTypeString,  pango_font_description_get_family (description),
			  XFT_WEIGHT, XftTypeInteger, weight,
			  XFT_SLANT,  XftTypeInteger, slant,
			  XFT_SIZE, XftTypeDouble, (double)pango_font_description_get_size (description)/PANGO_SCALE,
			  NULL);

  if (xft_font)
    {
      FT_Face face;
      FT_Error error;
      
      int charmap;

      g_assert (!xft_font->core);
      
      face = xft_font->u.ft.font->face;

      for (charmap = 0; charmap < face->num_charmaps; charmap++)
	if (face->charmaps[charmap]->encoding == ft_encoding_unicode)
	  break;

      if (charmap == face->num_charmaps)
	goto error;

      error = FT_Set_Charmap(face, face->charmaps[charmap]);
      
      if (error)
	goto error;
      
      font = _pango_xft_font_new (fontmap, description, xft_font);
    }
  else
    return NULL;

  return (PangoFont *)font;

 error:

  XftFontClose (xfontmap->display, xft_font);
  return NULL;
}

void
_pango_xft_font_map_cache_add (PangoFontMap *fontmap,
			       PangoXftFont *xfont)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  if (xfontmap->freed_fonts->length == MAX_FREED_FONTS)
    {
      GObject *old_font = g_queue_pop_tail (xfontmap->freed_fonts);
      g_object_unref (old_font);
    }

  g_object_ref (G_OBJECT (xfont));
  g_queue_push_head (xfontmap->freed_fonts, xfont);
  xfont->in_cache = TRUE;
}

static void
pango_xft_font_map_cache_remove (PangoFontMap *fontmap,
				 PangoXftFont *xfont)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  GList *link = g_list_find (xfontmap->freed_fonts->head, xfont);
  if (link == xfontmap->freed_fonts->tail)
    {
      xfontmap->freed_fonts->tail = xfontmap->freed_fonts->tail->prev;
      if (xfontmap->freed_fonts->tail)
	xfontmap->freed_fonts->tail->next = NULL;
    }
  
  xfontmap->freed_fonts->head = g_list_delete_link (xfontmap->freed_fonts->head, link);
  xfontmap->freed_fonts->length--;
  xfont->in_cache = FALSE;

  g_object_unref (G_OBJECT (xfont));
}

static void
pango_xft_font_map_cache_clear (PangoXftFontMap   *xfontmap)
{
  g_list_foreach (xfontmap->freed_fonts->head, (GFunc)g_object_unref, NULL);
  g_list_free (xfontmap->freed_fonts->head);
  xfontmap->freed_fonts->head = NULL;
  xfontmap->freed_fonts->tail = NULL;
  xfontmap->freed_fonts->length = 0;
}

void
_pango_xft_font_map_set_coverage (PangoFontMap  *fontmap,
				  const char    *name,
				  PangoCoverage *coverage)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  g_hash_table_insert (xfontmap->coverage_hash, g_strdup (name),
		       pango_coverage_ref (coverage));
}

PangoCoverage *
_pango_xft_font_map_get_coverage (PangoFontMap  *fontmap,
				  const char    *name)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  return g_hash_table_lookup (xfontmap->coverage_hash, name);
}

void
_pango_xft_font_map_get_info (PangoFontMap *fontmap,
			      Display     **display,
			      int          *screen)
{
  PangoXftFontMap *xfontmap = PANGO_XFT_FONT_MAP (fontmap);

  if (display)
    *display = xfontmap->display;
  if (screen)
    *screen = xfontmap->screen;

}


/* 
 * PangoXftFace
 */

static PangoFontDescription *
font_desc_from_pattern (XftPattern *pattern)
{
  PangoFontDescription *desc;
  PangoStyle style;
  PangoWeight weight;
  
  char *s;
  int i;

  desc = pango_font_description_new ();

  g_assert (XftPatternGetString (pattern, XFT_FAMILY, 0, &s) == XftResultMatch);

  pango_font_description_set_family (desc, s);
  
  if (XftPatternGetInteger (pattern, XFT_SLANT, 0, &i) == XftResultMatch)
    {
      if (i == XFT_SLANT_ROMAN)
	style = PANGO_STYLE_NORMAL;
      else if (i == XFT_SLANT_OBLIQUE)
	style = PANGO_STYLE_OBLIQUE;
      else
	style = PANGO_STYLE_ITALIC;
    }
  else
    style = PANGO_STYLE_NORMAL;

  pango_font_description_set_style (desc, style);

  if (XftPatternGetInteger (pattern, XFT_WEIGHT, 0, &i) == XftResultMatch)
    { 
     if (i < XFT_WEIGHT_LIGHT)
	weight = PANGO_WEIGHT_ULTRALIGHT;
      else if (i < (XFT_WEIGHT_LIGHT + XFT_WEIGHT_MEDIUM) / 2)
	weight = PANGO_WEIGHT_LIGHT;
      else if (i < (XFT_WEIGHT_MEDIUM + XFT_WEIGHT_DEMIBOLD) / 2)
	weight = PANGO_WEIGHT_NORMAL;
      else if (i < (XFT_WEIGHT_DEMIBOLD + XFT_WEIGHT_BOLD) / 2)
	weight = 600;
      else if (i < (XFT_WEIGHT_BOLD + XFT_WEIGHT_BLACK) / 2)
	weight = PANGO_WEIGHT_BOLD;
      else
	weight = PANGO_WEIGHT_ULTRABOLD;
    }
  else
    weight = PANGO_WEIGHT_NORMAL;

  pango_font_description_set_weight (desc, weight);
  
  pango_font_description_set_variant (desc, PANGO_VARIANT_NORMAL);
  pango_font_description_set_stretch (desc, PANGO_STRETCH_NORMAL);

  return desc;
}

static PangoFontDescription *
pango_xft_face_describe (PangoFontFace *face)
{
  PangoXftFace *xface = PANGO_XFT_FACE (face);
  PangoXftFamily *xfamily = xface->family;
  PangoXftFontMap *xfontmap = xfamily->fontmap;
  PangoFontDescription *desc = NULL;
  XftResult res;
  XftPattern *match_pattern;
  XftPattern *result_pattern;

  match_pattern = XftPatternBuild (NULL,
				   XFT_ENCODING, XftTypeString, "iso10646-1",
				   XFT_FAMILY, XftTypeString, xfamily->family_name,
				   XFT_CORE, XftTypeBool, False,
				   XFT_STYLE, XftTypeString, xface->style,
				   NULL);
  g_assert (match_pattern);
  
  result_pattern = XftFontMatch (xfontmap->display, xfontmap->screen, match_pattern, &res);
  if (result_pattern)
    {
      desc = font_desc_from_pattern (result_pattern);
      XftPatternDestroy (result_pattern);
    }

  XftPatternDestroy (match_pattern);
  
  return desc;
}

static const char *
pango_xft_face_get_face_name (PangoFontFace *face)
{
  PangoXftFace *xface = PANGO_XFT_FACE (face);

  return xface->style;
}

static void
pango_xft_face_class_init (PangoFontFaceClass *class)
{
  class->describe = pango_xft_face_describe;
  class->get_face_name = pango_xft_face_get_face_name;
}

GType
pango_xft_face_get_type (void)
{
  static GType object_type = 0;

  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (PangoFontFaceClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) pango_xft_face_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (PangoXftFace),
        0,              /* n_preallocs */
        (GInstanceInitFunc) NULL,
      };
      
      object_type = g_type_register_static (PANGO_TYPE_FONT_FACE,
                                            "PangoXftFace",
                                            &object_info, 0);
    }
  
  return object_type;
}

/*
 * PangoXFontFamily
 */
static void
pango_xft_family_list_faces (PangoFontFamily  *family,
			     PangoFontFace  ***faces,
			     int              *n_faces)
{
  PangoXftFamily *xfamily = PANGO_XFT_FAMILY (family);
  PangoXftFontMap *xfontmap = xfamily->fontmap;

  if (xfamily->n_faces < 0)
    {
      XftFontSet *fontset;
      int i;
      
      fontset = XftListFonts (xfontmap->display, xfontmap->screen,
			      XFT_ENCODING, XftTypeString, "iso10646-1",
			      XFT_FAMILY, XftTypeString, xfamily->family_name,
			      XFT_CORE, XftTypeBool, False,
			      NULL,
			      XFT_STYLE,
			      NULL);
      
      xfamily->n_faces = fontset->nfont;
      xfamily->faces = g_new (PangoXftFace *, xfamily->n_faces);

      for (i = 0; i < fontset->nfont; i++)
	{
	  char *s;
	  XftResult res;
	  
	  res = XftPatternGetString (fontset->fonts[i], XFT_STYLE, 0, &s);
	  g_assert (res == XftResultMatch);
	  
	  xfamily->faces[i] = g_object_new (PANGO_XFT_TYPE_FACE, NULL);
	  xfamily->faces[i]->style = g_strdup (s);
	  xfamily->faces[i]->family = xfamily;
	}
      
      XftFontSetDestroy (fontset);
    }
  
  if (n_faces)
    *n_faces = xfamily->n_faces;
  
  if (faces)
    *faces = g_memdup (xfamily->faces, xfamily->n_faces * sizeof (PangoFontFace *));
}

const char *
pango_xft_family_get_name (PangoFontFamily  *family)
{
  PangoXftFamily *xfamily = PANGO_XFT_FAMILY (family);

  return xfamily->family_name;
}

static void
pango_xft_family_class_init (PangoFontFamilyClass *class)
{
  class->list_faces = pango_xft_family_list_faces;
  class->get_name = pango_xft_family_get_name;
}

void
pango_xft_family_init (PangoXftFamily *xfamily)
{
  xfamily->n_faces = -1;
}

GType
pango_xft_family_get_type (void)
{
  static GType object_type = 0;

  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (PangoFontFamilyClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) pango_xft_family_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (PangoXftFamily),
        0,              /* n_preallocs */
        (GInstanceInitFunc) pango_xft_family_init,
      };
      
      object_type = g_type_register_static (PANGO_TYPE_FONT_FAMILY,
                                            "PangoXftFamily",
                                            &object_info, 0);
    }
  
  return object_type;
}
