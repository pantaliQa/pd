/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* changes by Thomas Musil IEM KUG Graz Austria 2001 */
/* have to insert gui-objects into editor-list */
/* all changes are labeled with      iemlib      */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "m_pd.h"
#include "m_imp.h"
#include "s_stuff.h"
#include "g_canvas.h"
#include "s_utf8.h"

#define LMARGIN 2
#define RMARGIN 2
#define TMARGIN 4
/* for some reason, it draws text 1 pixel lower on Mac OS X */
#ifdef __APPLE__
#define BMARGIN 1
#else
#define BMARGIN 1
#endif

#define SEND_FIRST 1
#define SEND_UPDATE 2
#define SEND_CHECK 0

// selection for shift+arrow selecting
// 0 = none;
// 1 = start;
// 2 = end;
static int last_sel = 0;

struct _rtext
{
    char *x_buf;    /*-- raw byte string, assumed UTF-8 encoded (moo) --*/
    int x_bufsize;  /*-- byte length --*/
    int x_selstart; /*-- byte offset --*/
    int x_selend;   /*-- byte offset --*/
    int x_active;
    int x_dragfrom;
    int x_drawnwidth;
    int x_drawnheight;
    t_text *x_text;
    t_glist *x_glist;
    char x_tag[50];
    struct _rtext *x_next;
};

t_rtext *rtext_new(t_glist *glist, t_text *who)
{
    t_rtext *x = (t_rtext *)getbytes(sizeof *x);
    x->x_text = who;
    x->x_glist = glist;
    x->x_next = glist->gl_editor->e_rtext;
    x->x_selstart = x->x_selend = x->x_active =
        x->x_drawnwidth = x->x_drawnheight = 0;
    binbuf_gettext(who->te_binbuf, &x->x_buf, &x->x_bufsize);
    glist->gl_editor->e_rtext = x;
    // here we use a more complex tag which will later help us properly
    // select objects inside a gop on its parent that are otherwise not
    // supposed to be there (they don't belong to that canvas). See
    // in pd.tk pdtk_select_all_gop_widgets function and how it affects
    // draw data structures that are displayed via gop (Ico 20140831)
    sprintf(x->x_tag, ".x%lx.t%lx", (t_int)glist_getcanvas(x->x_glist),
        (t_int)x);
    return (x);
}

static t_rtext *rtext_entered;

void rtext_free(t_rtext *x)
{
    t_editor *e = x->x_glist->gl_editor;
    if (e->e_textedfor == x)
        e->e_textedfor = 0;
    if (e->e_rtext == x)
        e->e_rtext = x->x_next;
    else
    {
        t_rtext *e2;
        for (e2 = e->e_rtext; e2; e2 = e2->x_next)
            if (e2->x_next == x)
        {
            e2->x_next = x->x_next;
            break;
        }
    }
    if (rtext_entered == x) rtext_entered = 0;
    freebytes(x->x_buf, x->x_bufsize);
    freebytes(x, sizeof *x);
}

char *rtext_gettag(t_rtext *x)
{
    return (x->x_tag);
}

void rtext_gettext(t_rtext *x, char **buf, int *bufsize)
{
    *buf = x->x_buf;
    *bufsize = x->x_bufsize;
}

void rtext_getseltext(t_rtext *x, char **buf, int *bufsize)
{
    *buf = x->x_buf + x->x_selstart;
    *bufsize = x->x_selend - x->x_selstart;
}

/* convert t_text te_type symbol for use as a Tk tag */
static t_symbol *rtext_gettype(t_rtext *x)
{
    switch (x->x_text->te_type) 
    {
    case T_TEXT: return gensym("text");
    case T_OBJECT: return gensym("obj");
    case T_MESSAGE: return gensym("msg");
    case T_ATOM: return gensym("atom");
    }
    return (&s_);
}

/* LATER deal with tcl-significant characters */

/* firstone(), lastone()
 *  + returns byte offset of (first|last) occurrence of 'c' in 's[0..n-1]', or
 *    -1 if none was found
 *  + 's' is a raw byte string
 *  + 'c' is a byte value
 *  + 'n' is the length (in bytes) of the prefix of 's' to be searched.
 *  + we could make these functions work on logical characters in utf8 strings,
 *    but we don't really need to...
 */
static int firstone(char *s, int c, int n)
{
    char *s2 = s + n;
    int i = 0;
    while (s != s2)
    {
        //fprintf(stderr,"s=<%s> n=%d s=%d c=%d s2=%d\n", s, n, *s, c, *s2);
        if (*s == c)
        {
            //fprintf(stderr,"DONE\n");
            return (i);
        }
        i++;
        s++;
    }
    //fprintf(stderr,"FAILED\n");
    return (-1);
}

static int lastone(char *s, int c, int n)
{
    char *s2 = s + n;
    while (s2 != s)
    {
        s2--;
        n--;
        if (*s2 == c) return (n);
    }
    return (-1);
}

    /* the following routine computes line breaks and carries out
    some action which could be:
        SEND_FIRST - draw the box  for the first time
        SEND_UPDATE - redraw the updated box
        otherwise - don't draw, just calculate.
    Called with *widthp and *heightp as coordinates of
    a test point, the routine reports the index of the character found
    there in *indexp.  *widthp and *heightp are set to the width and height
    of the entire text in pixels.
    */

   /*-- moo: 
    * + some variables from the original version have been renamed
    * + variables with a "_b" suffix are raw byte strings, lengths, or offsets
    * + variables with a "_c" suffix are logical character lengths or offsets
    *   (assuming valid UTF-8 encoded byte string in x->x_buf)
    * + a fair amount of O(n) computations required to convert between raw byte
    *   offsets (needed by the C side) and logical character offsets (needed by
    *   the GUI)
    */

    /* LATER get this and sys_vgui to work together properly,
        breaking up messages as needed.  As of now, there's
        a limit of 1950 characters, imposed by sys_vgui(). */
#define UPBUFSIZE 4000
#define BOXWIDTH 60

/* Older (pre-8.3.4) TCL versions handle text selection differently; this
flag is set from the GUI if this happens.  LATER take this out: early 2006? */

extern int sys_oldtclversion;           

static void rtext_senditup(t_rtext *x, int action, int *widthp, int *heightp,
    int *indexp)
{
    //fprintf(stderr,"rtext_senditup <%s>\n", x->x_buf);
    if (x)
    {
        t_float dispx, dispy;
        char smallbuf[200] = { '\0' }, *tempbuf;
        int outchars_b = 0, nlines = 0, ncolumns = 0,
            pixwide, pixhigh, font, fontwidth, fontheight, findx, findy;
        int reportedindex = 0;
        t_canvas *canvas = glist_getcanvas(x->x_glist);
        int widthspec_c = x->x_text->te_width; // width if any specified
        // width limit in chars
        int widthlimit_c = (widthspec_c ? widthspec_c : BOXWIDTH);
        int inindex_b = 0; // index location in the buffer
        int inindex_c = 0; // index location in the u8 chars
        int selstart_b = 0, selend_b = 0; // selection start and end
        // buffer size in u8 chars
        //fprintf(stderr,"buf = <%s> | last 2 chars = %d %d\n", x->x_buf, x->x_buf[x->x_bufsize-1], x->x_buf[x->x_bufsize]);
        int x_bufsize_c = u8_charnum(x->x_buf, x->x_bufsize);
            /* if we're a GOP (the new, "goprect" style) borrow the font size
            from the inside to preserve the spacing */
        if (pd_class(&x->x_text->te_pd) == canvas_class &&
            ((t_glist *)(x->x_text))->gl_isgraph &&
            ((t_glist *)(x->x_text))->gl_goprect)
                font =  glist_getfont((t_glist *)(x->x_text));
        else font = glist_getfont(x->x_glist);
        fontwidth = sys_fontwidth(font);
        fontheight = sys_fontheight(font);
        // calculating x and y in pixels
        findx = (*widthp + (fontwidth/2)) / fontwidth;
        findy = *heightp / fontheight;
        if (x->x_bufsize >= 100)
             tempbuf = (char *)t_getbytes(2 * x->x_bufsize + 1);
        else tempbuf = smallbuf;
        while (x_bufsize_c - inindex_c > 0)
        {
            int inchars_b  = x->x_bufsize - inindex_b;
            int inchars_c  = x_bufsize_c  - inindex_c;
            int maxindex_c =
                (inchars_c > widthlimit_c ? widthlimit_c : inchars_c);
            int maxindex_b = u8_offset(x->x_buf + inindex_b, maxindex_c);
            int eatchar = 1;
            //fprintf(stderr, "firstone <%s> inindex_b=%d maxindex_b=%d\n", x->x_buf + inindex_b, inindex_b, maxindex_b);
            int foundit_b  = firstone(x->x_buf + inindex_b, '\n', maxindex_b);
            int foundit_c;
            //following deals with \v replacement for \n in multiline comments
            int foundit_bv  = firstone(x->x_buf + inindex_b, '\v', maxindex_b);
            if ((foundit_bv < foundit_b && foundit_bv != -1) ||
                (foundit_b == -1 && foundit_bv != -1))
                foundit_b = foundit_bv;
            if (foundit_b < 0) //if we did not find an \n
            { 
                /* too much text to fit in one line? */
                if (inchars_c > widthlimit_c)
                {
                    /* is there a space to break the line at?  OK if it's even
                    one byte past the end since in this context we know there's
                    more text */
                    foundit_b =
                        lastone(x->x_buf + inindex_b, ' ', maxindex_b + 1);
                    if (foundit_b < 0)
                    {
                        foundit_b = maxindex_b;
                        foundit_c = maxindex_c;
                        eatchar = 0;
                    }
                    else
                        foundit_c = u8_charnum(x->x_buf + inindex_b, foundit_b);
                }
                else
                {
                    foundit_b = inchars_b;
                    foundit_c = inchars_c;
                    eatchar = 0;
                }
            }
            else
                foundit_c = u8_charnum(x->x_buf + inindex_b, foundit_b);

            if (nlines == findy)
            {
                int actualx = (findx < 0 ? 0 :
                    (findx > foundit_c ? foundit_c : findx));
                *indexp = inindex_b + u8_offset(x->x_buf + inindex_b, actualx);
                reportedindex = 1;
            }
            strncpy(tempbuf+outchars_b, x->x_buf + inindex_b, foundit_b);
            if (x->x_selstart >= inindex_b &&
                x->x_selstart <= inindex_b + foundit_b + eatchar)
                    selstart_b = x->x_selstart + outchars_b - inindex_b;
            if (x->x_selend >= inindex_b &&
                x->x_selend <= inindex_b + foundit_b + eatchar)
                    selend_b = x->x_selend + outchars_b - inindex_b;
            outchars_b += foundit_b;
            inindex_b += (foundit_b + eatchar);
            inindex_c += (foundit_c + eatchar);
            if (inindex_b < x->x_bufsize)
                tempbuf[outchars_b++] = '\n';
            // if we found a row that is longer than previous (total width)
            if (foundit_c > ncolumns)
                ncolumns = foundit_c;
            nlines++;
        }
        // append new line in case we end our input with an \n
        if (x_bufsize_c > 0 && (x->x_buf[x_bufsize_c - 1] == '\n' || x->x_buf[x_bufsize_c - 1] == '\v'))
        {
            nlines++;
            tempbuf[outchars_b++] = '\n';
            //tempbuf[outchars_b] = '\0';
            //outchars_b++;
        }
        if (!reportedindex)
            *indexp = outchars_b;
        dispx = text_xpix(x->x_text, x->x_glist);
        dispy = text_ypix(x->x_text, x->x_glist);
        if (nlines < 1) nlines = 1;
        if (!widthspec_c)
        {
            while (ncolumns < (x->x_text->te_type == T_TEXT ? 1 : 3))
            {
                tempbuf[outchars_b++] = ' ';
                ncolumns++;
            }
        }
        else ncolumns = widthspec_c;

        // add a null character at the end of the string (for u8_charnum)
        tempbuf[outchars_b++] = '\0';

        pixwide = ncolumns * fontwidth + (LMARGIN + RMARGIN);
        pixhigh = nlines * fontheight + (TMARGIN + BMARGIN);
        //printf("outchars_b=%d bufsize=%d %d\n", outchars_b, x->x_bufsize, x->x_buf[outchars_b]);

        if (action && x->x_text->te_width && x->x_text->te_type != T_ATOM)
        {
            /* if our width is specified but the "natural" width is the
               same as the specified width, set specified width to zero
               so future text editing will automatically change width.
               Except atoms whose content changes at runtime. */
            int widthwas = x->x_text->te_width, newwidth = 0, newheight = 0,
                newindex = 0;
            x->x_text->te_width = 0;
            rtext_senditup(x, 0, &newwidth, &newheight, &newindex);
            if (newwidth/fontwidth != widthwas)
                x->x_text->te_width = widthwas;
            else x->x_text->te_width = 0;
        }
        if (action == SEND_FIRST)
        {
            //fprintf(stderr,"send_first rtext=%lx t_text=%lx\n", x, x->x_text);
            sys_vgui("pdtk_text_new .x%lx.c {%s %s text %s} %f %f {%.*s} %d %s\n",
                canvas, x->x_tag, rtext_gettype(x)->s_name,
                (glist_isselected(x->x_glist, ((t_gobj*)x->x_text)) ? "selected" : ""),
                dispx + LMARGIN, dispy + TMARGIN,
                outchars_b, tempbuf, sys_hostfontsize(font),
                (glist_isselected(x->x_glist, ((t_gobj*)x->x_text)) ? "$pd_colors(selection)" : "$pd_colors(text)"));
        }
        else if (action == SEND_UPDATE)
        {
            sys_vgui("pdtk_text_set .x%lx.c %s {%.*s}\n",
                canvas, x->x_tag, outchars_b, tempbuf);

            if (pixwide != x->x_drawnwidth || pixhigh != x->x_drawnheight) 
                text_drawborder(x->x_text, x->x_glist, x->x_tag,
                    pixwide, pixhigh, 0);
            if (x->x_active)
            {
                if (selend_b > selstart_b)
                {
                    sys_vgui(".x%lx.c select from %s %d\n", canvas, 
                        x->x_tag, u8_charnum(tempbuf, selstart_b));
                    sys_vgui(".x%lx.c select to %s %d\n", canvas, 
                        x->x_tag, u8_charnum(tempbuf, selend_b)
                          + (sys_oldtclversion ? 0 : -1));
                    sys_vgui(".x%lx.c focus \"\"\n", canvas);        
                }
                else
                {
                    sys_vgui(".x%lx.c select clear\n", canvas);
                    sys_vgui(".x%lx.c icursor %s %d\n", canvas, x->x_tag,
                        u8_charnum(tempbuf, selstart_b));
                    sys_vgui(".x%lx.c focus %s\n", canvas, x->x_tag);        
                }
            }
        }
        x->x_drawnwidth = pixwide;
        x->x_drawnheight = pixhigh;
        
        *widthp = pixwide;
        *heightp = pixhigh;
        if (tempbuf != smallbuf)
            t_freebytes(tempbuf, 2 * x->x_bufsize + 1);
    }
    //printf("END\n");
}

void rtext_retext(t_rtext *x)
{
    int w = 0, h = 0, indx;
    t_text *text = x->x_text;
    t_freebytes(x->x_buf, x->x_bufsize);
    binbuf_gettext(text->te_binbuf, &x->x_buf, &x->x_bufsize);
    /* special case: for number boxes, try to pare the number down
       to the specified width of the box. */
    if (text->te_width > 0 && text->te_type == T_ATOM &&
        x->x_bufsize > text->te_width)
    {
        t_atom *atomp = binbuf_getvec(text->te_binbuf);
        int natom = binbuf_getnatom(text->te_binbuf);
        int bufsize = x->x_bufsize;
        if (natom == 1 && atomp->a_type == A_FLOAT)
        {
            /* try to reduce size by dropping decimal digits */
            int wantreduce = bufsize - text->te_width;
            char *decimal = 0, *nextchar, *ebuf = x->x_buf + bufsize,
                *s1, *s2;
            for (decimal = x->x_buf; decimal < ebuf; decimal++)
                if (*decimal == '.')
                    break;
            if (decimal >= ebuf)
                goto giveup;
            for (nextchar = decimal + 1; nextchar < ebuf; nextchar++)
                if (*nextchar < '0' || *nextchar > '9')
                    break;
            if (nextchar - decimal - 1 < wantreduce)
                goto giveup;
            for (s1 = nextchar - wantreduce, s2 = s1 + wantreduce;
                s2 < ebuf; s1++, s2++)
                    *s1 = *s2;
            x->x_buf = t_resizebytes(x->x_buf, bufsize, text->te_width);
            bufsize = text->te_width;
            goto done;
        giveup:
            /* give up and bash it to "+" or "-" */
            x->x_buf[0] = (atomp->a_w.w_float < 0 ? '-' : '+');
            x->x_buf = t_resizebytes(x->x_buf, bufsize, 1);
            bufsize = 1;
        }
        else if (bufsize > text->te_width)
        {
            x->x_buf[text->te_width - 1] = '>';
            x->x_buf = t_resizebytes(x->x_buf, bufsize, text->te_width);
            bufsize = text->te_width;
        }
    done:
        x->x_bufsize = bufsize;
    }
    rtext_senditup(x, SEND_UPDATE, &w, &h, &indx);
}

/* find the rtext that goes with a text item */
t_rtext *glist_findrtext(t_glist *gl, t_text *who)
{
    t_rtext *x=NULL;
    if (!gl->gl_editor)
        canvas_create_editor(gl);
    if (gl->gl_editor->e_rtext)
        for (x = gl->gl_editor->e_rtext; x && x->x_text != who; x = x->x_next)
            ;
    if (!x) bug("glist_findrtext");
    return (x);
}

/* same as above but without error reporting */
t_rtext *glist_tryfindrtext(t_glist *gl, t_text *who)
{
    t_rtext *x=NULL;
    if (!gl->gl_editor)
        canvas_create_editor(gl);
    if (gl->gl_editor->e_rtext)
        for (x = gl->gl_editor->e_rtext; x && x->x_text != who; x = x->x_next)
            ;
    return (x);
}

int rtext_width(t_rtext *x)
{
    int w = 0, h = 0, indx;
    rtext_senditup(x, SEND_CHECK, &w, &h, &indx);
    return (w);
}

int rtext_height(t_rtext *x)
{
    int w = 0, h = 0, indx;
    rtext_senditup(x, SEND_CHECK, &w, &h, &indx);
    return (h);
}

void rtext_draw(t_rtext *x)
{
    int w = 0, h = 0, indx;
    rtext_senditup(x, SEND_FIRST, &w, &h, &indx);
}

void rtext_erase(t_rtext *x)
{
    if (x && x->x_glist)
        sys_vgui(".x%lx.c delete %s\n", glist_getcanvas(x->x_glist), x->x_tag);
}

void rtext_displace(t_rtext *x, int dx, int dy)
{
    sys_vgui(".x%lx.c move %s %d %d\n", glist_getcanvas(x->x_glist), 
        x->x_tag, dx, dy);
}

void rtext_select(t_rtext *x, int state)
{
    t_glist *glist = x->x_glist;
    t_canvas *canvas = glist_getcanvas(glist);
    if (glist_istoplevel(glist))
        sys_vgui(".x%lx.c itemconfigure %s -fill %s\n", canvas, 
            x->x_tag, (state? "$pd_colors(selection)" : "$pd_colors(text)"));
    if (x->x_text->te_pd->c_wb && x->x_text->te_pd->c_wb->w_displacefnwtag)
    {
        if (state)
            sys_vgui(".x%lx.c addtag selected withtag %s\n",
                   glist_getcanvas(glist), x->x_tag);
        else
            sys_vgui(".x%lx.c dtag %s selected\n",
                   glist_getcanvas(glist), x->x_tag);
    }
    canvas_editing = canvas;
}

void rtext_activate(t_rtext *x, int state)
{
    //fprintf(stderr,"rtext_activate\n");
    int w = 0, h = 0, indx;
    t_glist *glist = x->x_glist;
    t_canvas *canvas = glist_getcanvas(glist);
    //if (state && x->x_active) printf("duplicate rtext_activate\n");
    // the following prevents from selecting all when inside an
    // object that is already being texted for... please *test*
    // "fixes" before committing them
    //if (state == x->x_active) return; // avoid excess calls
    if (state)
    {
        sys_vgui(".x%lx.c focus %s\n", canvas, x->x_tag);
        glist->gl_editor->e_textedfor = x;
        glist->gl_editor->e_textdirty = 0;
        x->x_dragfrom = x->x_selstart = 0;
        x->x_selend = x->x_bufsize;
        x->x_active = 1;
    }
    else
    {
        sys_vgui("selection clear .x%lx.c\n", canvas);
        sys_vgui(".x%lx.c focus \"\"\n", canvas);
        if (glist->gl_editor->e_textedfor == x)
            glist->gl_editor->e_textedfor = 0;
        x->x_active = 0;
    }
    rtext_senditup(x, SEND_UPDATE, &w, &h, &indx);
}

// outputs 1 if found one of the special chars
// this function is used with traversal through rtext below
// using ctrl+left/right and similar shortcuts
static int rtext_compare_special_chars(const char c)
{
        if (c != '\n' && c != '\v' && c != ' ')
            return 0;
        return 1;
}

void rtext_key(t_rtext *x, int keynum, t_symbol *keysym)
{
    //fprintf(stderr,"rtext_key %d %s\n", keynum, keysym->s_name);
    int w = 0, h = 0, indx, i, newsize, ndel;
    if (keynum)
    {
        int n = keynum;
        if (n == '\r' || n == '\v') n = '\n';
        if (n == '\b') /* backspace */
        {
            if (x->x_selstart && (x->x_selstart == x->x_selend))
            {
                u8_dec(x->x_buf, &x->x_selstart);
                if (glist_isvisible(glist_getcanvas(x->x_glist)))
                    canvas_getscroll(glist_getcanvas(x->x_glist));
            }
            
        }
        else if (n == 127) /* delete */
        {
            if (x->x_selend < x->x_bufsize && (x->x_selstart == x->x_selend))
                u8_inc(x->x_buf, &x->x_selend);
            if (glist_isvisible(glist_getcanvas(x->x_glist)))
                canvas_getscroll(glist_getcanvas(x->x_glist));
        }
        
        ndel = x->x_selend - x->x_selstart;
        if (ndel)
        {
            for (i = x->x_selend; i < x->x_bufsize; i++)
                x->x_buf[i- ndel] = x->x_buf[i];
            newsize = x->x_bufsize - ndel;
            x->x_buf = resizebytes(x->x_buf, x->x_bufsize, newsize);
            x->x_bufsize = newsize;
        }

/* at Guenter's suggestion, use 'n>31' to test wither a character might
be printable in whatever 8-bit character set we find ourselves. */

/*-- moo:
  ... but test with "<" rather than "!=" in order to accomodate unicode
  codepoints for n (which we get since Tk is sending the "%A" substitution
  for bind <Key>), effectively reducing the coverage of this clause to 7
  bits.  Case n>127 is covered by the next clause.
*/
        if (n == '\n' || (n > 31 && n < 127))
        {
            newsize = x->x_bufsize+1;
            x->x_buf = resizebytes(x->x_buf, x->x_bufsize, newsize);
            for (i = x->x_bufsize; i > x->x_selstart; i--)
                x->x_buf[i] = x->x_buf[i-1];
            x->x_buf[x->x_selstart] = n;
            x->x_bufsize = newsize;
            x->x_selstart = x->x_selstart + 1;
            if (glist_isvisible(glist_getcanvas(x->x_glist)))
                canvas_getscroll(glist_getcanvas(x->x_glist));
        }
        /*--moo: check for unicode codepoints beyond 7-bit ASCII --*/
        else if (n > 127)
        {
            int ch_nbytes = u8_wc_nbytes(n);
            newsize = x->x_bufsize + ch_nbytes;
            x->x_buf = resizebytes(x->x_buf, x->x_bufsize, newsize);
            //fprintf(stderr,"x->x_bufsize=%d newsize=%d\n", x->x_bufsize, newsize);
            //for (i = newsize-1; i >= x->x_selstart; i--)
            //{
                //fprintf(stderr,"%d-%d <%d>\n", i, i-ch_nbytes, x->x_buf[i-ch_nbytes]);
                //x->x_buf[i] = '\0';
            //}
            x->x_bufsize = newsize;
            /*-- moo: assume canvas_key() has encoded keysym as UTF-8 */
            strncpy(x->x_buf+x->x_selstart, keysym->s_name, ch_nbytes);
            x->x_selstart = x->x_selstart + ch_nbytes;
        }
        x->x_selend = x->x_selstart;
        x->x_glist->gl_editor->e_textdirty = 1;
    }
    else if (!strcmp(keysym->s_name, "Right"))
    {
        if (x->x_selend == x->x_selstart && x->x_selstart < x->x_bufsize)
        {
            u8_inc(x->x_buf, &x->x_selstart);
            x->x_selend = x->x_selstart;
        }
        else
            x->x_selstart = x->x_selend;
        last_sel = 0;        
    }
    else if (!strcmp(keysym->s_name, "Left"))
    {
        if (x->x_selend == x->x_selstart && x->x_selstart > 0)
        {
            u8_dec(x->x_buf, &x->x_selstart);
            x->x_selend = x->x_selstart;
        }
        else
            x->x_selend = x->x_selstart;
        last_sel = 0;
    }
    else if (!strcmp(keysym->s_name, "ShiftRight"))
    {
        if (!last_sel) last_sel = 2;
        if (last_sel == 1 && x->x_selstart < x->x_selend)
        {
            if (x->x_selstart < x->x_bufsize)
                u8_inc(x->x_buf, &x->x_selstart);        
        }
        else
        {
            last_sel = 2;
            if (x->x_selend < x->x_bufsize)
                u8_inc(x->x_buf, &x->x_selend);
        }
    }
    else if (!strcmp(keysym->s_name, "ShiftLeft"))
    {
        if (!last_sel) last_sel = 1;
        if (last_sel == 2 && x->x_selend > x->x_selstart)
        {
            x->x_selend = x->x_selend - 1;
        }
        else
        {
            last_sel = 1;
            if (x->x_selstart > 0)
                u8_dec(x->x_buf, &x->x_selstart);
        }
    }
    else if (!strcmp(keysym->s_name, "Up"))
    {
        if (x->x_selstart != x->x_selend)
        {
            x->x_selend = x->x_selstart;
            last_sel = 0;
        }
        else
        {
            // we do this twice and then move to the right
            // as many spots as we had before, this will
            // allow us to go visually above where we used
            // to be in multiline situations (e.g. comments)
            int right = 0;
            //printf("start: selstart=%d x->x_bufsize=%d\n", x->x_selstart, x->x_bufsize);
            if (x->x_selstart > 0 &&
                   (x->x_selstart == x->x_bufsize ||
                    x->x_buf[x->x_selstart] == '\n' ||
                    x->x_buf[x->x_selstart] == '\v'))
            {
                //printf("found break\n");
                u8_dec(x->x_buf, &x->x_selstart);
                right++;
            }
            while (x->x_selstart > 0 &&
                    (x->x_buf[x->x_selstart-1] != '\n' &&
                        x->x_buf[x->x_selstart-1] != '\v'))
            {
                u8_dec(x->x_buf, &x->x_selstart);
                right++;
            }
            if (x->x_selstart == 0)
                right = 0;
            //printf("first linebreak: right=%d selstart=%d\n", right, x->x_selstart);
            if (x->x_selstart > 0)
                u8_dec(x->x_buf, &x->x_selstart);
            //printf("decrease by 1: selstart=%d\n", x->x_selstart);
            while (x->x_selstart > 0 &&
                (x->x_buf[x->x_selstart-1] != '\n' &&
                    x->x_buf[x->x_selstart-1] != '\v'))
                u8_dec(x->x_buf, &x->x_selstart);
            //printf("second linebreak: selstart=%d\n", x->x_selstart);
            if (x->x_selstart < x->x_bufsize && right > 0)
            {
                u8_inc(x->x_buf, &x->x_selstart);
                right--;
            }
            //printf("increase by 1: selstart=%d\n", x->x_selstart);
            while (right > 0 && 
                (x->x_buf[x->x_selstart] != '\n' &&
                    x->x_buf[x->x_selstart] != '\v'))
            {
                u8_inc(x->x_buf, &x->x_selstart);
                right--;
            }
            //printf("final: selstart=%d\n", x->x_selstart);
            x->x_selend = x->x_selstart;
            last_sel = 0;
        }
    }
    else if (!strcmp(keysym->s_name, "Down"))
    {
        if (x->x_selstart != x->x_selend)
        {
            x->x_selstart = x->x_selend;
            last_sel = 0;
        }
        else
        {
            // we do this twice and then move to the right
            // as many spots as we had before, this will
            // allow us to go visually below where we used
            // to be in multiline situations (e.g. comments)
            int right = 0;
            if (x->x_selstart > 0 &&
                    (x->x_buf[x->x_selstart] != '\n' ||
                        x->x_buf[x->x_selstart] != '\v'))
            {
                while (x->x_selstart > 0 &&
                        (x->x_buf[x->x_selstart-1] != '\n' &&
                            x->x_buf[x->x_selstart-1] != '\v'))
                {
                    x->x_selstart--;
                    right++;
                }
            }
            //printf("start: right=%d selstart=%d selend=%d\n", right, x->x_selstart, x->x_selend);
            if (x->x_selend < x->x_bufsize &&
                    (x->x_buf[x->x_selend] == '\n' ||
                        x->x_buf[x->x_selend] == '\v'))
            {
                //printf("found break\n");
                u8_inc(x->x_buf, &x->x_selend);
                right--;
            }
            else while (x->x_selend < x->x_bufsize &&
                (x->x_buf[x->x_selend] != '\n' && x->x_buf[x->x_selend] != '\v'))
            {
                u8_inc(x->x_buf, &x->x_selend);
            }
            //printf("first linebreak: selend=%d\n", x->x_selend);
            if (x->x_selend+1 < x->x_bufsize)
            {
                u8_inc(x->x_buf, &x->x_selend);
            }
            //printf("increase by 1: selend=%d\n", x->x_selend);
            while (right > 0 && x->x_selend < x->x_bufsize &&
                (x->x_buf[x->x_selend] != '\n' &&
                    x->x_buf[x->x_selend] != '\v'))
            {
                u8_inc(x->x_buf, &x->x_selend);
                right--;
            }
            //printf("final: selend=%d\n", x->x_selend);
            x->x_selstart = x->x_selend;
            last_sel = 0;
        }
    }
    else if (!strcmp(keysym->s_name, "Home"))
    {
        if (x->x_selstart)
            u8_dec(x->x_buf, &x->x_selstart);
        while (x->x_selstart > 0 && x->x_buf[x->x_selstart] != '\n')
            u8_dec(x->x_buf, &x->x_selstart);
        x->x_selend = x->x_selstart;
        last_sel = 0;
    }
    else if (!strcmp(keysym->s_name, "ShiftHome"))
    {
        if (x->x_selstart)
        {
            if (last_sel == 2)
                x->x_selend = x->x_selstart;
            u8_dec(x->x_buf, &x->x_selstart);
            last_sel = 1;
        }
        while (x->x_selstart > 0 && x->x_buf[x->x_selstart] != '\n')
            u8_dec(x->x_buf, &x->x_selstart);
        //x->x_selend = x->x_selstart;
        //last_sel = 1;
    }
    else if (!strcmp(keysym->s_name, "End"))
    {
        while (x->x_selend < x->x_bufsize &&
            x->x_buf[x->x_selend] != '\n')
            u8_inc(x->x_buf, &x->x_selend);
        if (x->x_selend < x->x_bufsize)
            u8_inc(x->x_buf, &x->x_selend);
        x->x_selstart = x->x_selend;
        last_sel = 0;
    }
    else if (!strcmp(keysym->s_name, "ShiftEnd"))
    {
        if (last_sel == 1)
            x->x_selstart = x->x_selend;
        while (x->x_selend < x->x_bufsize &&
            x->x_buf[x->x_selend] != '\n')
            u8_inc(x->x_buf, &x->x_selend);
        if (x->x_selend < x->x_bufsize)
        {
            u8_inc(x->x_buf, &x->x_selend);
        }
        //x->x_selstart = x->x_selend;
        last_sel = 2;
    }
    else if (!strcmp(keysym->s_name, "CtrlLeft"))
    {
        /* first find first non-space char going back */
        while (x->x_selstart > 0 &&
            rtext_compare_special_chars(x->x_buf[x->x_selstart-1]))
        {
            u8_dec(x->x_buf, &x->x_selstart);
        }
        /* now go back until you find another space or
           the beginning of the buffer */
        while (x->x_selstart > 0 &&
          !rtext_compare_special_chars(x->x_buf[x->x_selstart-1]))
        {
            u8_dec(x->x_buf, &x->x_selstart);
        }
        if (x->x_buf[x->x_selstart+1] == ' ' &&
            x->x_buf[x->x_selstart] == ' ')
        {
            u8_inc(x->x_buf, &x->x_selstart);
        }
        x->x_selend = x->x_selstart;
    }
    else if (!strcmp(keysym->s_name, "CtrlRight"))
    {
        /* now go forward until you find another space
           or the end of the buffer */
        if (x->x_selend < x->x_bufsize - 1)
            u8_inc(x->x_buf, &x->x_selend);
        while (x->x_selend < x->x_bufsize &&
          !rtext_compare_special_chars(x->x_buf[x->x_selend]))
            u8_inc(x->x_buf, &x->x_selend);
        x->x_selstart = x->x_selend;
    }
    else if (!strcmp(keysym->s_name, "CtrlShiftLeft"))
    {
        int swap = 0;
        int *target;
        if (!last_sel) last_sel = 1;
        if (last_sel == 2 && x->x_selend > x->x_selstart)
            target = &x->x_selend;
        else
        {
            last_sel = 1;
            target = &x->x_selstart;
        }
        /* first find first non-space char going back */
        while (*target > 0 &&
            rtext_compare_special_chars(x->x_buf[*target-1]))
        {
            u8_dec(x->x_buf, target);
        }
        /* now go back until you find another space or
           the beginning of the buffer */
        while (*target > 0 &&
            !rtext_compare_special_chars(x->x_buf[*target-1]))
        {
            u8_dec(x->x_buf, target);
        }
        if (x->x_buf[*target+1] == ' ' &&
            x->x_buf[x->x_selstart] == ' ')
        {
            u8_inc(x->x_buf, target);
        }
        if (x->x_selstart > x->x_selend)
        {
            swap = x->x_selend;
            x->x_selend = x->x_selstart;
            x->x_selstart = swap;
            last_sel = 1;
        }
    }
    else if (!strcmp(keysym->s_name, "CtrlShiftRight"))
    {
        int swap = 0;
        int *target;
        if (!last_sel) last_sel = 2;
        if (last_sel == 1 && x->x_selstart < x->x_selend)
            target = &x->x_selstart;
        else
        {
            last_sel = 2;
            target = &x->x_selend;
        }
        /* now go forward until you find another space or
           the end of the buffer */
        if (*target < x->x_bufsize - 1)
        {
            u8_inc(x->x_buf, target);
        }
        while (*target < x->x_bufsize &&
            !rtext_compare_special_chars(x->x_buf[*target]))
        {
            u8_inc(x->x_buf, target);
        }
        if (x->x_selstart > x->x_selend)
        {
            swap = x->x_selend;
            x->x_selend = x->x_selstart;
            x->x_selstart = swap;
            last_sel = 2;
        }
    }
    rtext_senditup(x, SEND_UPDATE, &w, &h, &indx);
}

void rtext_mouse(t_rtext *x, int xval, int yval, int flag)
{
    int w = xval, h = yval, indx;
    rtext_senditup(x, SEND_CHECK, &w, &h, &indx);
    if (flag == RTEXT_DOWN)
    {
        x->x_dragfrom = x->x_selstart = x->x_selend = indx;
    }
    else if (flag == RTEXT_DBL)
    {
        int whereseparator, newseparator;
        x->x_dragfrom = -1;
        whereseparator = 0;
        if ((newseparator = lastone(x->x_buf, ' ', indx)) > whereseparator)
            whereseparator = newseparator+1;
        if ((newseparator = lastone(x->x_buf, '\n', indx)) > whereseparator)
            whereseparator = newseparator+1;
        if ((newseparator = lastone(x->x_buf, ';', indx)) > whereseparator)
            whereseparator = newseparator+1;
        if ((newseparator = lastone(x->x_buf, ',', indx)) > whereseparator)
            whereseparator = newseparator+1;
        x->x_selstart = whereseparator;
        
        whereseparator = x->x_bufsize - indx;
        if ((newseparator =
            firstone(x->x_buf+indx, ' ', x->x_bufsize - indx)) >= 0 &&
                newseparator < whereseparator)
                    whereseparator = newseparator;
        if ((newseparator =
            firstone(x->x_buf+indx, '\n', x->x_bufsize - indx)) >= 0 &&
                newseparator < whereseparator)
                    whereseparator = newseparator;
        if ((newseparator =
            firstone(x->x_buf+indx, ';', x->x_bufsize - indx)) >= 0 &&
                newseparator < whereseparator)
                    whereseparator = newseparator;
        if ((newseparator =
            firstone(x->x_buf+indx, ',', x->x_bufsize - indx)) >= 0 &&
                newseparator < whereseparator)
                    whereseparator = newseparator;
        x->x_selend = indx + whereseparator;
    }
    else if (flag == RTEXT_SHIFT)
    {
        if (indx * 2 > x->x_selstart + x->x_selend)
            x->x_dragfrom = x->x_selstart, x->x_selend = indx;
        else
            x->x_dragfrom = x->x_selend, x->x_selstart = indx;
    }
    else if (flag == RTEXT_DRAG)
    {
        if (x->x_dragfrom < 0)
            return;
        x->x_selstart = (x->x_dragfrom < indx ? x->x_dragfrom : indx);
        x->x_selend = (x->x_dragfrom > indx ? x->x_dragfrom : indx);
    }
    rtext_senditup(x, SEND_UPDATE, &w, &h, &indx);
}
