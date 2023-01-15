/*
 * Copyright © 2006 Novell, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Novell, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior permission.
 * Novell, Inc. makes no representations about the suitability of this
 * software for any purpose. It is provided "as is" without express or
 * implied warranty.
 *
 * NOVELL, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL NOVELL, INC. BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: eignar samaniego <eignar17@gmail.com>
 */

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <compiz-core.h>

#define TEXTURE_SIZE 256

#define K 0.1964f

#define TEXTURE_NUM 3

typedef struct _frostFunction {
    struct _frostFunction *next;

    int handle;
    int target;
    int param;
    int unit;
} frostFunction;

#define TINDEX(fs, i) (((fs)->tIndex + (i)) % TEXTURE_NUM)

#define CLAMP(v, min, max) \
    if ((v) > (max))	   \
	(v) = (max);	   \
    else if ((v) < (min))  \
	(v) = (min)

#define FROST_INITIATE_MODIFIERS_DEFAULT (ControlMask | CompSuperMask)

static CompMetadata frostMetadata;

static int displayPrivateIndex;

static int frostLastPointerX = 0;
static int frostLastPointerY = 0;

#define FROST_DISPLAY_OPTION_INITIATE_KEY     0
#define FROST_DISPLAY_OPTION_TOGGLE_RAIN_KEY  1
#define FROST_DISPLAY_OPTION_TOGGLE_WIPER_KEY 2
#define FROST_DISPLAY_OPTION_OFFSET_SCALE     3
#define FROST_DISPLAY_OPTION_RAIN_DELAY	      4
#define FROST_DISPLAY_OPTION_TITLE_WAVE       5
#define FROST_DISPLAY_OPTION_POINT            6
#define FROST_DISPLAY_OPTION_LINE             7
#define FROST_DISPLAY_OPTION_NUM              8

typedef struct _frostDisplay {
    int		    screenPrivateIndex;

    CompOption opt[FROST_DISPLAY_OPTION_NUM];

    HandleEventProc handleEvent;

    float offsetScale;
} frostDisplay;

typedef struct _frostScreen {
    PreparePaintScreenProc preparePaintScreen;
    DonePaintScreenProc    donePaintScreen;
    DrawWindowTextureProc  drawWindowTexture;

    int grabIndex;
    int width, height;

    GLuint program;
    GLuint texture[TEXTURE_NUM];

    int     tIndex;
    GLenum  target;
    GLfloat tx, ty;

    int count;

    GLuint fbo;
    GLint  fboStatus;

    void	  *data;
    float	  *d0;
    float	  *d1;
    unsigned char *t0;

    CompTimeoutHandle rainHandle;
    CompTimeoutHandle wiperHandle;

    float wiperAngle;
    float wiperSpeed;

    frostFunction *bumpMapFunctions;
} frostScreen;

#define GET_FROST_DISPLAY(d)					   \
    ((frostDisplay *) (d)->base.privates[displayPrivateIndex].ptr)

#define FROST_DISPLAY(d)		     \
    frostDisplay *fd = GET_FROST_DISPLAY (d)

#define GET_FROST_SCREEN(s, fd)					       \
    ((frostScreen *) (s)->base.privates[(fd)->screenPrivateIndex].ptr)

#define FROST_SCREEN(s)							   \
    frostScreen *fs = GET_FROST_SCREEN (s, GET_FROST_DISPLAY (s->display))

#define NUM_OPTIONS(s) (sizeof ((s)->opt) / sizeof (CompOption))

static Bool
frostRainTimeout (void *closure);

static Bool
frostWiperTimeout (void *closure);

static const char *frostFpString =
    "!!ARBfp1.0"

    "PARAM param = program.local[0];"
    "ATTRIB t11  = fragment.texcoord[0];"

    "TEMP t01, t21, t10, t12;"
    "TEMP c11, c01, c21, c10, c12;"
    "TEMP prev, v, temp, accel;"

    "TEX prev, t11, texture[0], %s;"
    "TEX c11,  t11, texture[1], %s;"

    /* sample offsets */
    "ADD t01, t11, { - %f, 0.0, 0.0, 0.0 };"
    "ADD t21, t11, {   %f, 0.0, 0.0, 0.0 };"
    "ADD t10, t11, { 0.0, - %f, 0.0, 0.0 };"
    "ADD t12, t11, { 0.0,   %f, 0.0, 0.0 };"

    /* fetch nesseccary samples */
    "TEX c01, t01, texture[1], %s;"
    "TEX c21, t21, texture[1], %s;"
    "TEX c10, t10, texture[1], %s;"
    "TEX c12, t12, texture[1], %s;"

    /* x/y normals from frost */
    "MOV t01.rg, t01.ba; MOV t21.rg, t21.ba; MOV t10.rg, t10.ba; MOV t12.rg, t12.ba;"

    /* frostiness */
    "MUL t01.rg, t01.rg, { frostiness, frostiness, 0.0, 0.0 };"

    /* frostize */
    "DP3 t01.a, t01.rg, t01.rg; RSQ t01.a, t01.a; MUL t01.rg, t01.rg, t01.a;"

    /* add scale and bias to frost normal */
    "ADD t01.rg, t01.rg, { scale, bias, 0.0, 0.0 };"

    /* done with computing the frost normal, continue with computing the next frost value */
    "MOV t02, t01; ADD t02.rg, t02.rg, { 1.0, 1.0, 0.0, 0.0 };"

    /* store new frost in alpha component */
    "ALPHA_TO_COLOR t01.a, t01.a; STORE t01.a, { t01.r, t01.g, t01.b, t01.a };"

    /* fade out frost */
    "MUL t01.a, t01.a, { fade, fade, fade, fade };"

    "MOV result.color, t01;"

    "END";

static int
loadFragmentProgram (CompScreen *s,
		     GLuint	*program,
		     const char *string)
{
    GLint errorPos;

    /* clear errors */
    glGetError ();

    if (!*program)
	(*s->genPrograms) (1, program);

    (*s->bindProgram) (GL_FRAGMENT_PROGRAM_ARB, *program);
    (*s->programString) (GL_FRAGMENT_PROGRAM_ARB,
			 GL_PROGRAM_FORMAT_ASCII_ARB,
			 strlen (string), string);

    glGetIntegerv (GL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
    if (glGetError () != GL_NO_ERROR || errorPos != -1)
    {
	compLogMessage ("frost", CompLogLevelError,
			"failed to load bump map program");

	(*s->deletePrograms) (1, program);
	*program = 0;

	return 0;
    }

    return 1;
}

static int
loadfrostProgram (CompScreen *s)
{
    char buffer[1024];

    FROST_SCREEN (s);

    if (fs->target == GL_TEXTURE_2D)
	sprintf (buffer, frostFpString,
		 "2D", "2D",
		 1.0f / fs->width,  1.0f / fs->width,
		 1.0f / fs->height, 1.0f / fs->height,
		 "2D", "2D", "2D", "2D");
    else
	sprintf (buffer, frostFpString,
		 "RECT", "RECT",
		 1.0f, 1.0f, 1.0f, 1.0f,
		 "RECT", "RECT", "RECT", "RECT");

    return loadFragmentProgram (s, &fs->program, buffer);
}

static int
getBumpMapFragmentFunction (CompScreen  *s,
			    CompTexture *texture,
			    int		unit,
			    int		param)
{
    frostFunction    *function;
    CompFunctionData *data;
    int		     target;

    FROST_SCREEN (s);

    if (texture->target == GL_TEXTURE_2D)
	target = COMP_FETCH_TARGET_2D;
    else
	target = COMP_FETCH_TARGET_RECT;

    for (function = fs->bumpMapFunctions; function; function = function->next)
    {
	if (function->param  == param &&
	    function->unit   == unit  &&
	    function->target == target)
	    return function->handle;
    }

    data = createFunctionData ();
    if (data)
    {
	static char *temp[] = { "normal", "temp", "total", "bump", "offset" };
	int	    i, handle = 0;
	char	    str[1024];

	for (i = 0; i < sizeof (temp) / sizeof (temp[0]); i++)
	{
	    if (!addTempHeaderOpToFunctionData (data, temp[i]))
	    {
		destroyFunctionData (data);
		return 0;
	    }
	}

	snprintf (str, 1024,

		  /* get frost normal from frost normal map */
		  "TEX t01, vTexCoord, texture[1], 2D;"

		  /* save frost */
		  "MOV t02, t01;"

		  /* remove scale and bias from frost normal */
		  "MUL t01, t01, { scale, scale, scale, scale };"
		  
		  /*normalize the frost normal map */
		  "DP3 t01, t01, t01;"
		  "RSQ t01.w, t01.w;"
		  "MUL t01, t01, t01.w;"

		  /* scale down frost normal by frost height and constant and use as offset in frost texture */
		  "MUL t01, t01, { frostHeight, frostHeight, frostHeight, frostHeight };"
		  "ADD t02, t02, t01;",

		  unit, unit,
		  (fs->target == GL_TEXTURE_2D) ? "2D" : "RECT",
		  param);

	if (!addDataOpToFunctionData (data, str))
	{
	    destroyFunctionData (data);
	    return 0;
	}

	if (!addFetchOpToFunctionData (data, "output", "offset.yxzz", target))
	{
	    destroyFunctionData (data);
	    return 0;
	}

	snprintf (str, 1024,

		  /* frost normal dot frost lightdir, this should eventually be changed to a real frost light vector */
		  "DP3 t01, t01, frostLightDir;"

	if (!addDataOpToFunctionData (data, str))
	{
	    destroyFunctionData (data);
	    return 0;
	}

	if (!addColorOpToFunctionData (data, "output", "output"))
	{
	    destroyFunctionData (data);
	    return 0;
	}

	snprintf (str, 1024,

		  /* diffuse per-vertex frost lighting, frost opacity and frost brightness and add frost lightsource bump color */
		  "MUL t01, frostOpacity, frostBrightness;"
		  "MAD t01, t01, frostLightSourceBumpColor, t02;"
		  "MUL result.color, t01, t02;"

	if (!addDataOpToFunctionData (data, str))
	{
	    destroyFunctionData (data);
	    return 0;
	}

	function = malloc (sizeof (frostFunction));
	if (function)
	{
	    handle = createFragmentFunction (s, "frost", data);

	    function->handle = handle;
	    function->target = target;
	    function->param  = param;
	    function->unit   = unit;

	    function->next = fs->bumpMapFunctions;
	    fs->bumpMapFunctions = function;
	}

	destroyFunctionData (data);

	return handle;
    }

    return 0;
}

static void
allocTexture (CompScreen *s,
	      int	 index)
{
    FROST_SCREEN (s);

    glGenTextures (1, &fs->texture[index]);
    glBindTexture (fs->target, fs->texture[index]);

    glTexParameteri (fs->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (fs->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (fs->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (fs->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D (fs->target,
		  0,
		  GL_RGBA,
		  fs->width,
		  fs->height,
		  0,
		  GL_BGRA,

#if IMAGE_BYTE_ORDER == MSBFirst
		  GL_UNSIGNED_INT_8_8_8_8_REV,
#else
		  GL_UNSIGNED_BYTE,
#endif

		  fs->t0);

    glBindTexture (fs->target, 0);
}

static int
fboPrologue (CompScreen *s,
	     int	tIndex)
{
    FROST_SCREEN (s);

    if (!fs->fbo)
	return 0;

    if (!fs->texture[tIndex])
	allocTexture (s, tIndex);

    (*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, fs->fbo);

    (*s->framebufferTexture2D) (GL_FRAMEBUFFER_EXT,
				GL_COLOR_ATTACHMENT0_EXT,
				fs->target, fs->texture[tIndex],
				0);

    glDrawBuffer (GL_COLOR_ATTACHMENT0_EXT);
    glReadBuffer (GL_COLOR_ATTACHMENT0_EXT);

    /* check status the first time */
    if (!fs->fboStatus)
    {
	fs->fboStatus = (*s->checkFramebufferStatus) (GL_FRAMEBUFFER_EXT);
	if (fs->fboStatus != GL_FRAMEBUFFER_COMPLETE_EXT)
	{
	    compLogMessage ("frost", CompLogLevelError,
			    "framebuffer incomplete");

	    (*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, 0);
	    (*s->deleteFramebuffers) (1, &fs->fbo);

	    glDrawBuffer (GL_BACK);
	    glReadBuffer (GL_BACK);

	    fs->fbo = 0;

	    return 0;
	}
    }

    glViewport (0, 0, fs->width, fs->height);
    glMatrixMode (GL_PROJECTION);
    glPushMatrix ();
    glLoadIdentity ();
    glOrtho (0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode (GL_MODELVIEW);
    glPushMatrix ();
    glLoadIdentity ();

    return 1;
}

static void
fboEpilogue (CompScreen *s)
{
    (*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, 0);

    glMatrixMode (GL_PROJECTION);
    glLoadIdentity ();
    glMatrixMode (GL_MODELVIEW);
    glLoadIdentity ();
    glDepthRange (0, 1);
    glViewport (-1, -1, 2, 2);
    glRasterPos2f (0, 0);

    s->rasterX = s->rasterY = 0;

    setDefaultViewport (s);

    glMatrixMode (GL_PROJECTION);
    glPopMatrix ();
    glMatrixMode (GL_MODELVIEW);
    glPopMatrix ();

    glDrawBuffer (GL_BACK);
    glReadBuffer (GL_BACK);
}

static int
fboUpdate (CompScreen *s,
	   float      dt,
	   float      fade)
{
    FROST_SCREEN (s);

    if (!fboPrologue (s, TINDEX (fs, 1)))
	return 0;

    if (!fs->texture[TINDEX (fs, 2)])
	allocTexture (s, TINDEX (fs, 2));

    if (!fs->texture[TINDEX (fs, 0)])
	allocTexture (s, TINDEX (fs, 0));

    glEnable (fs->target);

    (*s->activeTexture) (GL_TEXTURE0_ARB);
    glBindTexture (fs->target, fs->texture[TINDEX (fs, 2)]);

    glTexParameteri (fs->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (fs->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    (*s->activeTexture) (GL_TEXTURE1_ARB);
    glBindTexture (fs->target, fs->texture[TINDEX (fs, 0)]);
    glTexParameteri (fs->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (fs->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable (GL_FRAGMENT_PROGRAM_ARB);
    (*s->bindProgram) (GL_FRAGMENT_PROGRAM_ARB, fs->program);

    (*s->programLocalParameter4f) (GL_FRAGMENT_PROGRAM_ARB, 0,
				   dt * K, fade, 1.0f, 1.0f);

    glBegin (GL_QUADS);

    glTexCoord2f (0.0f, 0.0f);
    glVertex2f   (-1.0f, -1.0f);
    glTexCoord2f (fs->tx, 0.0f);
    glVertex2f   (1.0f, -1.0f);
    glTexCoord2f (fs->tx, fs->ty);
    glVertex2f   (1.0f, 1.0f);
    glTexCoord2f (0.0f, fs->ty);
    glVertex2f   (-1.0f, 1.0f);

    glEnd ();

    glDisable (GL_FRAGMENT_PROGRAM_ARB);

    glTexParameteri (fs->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (fs->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture (fs->target, 0);
    (*s->activeTexture) (GL_TEXTURE0_ARB);
    glTexParameteri (fs->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (fs->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture (fs->target, 0);

    glDisable (fs->target);

    fboEpilogue (s);

    /* increment texture index */
    fs->tIndex = TINDEX (fs, 1);

    return 1;
}

static int
fboVertices (CompScreen *s,
	     GLenum     type,
	     XPoint     *p,
	     int	n,
	     float	v)
{
    FROST_SCREEN (s);

    if (!fboPrologue (s, TINDEX (fs, 0)))
	return 0;

    glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glColor4f (1.0f, 1.0f, 1.0f, v);

    glPointSize (3.0f);
    glLineWidth (1.0f);

    glScalef (1.0f / fs->width, 1.0f / fs->height, 1.0);
    glTranslatef (0.5f, 0.5f, 0.0f);

    glBegin (type);

    while (n--)
    {
	glVertex2i (p->x, p->y);
	p++;
    }

    glEnd ();

    glColor4usv (defaultColor);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    fboEpilogue (s);

    return 1;
}

static void
softwareUpdate (CompScreen *s,
		float      dt,
		float      fade)
{
    float	   *dTmp;
    int		   i, j;
    float	   v0, v1, inv;
    float	   accel, value;
    unsigned char *t0, *t;
    int		  dWidth, dHeight;
    float	  *d01, *d10, *d11, *d12;

    FROST_SCREEN (s);

    if (!fs->texture[TINDEX (fs, 0)])
	allocTexture (s, TINDEX (fs, 0));

    dt *= K * 2.0f;
    fade *= 0.99f;

    dWidth = fs->width + 2;
    dHeight = fs->height + 2;

#define D(d, j) (*((d) + (j)))

    d01 = ->d0 + dWidth;
    d10 = fs->d1;
    d11 = d10 + dWidth;
    d12 = d11 + dWidth;

    for (i = 1; i < dHeight - 1; i++)
    {
	for (j = 1; j < dWidth - 1; j++)
	{
	    accel = dt * (D (d10, j)     +
			  D (d12, j)     +
			  D (d11, j - 1) +
			  D (d11, j + 1) - 4.0f * D (d11, j));

	    value = (2.0f * D (d11, j) - D (d01, j) + accel) * fade;

	    CLAMP (value, -1.0f, 1.0f);

	    D (d01, j) = value;
	}

	d01 += dWidth;
	d10 += dWidth;
	d11 += dWidth;
	d12 += dWidth;
    }

    /* update border */
    memcpy (fs->d0, fs->d0 + dWidth, dWidth * sizeof (GLfloat));
    memcpy (fs->d0 + dWidth * (dHeight - 1),
	    fs->d0 + dWidth * (dHeight - 2),
	    dWidth * sizeof (GLfloat));

    d01 = fs->d0 + dWidth;

    for (i = 1; i < dHeight - 1; i++)
    {
	D (d01, 0)	    = D (d01, 1);
	D (d01, dWidth - 1) = D (d01, dWidth - 2);

	d01 += dWidth;

    }

    d01 = fs->d0 + dWidth;
    d10 = fs->d1;
    d11 = d10 + dWidth;
    d12 = d11 + dWidth;

    t0 = fs->t0;

    /* update texture */
    for (i = 0; i < fs->height; i++)
    {
	for (j = 0; j < fs->width; j++)
	{
	d01 += dWidth;
	d10 += dWidth;
	d11 += dWidth;
	d12 += dWidth;
    }
	   
	    v0 = (D (d12, j)     - D (d10, j))     * 1.5f;
	    v1 = (D (d11, j - 1) - D (d11, j + 1)) * 1.5f;

	    /* 0.5 for scale */
	    inv = 0.5f / sqrtf (v0 * v0 + v1 * v1 + 1.0f);

	    /* add scale and bias to normal */
	    v0 = v0 * inv + 0.5f;
	    v1 = v1 * inv + 0.5f;

	    /* store normal map in RGB components */
	    t = t0 + (j * 4);
	    t[0] = (unsigned char) ((inv + 0.5f) * 255.0f);
	    t[1] = (unsigned char) (v1 * 255.0f);
	    t[2] = (unsigned char) (v0 * 255.0f);

	    /* store height in A component */
	    t[3] = (unsigned char) (D (d11, j) * 255.0f);
	}

	d10 += dWidth;
	d11 += dWidth;
	d12 += dWidth;

	t0 += fs->width * 4;
    }

#undef D

    /* swap height maps */
    dTmp   = fs->d0;
    fs->d0 = fs->d1;
    fs->d1 = dTmp;

    if (fs->texture[TINDEX (fs, 0)])
    {
	glBindTexture (fs->target, fs->texture[TINDEX (fs, 0)]);
	glTexImage2D (fs->target,
		      0,
		      GL_RGBA,
		      fs->width,
		      fs->height,
		      0,
		      GL_BGRA,

#if IMAGE_BYTE_ORDER == MSBFirst
		  GL_UNSIGNED_INT_8_8_8_8_REV,
#else
		  GL_UNSIGNED_BYTE,
#endif

		      fs->t0);
    }
}


#define SET(x, y, v) *((fs->d1) + (fs->width + 2) * (y + 1) + (x + 1)) = (v)

static void
softwarePoints (CompScreen *s,
		XPoint	   *p,
		int	   n,
		float	   add)
{
    FROST_SCREEN (s);

    while (n--)
    {
	SET (p->x - 1, p->y - 1, add);
	SET (p->x, p->y - 1, add);
	SET (p->x + 1, p->y - 1, add);

	SET (p->x - 1, p->y, add);
	SET (p->x, p->y, add);
	SET (p->x + 1, p->y, add);

	SET (p->x - 1, p->y + 1, add);
	SET (p->x, p->y + 1, add);
	SET (p->x + 1, p->y + 1, add);

	p++;
    }
}

/* bresenham */
static void
softwareLines (CompScreen *s,
	       XPoint	  *p,
	       int	  n,
	       float	  v)
{
    int	 x1, y1, x2, y2;
    Bool steep;
    int  tmp;
    int  deltaX, deltaY;
    int  error = 0;
    int  yStep;
    int  x, y;

    FROST_SCREEN (s);

#define SWAP(v0, v1) \
    tmp = v0;	     \
    v0 = v1;	     \
    v1 = tmp

    while (n > 1)
    {
	x1 = p->x;
	y1 = p->y;

	p++;
	n--;

	x2 = p->x;
	y2 = p->y;

	p++;
	n--;

	steep = abs (y2 - y1) > abs (x2 - x1);
	if (steep)
	{
	    SWAP (x1, y1);
	    SWAP (x2, y2);
	}

	if (x1 > x2)
	{
	    SWAP (x1, x2);
	    SWAP (y1, y2);
	}

#undef SWAP

	deltaX = x2 - x1;
	deltaY = abs (y2 - y1);

	y = y1;
	if (y1 < y2)
	    yStep = 1;
	else
	    yStep = -1;

	for (x = x1; x <= x2; x++)
	{
	    if (steep)
	    {
		SET (y, x, v);
	    }
	    else
	    {
		SET (x, y, v);
	    }

	    error += deltaY;
	    if (2 * error >= deltaX)
	    {
		y += yStep;
		error -= deltaX;
	    }
	}
    }
}

#undef SET

static void
softwareVertices (CompScreen *s,
		  GLenum     type,
		  XPoint     *p,
		  int	     n,
		  float	     v)
{
    switch (type) {
    case GL_POINTS:
	softwarePoints (s, p, n, v);
	break;
    case GL_LINES:
	softwareLines (s, p, n, v);
	break;
    }
}

static void
frostUpdate (CompScreen *s,
	     float	dt)
{
    GLfloat fade = 1.0f;

    FROST_SCREEN (s);

    if (fs->count < 1000)
    {
	if (fs->count > 1)
	    fade = 0.90f + fs->count / 10000.0f;
	else
	    fade = 0.0f;
    }

    if (!fboUpdate (s, dt, fade))
	softwareUpdate (s, dt, fade);
}

static void
scaleVertices (CompScreen *s,
	       XPoint	  *p,
	       int	  n)
{
    FROST_SCREEN (s);

    while (n--)
    {
	p[n].x = (fs->width  * p[n].x) / s->width;
	p[n].y = (fs->height * p[n].y) / s->height;
    }
}

static void
frostVertices (CompScreen *s,
	       GLenum     type,
	       XPoint     *p,
	       int	  n,
	       float	  v)
{
    FROST_SCREEN (s);

    if (!s->fragmentProgram)
	return;

    scaleVertices (s, p, n);

    if (!fboVertices (s, type, p, n, v))
	softwareVertices (s, type, p, n, v);

    if (fs->count < 3000)
	fs->count = 3000;
}

static Bool
frostRainTimeout (void *closure)
{
    CompScreen *s = closure;
    XPoint     p;

    p.x = (int) (s->width  * (rand () / (float) RAND_MAX));
    p.y = (int) (s->height * (rand () / (float) RAND_MAX));

    frostVertices (s, GL_POINTS, &p, 1, 0.8f * (rand () / (float) RAND_MAX));

    damageScreen (s);

    return TRUE;
}

static Bool
frostWiperTimeout (void *closure)
{
    CompScreen *s = closure;

    FROST_SCREEN (s);

    if (fs->count)
    {
	if (fs->wiperAngle == 0.0f)
	    fs->wiperSpeed = 2.5f;
	else if (fs->wiperAngle == 180.0f)
	    fs->wiperSpeed = -2.5f;
    }

    return TRUE;
}

static void
frostReset (CompScreen *s)
{
    int size, i, j;

    FROST_SCREEN (s);

    fs->height = TEXTURE_SIZE;
    fs->width  = (fs->height * s->width) / s->height;

    if (s->textureNonPowerOfTwo ||
	(POWER_OF_TWO (fs->width) && POWER_OF_TWO (fs->height)))
    {
	fs->target = GL_TEXTURE_2D;
	fs->tx = fs->ty = 1.0f;
    }
    else
    {
	fs->target = GL_TEXTURE_RECTANGLE_NV;
	fs->tx = fs->width;
	fs->ty = fs->height;
    }

    if (!s->fragmentProgram)
	return;

    if (s->fbo)
    {
	loadfrostProgram (s);
	if (!fs->fbo)
	    (*s->genFramebuffers) (1, &fs->fbo);
    }

    fs->fboStatus = 0;

    for (i = 0; i < TEXTURE_NUM; i++)
    {
	if (fs->texture[i])
	{
	    glDeleteTextures (1, &fs->texture[i]);
	    fs->texture[i] = 0;
	}
    }

    if (fs->data)
	free (fs->data);

    size = (fs->width + 2) * (fs->height + 2);

    fs->data = calloc (1, (sizeof (float) * size * 2) +
		       (sizeof (GLubyte) * fs->width * fs->height * 4));
    if (!fs->data)
	return;

    fs->d0 = fs->data;
    fs->d1 = (fs->d0 + (size));
    fs->t0 = (unsigned char *) (fs->d1 + (size));

    for (i = 0; i < fs->height; i++)
    {
	for (j = 0; j < fs->width; j++)
	{
	    (fs->t0 + (fs->width * 4 * i + j * 4))[0] = 0xff;
	}
    }
}

static void
frostDrawWindowTexture (CompWindow	     *w,
			CompTexture	     *texture,
			const FragmentAttrib *attrib,
			unsigned int	     mask)
{
    FROST_SCREEN (w->screen);

    if (fs->count)
    {
	FragmentAttrib fa = *attrib;
	Bool	       lighting = w->screen->lighting;
	int	       param, function, unit;
	GLfloat	       plane[4];

	FROST_DISPLAY (w->screen->display);

	param = allocFragmentParameters (&fa, 1);
	unit  = allocFragmentTextureUnits (&fa, 1);

	function = getBumpMapFragmentFunction (w->screen, texture, unit, param);
	if (function)
	{
	    addFragmentFunction (&fa, function);

	    screenLighting (w->screen, TRUE);

	    (*w->screen->activeTexture) (GL_TEXTURE0_ARB + unit);

	    glBindTexture (fs->target, fs->texture[TINDEX (fs, 0)]);

	    plane[1] = plane[2] = 0.0f;
	    plane[0] = fs->tx / (GLfloat) w->screen->width;
	    plane[3] = 0.0f;

	    glTexGeni (GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	    glTexGenfv (GL_S, GL_EYE_PLANE, plane);
	    glEnable (GL_TEXTURE_GEN_S);

	    plane[0] = plane[2] = 0.0f;
	    plane[1] = fs->ty / (GLfloat) w->screen->height;
	    plane[3] = 0.0f;

	    glTexGeni (GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	    glTexGenfv (GL_T, GL_EYE_PLANE, plane);
	    glEnable (GL_TEXTURE_GEN_T);

	    (*w->screen->activeTexture) (GL_TEXTURE0_ARB);

	    (*w->screen->programEnvParameter4f) (GL_FRAGMENT_PROGRAM_ARB, param,
						 texture->matrix.yy *
						 fd->offsetScale,
						 -texture->matrix.xx *
						 fd->offsetScale,
						 0.0f, 0.0f);
	}

	/* to get appropriate filtering of texture */
	mask |= PAINT_WINDOW_ON_TRANSFORMED_SCREEN_MASK;

	UNWRAP (fs, w->screen, drawWindowTexture);
	(*w->screen->drawWindowTexture) (w, texture, &fa, mask);
	WRAP (fs, w->screen, drawWindowTexture, frostDrawWindowTexture);

	if (function)
	{
	    (*w->screen->activeTexture) (GL_TEXTURE0_ARB + unit);
	    glDisable (GL_TEXTURE_GEN_T);
	    glDisable (GL_TEXTURE_GEN_S);
	    glBindTexture (fs->target, 0);
	    (*w->screen->activeTexture) (GL_TEXTURE0_ARB);

	    screenLighting (w->screen, lighting);
	}
    }
    else
    {
	UNWRAP (fs, w->screen, drawWindowTexture);
	(*w->screen->drawWindowTexture) (w, texture, attrib, mask);
	WRAP (fs, w->screen, drawWindowTexture, frostDrawWindowTexture);
    }
}

/* TODO: a way to control the speed */
static void
frostPreparePaintScreen (CompScreen *s,
			 int	    msSinceLastPaint)
{
    FROST_SCREEN (s);

    if (fs->count)
    {
	fs->count -= 10;
	if (fs->count < 0)
	    fs->count = 0;

	if (fs->wiperHandle)
	{
	    float  step, angle0, angle1;
	    Bool   wipe = FALSE;
	    XPoint p[3];

	    p[1].x = s->width / 2;
	    p[1].y = s->height;

	    step = fs->wiperSpeed * msSinceLastPaint / 20.0f;

	    if (fs->wiperSpeed > 0.0f)
	    {
		if (fs->wiperAngle < 180.0f)
		{
		    angle0 = fs->wiperAngle;

		    fs->wiperAngle += step;
		    fs->wiperAngle = MIN (fs->wiperAngle, 180.0f);

		    angle1 = fs->wiperAngle;

		    wipe = TRUE;
		}
	    }
	    else
	    {
		if (fs->wiperAngle > 0.0f)
		{
		    angle1 = fs->wiperAngle;

		    fs->wiperAngle += step;
		    fs->wiperAngle = MAX (fs->wiperAngle, 0.0f);

		    angle0 = fs->wiperAngle;

		    wipe = TRUE;
		}
	    }

#define TAN(a) (tanf ((a) * (M_PI / 180.0f)))

	    if (wipe)
	    {
		if (angle0 > 0.0f)
		{
		    p[2].x = s->width / 2 - s->height / TAN (angle0);
		    p[2].y = 0;
		}
		else
		{
		    p[2].x = 0;
		    p[2].y = s->height;
		}

		if (angle1 < 180.0f)
		{
		    p[0].x = s->width / 2 - s->height / TAN (angle1);
		    p[0].y = 0;
		}
		else
		{
		    p[0].x = s->width;
		    p[0].y = s->height;
		}

		/* software rasterizer doesn't support triangles yet so wiper
		   effect will only work with FBOs right now */
		frostVertices (s, GL_TRIANGLES, p, 3, 0.0f);
	    }

#undef TAN

	}

	frostUpdate (s, 0.8f);
    }

    UNWRAP (fs, s, preparePaintScreen);
    (*s->preparePaintScreen) (s, msSinceLastPaint);
    WRAP (fs, s, preparePaintScreen, frostPreparePaintScreen);
}

static void
frostDonePaintScreen (CompScreen *s)
{
    FROST_SCREEN (s);

    if (fs->count)
	damageScreen (s);

    UNWRAP (fs, s, donePaintScreen);
    (*s->donePaintScreen) (s);
    WRAP (fs, s, donePaintScreen, frostDonePaintScreen);
}

static void
frostHandleMotionEvent (CompDisplay *d,
			Window	    root)
{
    CompScreen *s;

    s = findScreenAtDisplay (d, root);
    if (s)
    {
	FROST_SCREEN (s);

	if (fs->grabIndex)
	{
	    XPoint p[2];

	    p[0].x = frostLastPointerX;
	    p[0].y = frostLastPointerY;

	    p[1].x = frostLastPointerX = pointerX;
	    p[1].y = frostLastPointerY = pointerY;

	    frostVertices (s, GL_LINES, p, 2, 0.2f);

	    damageScreen (s);
	}
    }
}

static Bool
frostInitiate (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    CompScreen   *s;
    unsigned int ui;
    Window	 root, child;
    int	         xRoot, yRoot, i;

    for (s = d->screens; s; s = s->next)
    {
	FROST_SCREEN (s);

	if (otherScreenGrabExist (s, "frost", NULL))
	    continue;

	if (!fs->grabIndex)
	    fs->grabIndex = pushScreenGrab (s, None, "frost");

	if (XQueryPointer (d->display, s->root, &root, &child, &xRoot, &yRoot,
			   &i, &i, &ui))
	{
	    XPoint p;

	    p.x = frostLastPointerX = xRoot;
	    p.y = frostLastPointerY = yRoot;

	    frostVertices (s, GL_POINTS, &p, 1, 0.8f);

	    damageScreen (s);
	}
    }

    if (state & CompActionStateInitButton)
	action->state |= CompActionStateTermButton;

    if (state & CompActionStateInitKey)
	action->state |= CompActionStateTermKey;

    return FALSE;
}

static Bool
frostTerminate (CompDisplay	*d,
		CompAction	*action,
		CompActionState state,
		CompOption	*option,
		int		nOption)
{
    CompScreen *s;

    for (s = d->screens; s; s = s->next)
    {
	FROST_SCREEN (s);

	if (fs->grabIndex)
	{
	    removeScreenGrab (s, fs->grabIndex, 0);
	    fs->grabIndex = 0;
	}
    }

    return FALSE;
}

static Bool
frostToggleRain (CompDisplay     *d,
		 CompAction      *action,
		 CompActionState state,
		 CompOption      *option,
		 int	         nOption)
{
    CompScreen *s;

    FROST_DISPLAY (d);

    s = findScreenAtDisplay (d, getIntOptionNamed (option, nOption, "root", 0));
    if (s)
    {
	FROST_SCREEN (s);

	if (!fs->rainHandle)
	{
	    int delay;

	    delay = fd->opt[FROST_DISPLAY_OPTION_RAIN_DELAY].value.i;
	    fs->rainHandle = compAddTimeout (delay, (float) delay * 1.2,
					     frostRainTimeout, s);
	}
	else
	{
	    compRemoveTimeout (fs->rainHandle);
	    fs->rainHandle = 0;
	}
    }

    return FALSE;
}

static Bool
frostToggleWiper (CompDisplay     *d,
		  CompAction      *action,
		  CompActionState state,
		  CompOption      *option,
		  int	          nOption)
{
    CompScreen *s;

    s = findScreenAtDisplay (d, getIntOptionNamed (option, nOption, "root", 0));
    if (s)
    {
	FROST_SCREEN (s);

	if (!fs->wiperHandle)
	{
	    fs->wiperHandle = compAddTimeout (2000, 2400, frostWiperTimeout, s);
	}
	else
	{
	    compRemoveTimeout (fs->wiperHandle);
	    fs->wiperHandle = 0;
	}
    }

    return FALSE;
}

static Bool
frostTitleWave (CompDisplay     *d,
		CompAction      *action,
		CompActionState state,
		CompOption      *option,
		int	        nOption)
{
    CompWindow *w;
    int	       xid;

    xid = getIntOptionNamed (option, nOption, "window", d->activeWindow);

    w = findWindowAtDisplay (d, xid);
    if (w)
    {
	XPoint p[2];

	p[0].x = w->attrib.x - w->input.left;
	p[0].y = w->attrib.y - w->input.top / 2;

	p[1].x = w->attrib.x + w->width + w->input.right;
	p[1].y = p[0].y;

	frostVertices (w->screen, GL_LINES, p, 2, 0.15f);

	damageScreen (w->screen);
    }

    return FALSE;
}

static Bool
frostPoint (CompDisplay     *d,
	    CompAction      *action,
	    CompActionState state,
	    CompOption      *option,
	    int	            nOption)
{
    CompScreen *s;
    int	       xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);

    s = findScreenAtDisplay (d, xid);
    if (s)
    {
	XPoint p;
	float  amp;

	p.x = getIntOptionNamed (option, nOption, "x", s->width / 2);
	p.y = getIntOptionNamed (option, nOption, "y", s->height / 2);

	amp = getFloatOptionNamed (option, nOption, "amplitude", 0.5f);

	frostVertices (s, GL_POINTS, &p, 1, amp);

	damageScreen (s);
    }

    return FALSE;
}

static Bool
frostLine (CompDisplay     *d,
	   CompAction      *action,
	   CompActionState state,
	   CompOption      *option,
	   int	           nOption)
{
    CompScreen *s;
    int	       xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);

    s = findScreenAtDisplay (d, xid);
    if (s)
    {
	XPoint p[2];
	float  amp;

	p[0].x = getIntOptionNamed (option, nOption, "x0", s->width / 4);
	p[0].y = getIntOptionNamed (option, nOption, "y0", s->height / 2);

	p[1].x = getIntOptionNamed (option, nOption, "x1",
				    s->width - s->width / 4);
	p[1].y = getIntOptionNamed (option, nOption, "y1", s->height / 2);


	amp = getFloatOptionNamed (option, nOption, "amplitude", 0.25f);

	frostVertices (s, GL_LINES, p, 2, amp);

	damageScreen (s);
    }

    return FALSE;
}

static void
frostHandleEvent (CompDisplay *d,
		  XEvent      *event)
{
    CompScreen *s;

    FROST_DISPLAY (d);

    switch (event->type) {
    case ButtonPress:
	s = findScreenAtDisplay (d, event->xbutton.root);
	if (s)
	{
	    FROST_SCREEN (s);

	    if (fs->grabIndex)
	    {
		XPoint p;

		p.x = pointerX;
		p.y = pointerY;

		frostVertices (s, GL_POINTS, &p, 1, 0.8f);
		damageScreen (s);
	    }
	}
	break;
    case EnterNotify:
    case LeaveNotify:
	frostHandleMotionEvent (d, event->xcrossing.root);
	break;
    case MotionNotify:
	frostHandleMotionEvent (d, event->xmotion.root);
    default:
	break;
    }

    UNWRAP (fd, d, handleEvent);
    (*d->handleEvent) (d, event);
    WRAP (fd, d, handleEvent, frostHandleEvent);
}

static CompOption *
frostGetDisplayOptions (CompPlugin  *plugin,
			CompDisplay *display,
			int	    *count)
{
    FROST_DISPLAY (display);

    *count = NUM_OPTIONS (fd);
    return fd->opt;
}

static Bool
frostSetDisplayOption (CompPlugin      *plugin,
		       CompDisplay     *display,
		       const char      *name,
		       CompOptionValue *value)
{
    CompOption *o;
    int	       index;

    FROST_DISPLAY (display);

    o = compFindOption (fd->opt, NUM_OPTIONS (fd), name, &index);
    if (!o)
	return FALSE;

    switch (index) {
    case FROST_DISPLAY_OPTION_OFFSET_SCALE:
	if (compSetFloatOption (o, value))
	{
	    fd->offsetScale = o->value.f * 50.0f;
	    return TRUE;
	}
	break;
    case FROST_DISPLAY_OPTION_RAIN_DELAY:
	if (compSetIntOption (o, value))
	{
	    CompScreen *s;

	    for (s = display->screens; s; s = s->next)
	    {
		FROST_SCREEN (s);

		if (!fs->rainHandle)
		    continue;

		compRemoveTimeout (fs->rainHandle);
		fs->rainHandle = compAddTimeout (value->i,
						 (float)value->i * 1.2,
						 frostRainTimeout, s);
	    }
	    return TRUE;
	}
	break;
    default:
	return compSetDisplayOption (display, o, value);
    }

    return FALSE;
}

static const CompMetadataOptionInfo frostDisplayOptionInfo[] = {
    { "initiate_key", "key", 0, frostInitiate, frostTerminate },
    { "toggle_rain_key", "key", 0, frostToggleRain, 0 },
    { "toggle_wiper_key", "key", 0, frostToggleWiper, 0 },
    { "offset_scale", "float", "<min>0</min>", 0, 0 },
    { "rain_delay", "int", "<min>1</min>", 0, 0 },
    { "title_wave", "bell", 0, frostTitleWave, 0 },
    { "point", "action", 0, frostPoint, 0 },
    { "line", "action", 0, frostLine, 0 }
};

static Bool
frostInitDisplay (CompPlugin  *p,
		  CompDisplay *d)
{
    frostDisplay *fd;

    if (!checkPluginABI ("core", CORE_ABIVERSION))
	return FALSE;

    fd = malloc (sizeof (frostDisplay));
    if (!fd)
	return FALSE;

    if (!compInitDisplayOptionsFromMetadata (d,
					     &frostMetadata,
					     frostDisplayOptionInfo,
					     fd->opt,
					     FROST_DISPLAY_OPTION_NUM))
    {
	free (fd);
	return FALSE;
    }

    fd->screenPrivateIndex = allocateScreenPrivateIndex (d);
    if (fd->screenPrivateIndex < 0)
    {
	compFiniDisplayOptions (d, fd->opt, FROST_DISPLAY_OPTION_NUM);
	free (fd);
	return FALSE;
    }

    fd->offsetScale = fd->opt[FROST_DISPLAY_OPTION_OFFSET_SCALE].value.f * 50.0f;

    WRAP (fd, d, handleEvent, frostHandleEvent);

    d->base.privates[displayPrivateIndex].ptr = fd;

    return TRUE;
}

static void
frostFiniDisplay (CompPlugin  *p,
		  CompDisplay *d)
{
    FROST_DISPLAY (d);

    freeScreenPrivateIndex (d, fd->screenPrivateIndex);

    UNWRAP (fd, d, handleEvent);

    compFiniDisplayOptions (d, fd->opt, FROST_DISPLAY_OPTION_NUM);

    free (fd);
}

static Bool
frostInitScreen (CompPlugin *p,
		 CompScreen *s)
{
    frostScreen *fs;

    FROST_DISPLAY (s->display);

    fs = calloc (1, sizeof (frostScreen));
    if (!fs)
	return FALSE;

    fs->grabIndex = 0;

    WRAP (fs, s, preparePaintScreen, frostPreparePaintScreen);
    WRAP (fs, s, donePaintScreen, frostDonePaintScreen);
    WRAP (fs, s, drawWindowTexture, frostDrawWindowTexture);

    s->base.privates[fd->screenPrivateIndex].ptr = fs;

    frostReset (s);

    return TRUE;
}

static void
frostFiniScreen (CompPlugin *p,
		 CompScreen *s)
{
    frostFunction *function, *next;
    int		  i;

    FROST_SCREEN (s);

    if (fs->rainHandle)
	compRemoveTimeout (fs->rainHandle);

    if (fs->wiperHandle)
	compRemoveTimeout (fs->wiperHandle);

    if (fs->fbo)
	(*s->deleteFramebuffers) (1, &fs->fbo);

    for (i = 0; i < TEXTURE_NUM; i++)
    {
	if (fs->texture[i])
	    glDeleteTextures (1, &fs->texture[i]);
    }

    if (fs->program)
	(*s->deletePrograms) (1, &fs->program);

    if (fs->data)
	free (fs->data);

    function = fs->bumpMapFunctions;
    while (function)
    {
	destroyFragmentFunction (s, function->handle);

	next = function->next;
	free (function);
	function = next;
    }

    UNWRAP (fs, s, preparePaintScreen);
    UNWRAP (fs, s, donePaintScreen);
    UNWRAP (fs, s, drawWindowTexture);

    free (fs);
}

static CompBool
frostInitObject (CompPlugin *p,
		 CompObject *o)
{
    static InitPluginObjectProc dispTab[] = {
	(InitPluginObjectProc) 0, /* InitCore */
	(InitPluginObjectProc) frostInitDisplay,
	(InitPluginObjectProc) frostInitScreen
    };

    RETURN_DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), TRUE, (p, o));
}

static void
frostFiniObject (CompPlugin *p,
		 CompObject *o)
{
    static FiniPluginObjectProc dispTab[] = {
	(FiniPluginObjectProc) 0, /* FiniCore */
	(FiniPluginObjectProc) frostFiniDisplay,
	(FiniPluginObjectProc) frostFiniScreen
    };

    DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), (p, o));
}

static CompOption *
frostGetObjectOptions (CompPlugin *plugin,
		       CompObject *object,
		       int	  *count)
{
    static GetPluginObjectOptionsProc dispTab[] = {
	(GetPluginObjectOptionsProc) 0, /* GetCoreOptions */
	(GetPluginObjectOptionsProc) frostGetDisplayOptions
    };

    *count = 0;
    RETURN_DISPATCH (object, dispTab, ARRAY_SIZE (dispTab),
		     (void *) count, (plugin, object, count));
}

static CompBool
frostSetObjectOption (CompPlugin      *plugin,
		      CompObject      *object,
		      const char      *name,
		      CompOptionValue *value)
{
    static SetPluginObjectOptionProc dispTab[] = {
	(SetPluginObjectOptionProc) 0, /* SetCoreOption */
	(SetPluginObjectOptionProc) frostSetDisplayOption
    };

    RETURN_DISPATCH (object, dispTab, ARRAY_SIZE (dispTab), FALSE,
		     (plugin, object, name, value));
}

static Bool
frostInit (CompPlugin *p)
{
    if (!compInitPluginMetadataFromInfo (&frostMetadata,
					 p->vTable->name,
					 frostDisplayOptionInfo,
					 FROST_DISPLAY_OPTION_NUM,
					 0, 0))
	return FALSE;

    displayPrivateIndex = allocateDisplayPrivateIndex ();
    if (displayPrivateIndex < 0)
    {
	compFiniMetadata (&frostMetadata);
	return FALSE;
    }

    compAddMetadataFromFile (&frostMetadata, p->vTable->name);

    return TRUE;
}

static void
frostFini (CompPlugin *p)
{
    freeDisplayPrivateIndex (displayPrivateIndex);
    compFiniMetadata (&frostMetadata);
}

static CompMetadata *
frostGetMetadata (CompPlugin *plugin)
{
    return &frostMetadata;
}

static CompPluginVTable frostVTable = {
    "frost",
    frostGetMetadata,
    frostInit,
    frostFini,
    frostInitObject,
    frostFiniObject,
    frostGetObjectOptions,
    frostSetObjectOption
};

CompPluginVTable *
getCompPluginInfo20070830 (void)
{
    return &frostVTable;
}
