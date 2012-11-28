/*
 * See LICENSE file for copyright and license details.
 *
 * To understand tabbed, start reading main().
 */

#include <sys/wait.h>
#include <locale.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>

#include "arg.h"

/* XEMBED messages */
#define XEMBED_EMBEDDED_NOTIFY          0
#define XEMBED_WINDOW_ACTIVATE          1
#define XEMBED_WINDOW_DEACTIVATE        2
#define XEMBED_REQUEST_FOCUS            3
#define XEMBED_FOCUS_IN                 4
#define XEMBED_FOCUS_OUT                5
#define XEMBED_FOCUS_NEXT               6
#define XEMBED_FOCUS_PREV               7
/* 8-9 were used for XEMBED_GRAB_KEY/XEMBED_UNGRAB_KEY */
#define XEMBED_MODALITY_ON              10
#define XEMBED_MODALITY_OFF             11
#define XEMBED_REGISTER_ACCELERATOR     12
#define XEMBED_UNREGISTER_ACCELERATOR   13
#define XEMBED_ACTIVATE_ACCELERATOR     14

/* Details for  XEMBED_FOCUS_IN: */
#define XEMBED_FOCUS_CURRENT            0
#define XEMBED_FOCUS_FIRST              1
#define XEMBED_FOCUS_LAST               2

/* Macros */
#define MAX(a, b)                ((a) > (b) ? (a) : (b))
#define MIN(a, b)                ((a) < (b) ? (a) : (b))
#define LENGTH(x)                (sizeof((x)) / sizeof(*(x)))
#define CLEANMASK(mask)          (mask & ~(numlockmask|LockMask))
#define TEXTW(x)                 (textnw(x, strlen(x)) + dc.font.height)

enum { ColFG, ColBG, ColLast };                         /* color */
enum { WMProtocols, WMDelete, WMName, XEmbed, WMLast }; /* default atoms */

typedef union {
	int i;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	int x, y, w, h;
	unsigned long norm[ColLast];
	unsigned long sel[ColLast];
	Drawable drawable;
	GC gc;
	struct {
		int ascent;
		int descent;
		int height;
		XFontSet set;
		XFontStruct *xfont;
	} font;
} DC; /* draw context */

typedef struct Client {
	char name[256];
	Window win;
	int tabx;
	Bool mapped;
	Bool closed;
} Client;

/* function declarations */
static void buttonpress(const XEvent *e);
static void cleanup(void);
static void clientmessage(const XEvent *e);
static void configurenotify(const XEvent *e);
static void configurerequest(const XEvent *e);
static void createnotify(const XEvent *e);
static void destroynotify(const XEvent *e);
static void die(const char *errstr, ...);
static void drawbar(void);
static void drawtext(const char *text, unsigned long col[ColLast]);
static void *emallocz(size_t size);
static void *erealloc(void *o, size_t size);
static void expose(const XEvent *e);
static void focus(int c);
static void focusin(const XEvent *e);
static void focusonce(const Arg *arg);
static int getclient(Window w);
static unsigned long getcolor(const char *colstr);
static int getfirsttab(void);
static Bool gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void initfont(const char *fontstr);
static Bool isprotodel(int c);
static void keypress(const XEvent *e);
static void killclient(const Arg *arg);
static void manage(Window win);
static void maprequest(const XEvent *e);
static void move(const Arg *arg);
static void movetab(const Arg *arg);
static void propertynotify(const XEvent *e);
static void resize(int c, int w, int h);
static void rotate(const Arg *arg);
static void run(void);
static void sendxembed(int c, long msg, long detail, long d1, long d2);
static void setup(void);
static void setcmd(int argc, char *argv[], int);
static void sigchld(int unused);
static void spawn(const Arg *arg);
static int textnw(const char *text, unsigned int len);
static void unmanage(int c);
static void updatenumlockmask(void);
static void updatetitle(int c);
static int xerror(Display *dpy, XErrorEvent *ee);

/* variables */
static int screen;
static void (*handler[LASTEvent]) (const XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureNotify] = configurenotify,
	[ConfigureRequest] = configurerequest,
	[CreateNotify] = createnotify,
	[DestroyNotify] = destroynotify,
	[Expose] = expose,
	[FocusIn] = focusin,
	[KeyPress] = keypress,
	[MapRequest] = maprequest,
	[PropertyNotify] = propertynotify,
};
static int bh, wx, wy, ww, wh;
static unsigned int numlockmask = 0;
static Bool running = True, nextfocus, doinitspawn = True, fillagain = False;
static Display *dpy;
static DC dc;
static Atom wmatom[WMLast];
static Window root, win;
static Client **clients = NULL;
static int nclients = 0, sel = -1, lastsel = -1;
static int (*xerrorxlib)(Display *, XErrorEvent *);
static char winid[64];
static char **cmd = NULL;
static char *wmname = "tabbed";

char *argv0;

/* configuration, allows nested code to access above variables */
#include "config.h"

void
buttonpress(const XEvent *e) {
	const XButtonPressedEvent *ev = &e->xbutton;
	int i;
	Arg arg;

	if(getfirsttab() != 0 && ev->x < TEXTW(before))
		return;

	for(i = 0; i < nclients; i++) {
		if(clients[i]->tabx > ev->x) {
			switch(ev->button) {
			case Button1:
				focus(i);
				break;
			case Button2:
				focus(i);
				killclient(NULL);
				break;
			case Button4:
			case Button5:
				arg.i = ev->button == Button4 ? -1 : 1;
				rotate(&arg);
				break;
			}
			break;
		}
	}
}

void
cleanup(void) {
	int i;

	for(i = 0; i < nclients; i++) {
		focus(i);
		killclient(NULL);
		killclient(NULL);
		XReparentWindow(dpy, clients[i]->win, root, 0, 0);
		unmanage(i);
	}
	free(clients);
	clients = NULL;

	if(dc.font.set) {
		XFreeFontSet(dpy, dc.font.set);
	} else {
		XFreeFont(dpy, dc.font.xfont);
	}

	XFreePixmap(dpy, dc.drawable);
	XFreeGC(dpy, dc.gc);
	XDestroyWindow(dpy, win);
	XSync(dpy, False);
	free(cmd);
}

void
clientmessage(const XEvent *e) {
	const XClientMessageEvent *ev = &e->xclient;

	if(ev->message_type == wmatom[WMProtocols]
			&& ev->data.l[0] == wmatom[WMDelete]) {
		running = False;
	}
}

void
configurenotify(const XEvent *e) {
	const XConfigureEvent *ev = &e->xconfigure;

	if(ev->window == win && (ev->width != ww || ev->height != wh)) {
		ww = ev->width;
		wh = ev->height;
		XFreePixmap(dpy, dc.drawable);
		dc.drawable = XCreatePixmap(dpy, root, ww, wh,
				DefaultDepth(dpy, screen));
		if(sel > -1)
			resize(sel, ww, wh - bh);
		XSync(dpy, False);
	}
}

void
configurerequest(const XEvent *e) {
	const XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;
	int c;

	if((c = getclient(ev->window)) > -1) {
		wc.x = 0;
		wc.y = bh;
		wc.width = ww;
		wc.height = wh - bh;
		wc.border_width = 0;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, clients[c]->win, ev->value_mask, &wc);
	}
}

void
createnotify(const XEvent *e) {
	const XCreateWindowEvent *ev = &e->xcreatewindow;

	if(ev->window != win && getclient(ev->window) < 0)
		manage(ev->window);
}

void
destroynotify(const XEvent *e) {
	const XDestroyWindowEvent *ev = &e->xdestroywindow;
	int c;

	if((c = getclient(ev->window)) > -1)
		unmanage(c);
}

void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void
drawbar(void) {
	unsigned long *col;
	int c, fc, width, n = 0;
	char *name = NULL;

	if(nclients == 0) {
		dc.x = 0;
		dc.w = ww;
		XFetchName(dpy, win, &name);
		drawtext(name ? name : "", dc.norm);
		XCopyArea(dpy, dc.drawable, win, dc.gc, 0, 0, ww, bh, 0, 0);
		XSync(dpy, False);

		return;
	}

	width = ww;
	clients[nclients-1]->tabx = -1;
	fc = getfirsttab();
	if(fc > -1)
		n = nclients - fc;

	if((n * tabwidth) > width) {
		dc.w = TEXTW(after);
		dc.x = width - dc.w;
		drawtext(after, dc.sel);
		width -= dc.w;
	}
	dc.x = 0;

	if(fc > 0) {
		dc.w = TEXTW(before);
		drawtext(before, dc.sel);
		dc.x += dc.w;
		width -= dc.w;
	}

	for(c = (fc > 0)? fc : 0; c < nclients && dc.x < width; c++) {
		dc.w = tabwidth;
		if(c == sel) {
			col = dc.sel;
			if((n * tabwidth) > width) {
				dc.w += width % tabwidth;
			} else {
				dc.w = width - (n - 1) * tabwidth;
			}
		} else {
			col = dc.norm;
		}
		drawtext(clients[c]->name, col);
		dc.x += dc.w;
		clients[c]->tabx = dc.x;
	}
	XCopyArea(dpy, dc.drawable, win, dc.gc, 0, 0, ww, bh, 0, 0);
	XSync(dpy, False);
}

void
drawtext(const char *text, unsigned long col[ColLast]) {
	int i, x, y, h, len, olen;
	char buf[256];
	XRectangle r = { dc.x, dc.y, dc.w, dc.h };

	XSetForeground(dpy, dc.gc, col[ColBG]);
	XFillRectangles(dpy, dc.drawable, dc.gc, &r, 1);
	if(!text)
		return;

	olen = strlen(text);
	h = dc.font.ascent + dc.font.descent;
	y = dc.y + (dc.h / 2) - (h / 2) + dc.font.ascent;
	x = dc.x + (h / 2);

	/* shorten text if necessary */
	for(len = MIN(olen, sizeof(buf));
			len && textnw(text, len) > dc.w - h; len--);
	if(!len)
		return;

	memcpy(buf, text, len);
	if(len < olen) {
		for(i = len; i && i > len - 3; buf[--i] = '.');
	}

	XSetForeground(dpy, dc.gc, col[ColFG]);
	if(dc.font.set) {
		XmbDrawString(dpy, dc.drawable, dc.font.set,
				dc.gc, x, y, buf, len);
	} else {
		XDrawString(dpy, dc.drawable, dc.gc, x, y, buf, len);
	}
}

void *
emallocz(size_t size) {
	void *p;

	if(!(p = calloc(1, size)))
		die("tabbed: cannot malloc\n");
	return p;
}

void *
erealloc(void *o, size_t size) {
	void *p;

	if(!(p = realloc(o, size)))
		die("tabbed: cannot realloc\n");
	return p;
}

void
expose(const XEvent *e) {
	const XExposeEvent *ev = &e->xexpose;

	if(ev->count == 0 && win == ev->window)
		drawbar();
}

void
focus(int c) {
	char buf[BUFSIZ] = "tabbed-"VERSION" ::";
	size_t i, n;

	/* If c, sel and clients are -1, raise tabbed-win itself */
	if(nclients == 0) {
		for(i = 0, n = strlen(buf); cmd[i] && n < sizeof(buf); i++)
			n += snprintf(&buf[n], sizeof(buf) - n, " %s", cmd[i]);

		XStoreName(dpy, win, buf);
		XRaiseWindow(dpy, win);

		return;
	}

	if(c < 0 || c >= nclients)
		return;

	resize(c, ww, wh - bh);
	XRaiseWindow(dpy, clients[c]->win);
	XSetInputFocus(dpy, clients[c]->win, RevertToParent, CurrentTime);
	sendxembed(c, XEMBED_FOCUS_IN, XEMBED_FOCUS_CURRENT, 0, 0);
	sendxembed(c, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
	XStoreName(dpy, win, clients[c]->name);

	if(sel != c)
		lastsel = sel;
	sel = c;

	drawbar();
}

void
focusin(const XEvent *e) {
	const XFocusChangeEvent *ev = &e->xfocus;
	int dummy;
	Window focused;

	if(ev->mode != NotifyUngrab) {
		XGetInputFocus(dpy, &focused, &dummy);
		if(focused == win)
			focus(sel);
	}
}

void
focusonce(const Arg *arg) {
	nextfocus = True;
}

int
getclient(Window w) {
	int i;

	for(i = 0; i < nclients; i++) {
		if(clients[i]->win == w)
			return i;
	}

	return -1;
}

unsigned long
getcolor(const char *colstr) {
	Colormap cmap = DefaultColormap(dpy, screen);
	XColor color;

	if(!XAllocNamedColor(dpy, cmap, colstr, &color, &color))
		die("tabbed: cannot allocate color '%s'\n", colstr);

	return color.pixel;
}

int
getfirsttab(void) {
	int c, n, fc;

	if(sel < 0)
		return -1;

	c = sel;
	fc = 0;
	n = nclients;
	if((n * tabwidth) > ww) {
		for(; (c * tabwidth) > (ww / 2)
				&& (n * tabwidth) > ww;
				c--, n--, fc++);
	}

	return fc;
}

Bool
gettextprop(Window w, Atom atom, char *text, unsigned int size) {
	char **list = NULL;
	int n;
	XTextProperty name;

	if(!text || size == 0)
		return False;

	text[0] = '\0';
	XGetTextProperty(dpy, w, &name, atom);
	if(!name.nitems)
		return False;

	if(name.encoding == XA_STRING) {
		strncpy(text, (char *)name.value, size - 1);
	} else {
		if(XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success
				&& n > 0 && *list) {
			strncpy(text, *list, size - 1);
			XFreeStringList(list);
		}
	}
	text[size - 1] = '\0';
	XFree(name.value);

	return True;
}

void
initfont(const char *fontstr) {
	char *def, **missing, **font_names;
	int i, n;
	XFontStruct **xfonts;

	missing = NULL;
	if(dc.font.set)
		XFreeFontSet(dpy, dc.font.set);

	dc.font.set = XCreateFontSet(dpy, fontstr, &missing, &n, &def);
	if(missing) {
		while(n--)
			fprintf(stderr, "tabbed: missing fontset: %s\n", missing[n]);
		XFreeStringList(missing);
	}

	if(dc.font.set) {
		dc.font.ascent = dc.font.descent = 0;
		n = XFontsOfFontSet(dc.font.set, &xfonts, &font_names);
		for(i = 0, dc.font.ascent = 0, dc.font.descent = 0; i < n; i++) {
			dc.font.ascent = MAX(dc.font.ascent, (*xfonts)->ascent);
			dc.font.descent = MAX(dc.font.descent,(*xfonts)->descent);
			xfonts++;
		}
	} else {
		if(dc.font.xfont)
			XFreeFont(dpy, dc.font.xfont);
		dc.font.xfont = NULL;
		if(!(dc.font.xfont = XLoadQueryFont(dpy, fontstr))
				&& !(dc.font.xfont = XLoadQueryFont(dpy, "fixed"))) {
			die("tabbed: cannot load font: '%s'\n", fontstr);
		}

		dc.font.ascent = dc.font.xfont->ascent;
		dc.font.descent = dc.font.xfont->descent;
	}
	dc.font.height = dc.font.ascent + dc.font.descent;
}

Bool
isprotodel(int c) {
	int i, n;
	Atom *protocols;
	Bool ret = False;

	if(XGetWMProtocols(dpy, clients[c]->win, &protocols, &n)) {
		for(i = 0; !ret && i < n; i++) {
			if(protocols[i] == wmatom[WMDelete])
				ret = True;
		}
		XFree(protocols);
	}

	return ret;
}

void
keypress(const XEvent *e) {
	const XKeyEvent *ev = &e->xkey;
	unsigned int i;
	KeySym keysym;

	keysym = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, 0);
	for(i = 0; i < LENGTH(keys); i++) {
		if(keysym == keys[i].keysym
				&& CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)
				&& keys[i].func) {
			keys[i].func(&(keys[i].arg));
		}
	}
}

void
killclient(const Arg *arg) {
	XEvent ev;

	if(sel < 0)
		return;

	if(isprotodel(sel) && !clients[sel]->closed) {
		ev.type = ClientMessage;
		ev.xclient.window = clients[sel]->win;
		ev.xclient.message_type = wmatom[WMProtocols];
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = wmatom[WMDelete];
		ev.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, clients[sel]->win, False, NoEventMask, &ev);
		clients[sel]->closed = True;
	} else {
		XKillClient(dpy, clients[sel]->win);
	}
}

void
manage(Window w) {
	updatenumlockmask();
	{
		int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask,
			numlockmask|LockMask };
		KeyCode code;
		Client *c;
		XEvent e;

		XWithdrawWindow(dpy, w, 0);
		XReparentWindow(dpy, w, win, 0, bh);
		XSelectInput(dpy, w, PropertyChangeMask
				|StructureNotifyMask|EnterWindowMask);
		XSync(dpy, False);

		for(i = 0; i < LENGTH(keys); i++) {
			if((code = XKeysymToKeycode(dpy, keys[i].keysym))) {
				for(j = 0; j < LENGTH(modifiers); j++) {
					XGrabKey(dpy, code, keys[i].mod
							| modifiers[j], w,
						 True, GrabModeAsync,
						 GrabModeAsync);
				}
			}
		}

		c = emallocz(sizeof(*c));
		c->win = w;

		nclients++;
		clients = erealloc(clients, sizeof(Client *) * nclients);
		if(nclients > 1) {
			memmove(&clients[1], &clients[0],
					sizeof(Client *) * (nclients - 1));
		}
		clients[0] = c;

		updatetitle(0);
		XLowerWindow(dpy, w);
		XMapWindow(dpy, w);

		e.xclient.window = w;
		e.xclient.type = ClientMessage;
		e.xclient.message_type = wmatom[XEmbed];
		e.xclient.format = 32;
		e.xclient.data.l[0] = CurrentTime;
		e.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
		e.xclient.data.l[2] = 0;
		e.xclient.data.l[3] = win;
		e.xclient.data.l[4] = 0;
		XSendEvent(dpy, root, False, NoEventMask, &e);

		XSync(dpy, False);
		focus((nextfocus)? 0 : ((sel < 0)? 0 : sel));
		nextfocus = foreground;
	}
}

void
maprequest(const XEvent *e) {
	const XMapRequestEvent *ev = &e->xmaprequest;

	if(getclient(ev->window) > -1)
		manage(ev->window);
}

void
move(const Arg *arg) {
	if(arg->i >= 0 && arg->i < nclients)
		focus(arg->i);
}

void
movetab(const Arg *arg) {
	int c;
	Client *new;

	if(sel < 0 || (arg->i == 0))
		return;

	c = sel + arg->i;
	while(c >= nclients)
		c -= nclients;
	while(c < 0)
		c += nclients;

	new = clients[c];
	clients[c] = clients[sel];
	clients[sel] = new;

	sel = c;

	drawbar();
}

void
propertynotify(const XEvent *e) {
	const XPropertyEvent *ev = &e->xproperty;
	int c;

	if(ev->state != PropertyDelete && ev->atom == XA_WM_NAME
			&& (c = getclient(ev->window)) > -1) {
		updatetitle(c);
	}
}

void
resize(int c, int w, int h) {
	XConfigureEvent ce;
	XWindowChanges wc;

	ce.x = 0;
	ce.y = bh;
	ce.width = wc.width = w;
	ce.height = wc.height = h;
	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = clients[c]->win;
	ce.window = clients[c]->win;
	ce.above = None;
	ce.override_redirect = False;
	ce.border_width = 0;

	XConfigureWindow(dpy, clients[c]->win, CWWidth|CWHeight, &wc);
	XSendEvent(dpy, clients[c]->win, False, StructureNotifyMask,
			(XEvent *)&ce);
}

void
rotate(const Arg *arg) {
	int nsel = -1;

	if(sel < 0)
		return;

	if(arg->i == 0) {
		if(lastsel > -1)
			focus(lastsel);
	} else if(sel > -1) {
		/* Rotating in an arg->i step around the clients. */
		nsel = sel + arg->i;
		while(nsel >= nclients)
			nsel -= nclients;
		while(nsel < 0)
			nsel += nclients;
		focus(nsel);
	}
}

void
run(void) {
	XEvent ev;

	/* main event loop */
	XSync(dpy, False);
	drawbar();
	if(doinitspawn == True)
		spawn(NULL);

	while(running) {
		XNextEvent(dpy, &ev);
		if(handler[ev.type])
			(handler[ev.type])(&ev); /* call handler */
	}
}

void
sendxembed(int c, long msg, long detail, long d1, long d2) {
	XEvent e = { 0 };

	e.xclient.window = clients[c]->win;
	e.xclient.type = ClientMessage;
	e.xclient.message_type = wmatom[XEmbed];
	e.xclient.format = 32;
	e.xclient.data.l[0] = CurrentTime;
	e.xclient.data.l[1] = msg;
	e.xclient.data.l[2] = detail;
	e.xclient.data.l[3] = d1;
	e.xclient.data.l[4] = d2;
	XSendEvent(dpy, clients[c]->win, False, NoEventMask, &e);
}

void
setcmd(int argc, char *argv[], int replace) {
	int i;

	cmd = emallocz((argc+2) * sizeof(*cmd));
	for(i = 0; i < argc; i++)
		cmd[i] = argv[i];
	cmd[(replace > 0)? replace : argc] = winid;
	cmd[argc + !(replace > 0)] = NULL;
}

void
setup(void) {
	/* clean up any zombies immediately */
	sigchld(0);

	/* init screen */
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	initfont(font);
	bh = dc.h = dc.font.height + 2;

	/* init atoms */
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[XEmbed] = XInternAtom(dpy, "_XEMBED", False);
	wmatom[WMName] = XInternAtom(dpy, "_NET_WM_NAME", False);

	/* init appearance */
	wx = 0;
	wy = 0;
	ww = 800;
	wh = 600;

	dc.norm[ColBG] = getcolor(normbgcolor);
	dc.norm[ColFG] = getcolor(normfgcolor);
	dc.sel[ColBG] = getcolor(selbgcolor);
	dc.sel[ColFG] = getcolor(selfgcolor);
	dc.drawable = XCreatePixmap(dpy, root, ww, wh,
			DefaultDepth(dpy, screen));
	dc.gc = XCreateGC(dpy, root, 0, 0);
	if(!dc.font.set)
		XSetFont(dpy, dc.gc, dc.font.xfont->fid);

	win = XCreateSimpleWindow(dpy, root, wx, wy, ww, wh, 0,
			dc.norm[ColFG], dc.norm[ColBG]);
	XMapRaised(dpy, win);
	XSelectInput(dpy, win, SubstructureNotifyMask|FocusChangeMask|
			ButtonPressMask|ExposureMask|KeyPressMask|
			StructureNotifyMask|SubstructureRedirectMask);
	xerrorxlib = XSetErrorHandler(xerror);

	XClassHint class_hint;
	class_hint.res_name = wmname;
	class_hint.res_class = "tabbed";
	XSetClassHint(dpy, win, &class_hint);

	XSetWMProtocols(dpy, win, &wmatom[WMDelete], 1);

	snprintf(winid, sizeof(winid), "%lu", win);
	nextfocus = foreground;
	focus(-1);
}

void
sigchld(int unused) {
	if(signal(SIGCHLD, sigchld) == SIG_ERR)
		die("tabbed: cannot install SIGCHLD handler");

	while(0 < waitpid(-1, NULL, WNOHANG));
}

void
spawn(const Arg *arg) {
	if(fork() == 0) {
		if(dpy)
			close(ConnectionNumber(dpy));

		setsid();
		if(arg && arg->v) {
			execvp(((char **)arg->v)[0], (char **)arg->v);
			fprintf(stderr, "tabbed: execvp %s",
					((char **)arg->v)[0]);
		} else {
			execvp(cmd[0], cmd);
			fprintf(stderr, "tabbed: execvp %s", cmd[0]);
		}
		perror(" failed");
		exit(0);
	}
}

int
textnw(const char *text, unsigned int len) {
	XRectangle r;

	if(dc.font.set) {
		XmbTextExtents(dc.font.set, text, len, NULL, &r);

		return r.width;
	}

	return XTextWidth(dc.font.xfont, text, len);
}

void
unmanage(int c) {
	if(c < 0 || c >= nclients) {
		drawbar();
		return;
	}

	if(!nclients) {
		return;
	} else if(c == 0) {
		/* First client. */
		nclients--;
		free(clients[0]);
		memmove(&clients[0], &clients[1], sizeof(Client *) * nclients);
	} else if(c == nclients - 1) {
		/* Last client. */
		nclients--;
		free(clients[c]);
		clients = erealloc(clients, sizeof(Client *) * nclients);
	} else {
		/* Somewhere inbetween. */
		free(clients[c]);
		memmove(&clients[c], &clients[c+1],
				sizeof(Client *) * (nclients - (c + 1)));
		nclients--;
	}

	if(c == lastsel) {
		lastsel = -1;
	} else if(lastsel > c) {
		lastsel--;
	}

	if(sel > c && c > 0) {
		sel--;
		lastsel = -1;
	}
	if(c == nclients && nclients > 0)
		sel = nclients - 1;

	if(lastsel > -1) {
		focus(lastsel);
	} else {
		focus(sel);
	}

	if(nclients == 0 && fillagain)
		spawn(NULL);

	drawbar();
	XSync(dpy, False);
}

void
updatenumlockmask(void) {
	unsigned int i, j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap = XGetModifierMapping(dpy);
	for(i = 0; i < 8; i++) {
		for(j = 0; j < modmap->max_keypermod; j++) {
			if(modmap->modifiermap[i * modmap->max_keypermod + j]
					== XKeysymToKeycode(dpy,
						XK_Num_Lock)) {
				numlockmask = (1 << i);
			}
		}
	}
	XFreeModifiermap(modmap);
}

void
updatetitle(int c) {
	if(!gettextprop(clients[c]->win, wmatom[WMName],
				clients[c]->name, sizeof(clients[c]->name))) {
		gettextprop(clients[c]->win, XA_WM_NAME,
				clients[c]->name, sizeof(clients[c]->name));
	}
	if(sel == c)
		XStoreName(dpy, win, clients[c]->name);
	drawbar();
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's).  Other types of errors call Xlibs
 * default error handler, which may call exit.  */
int
xerror(Display *dpy, XErrorEvent *ee) {
	if(ee->error_code == BadWindow
			|| (ee->request_code == X_SetInputFocus
				&& ee->error_code == BadMatch)
			|| (ee->request_code == X_PolyText8
				&& ee->error_code == BadDrawable)
			|| (ee->request_code == X_PolyFillRectangle
				&& ee->error_code == BadDrawable)
			|| (ee->request_code == X_PolySegment
				&& ee->error_code == BadDrawable)
			|| (ee->request_code == X_ConfigureWindow
				&& ee->error_code == BadMatch)
			|| (ee->request_code == X_GrabButton
				&& ee->error_code == BadAccess)
			|| (ee->request_code == X_GrabKey
				&& ee->error_code == BadAccess)
			|| (ee->request_code == X_CopyArea
				&& ee->error_code == BadDrawable)) {
		return 0;
	}

	fprintf(stderr, "tabbed: fatal error: request code=%d, error code=%d\n",
			ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee); /* may call exit */
}

char *argv0;

void
usage(void) {
	die("usage: %s [-dfhsv] [-n name] [-r narg] command...\n", argv0);
}

int
main(int argc, char *argv[]) {
	int detach = 0, replace = 0;

	ARGBEGIN {
	case 'd':
		detach = 1;
		break;
	case 'f':
		fillagain = 1;
		break;
	case 'n':
		wmname = EARGF(usage());
		break;
	case 'r':
		replace = atoi(EARGF(usage()));
		break;
	case 's':
		doinitspawn = False;
		break;
	case 'v':
		die("tabbed-"VERSION", © 2009-2012"
			" tabbed engineers, see LICENSE"
			" for details.\n");
	default:
	case 'h':
		usage();
	} ARGEND;

	if(argc < 1) {
		doinitspawn = False;
		fillagain = False;
	}

	setcmd(argc, argv, replace);

	if(!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fprintf(stderr, "tabbed: no locale support\n");
	if(!(dpy = XOpenDisplay(NULL)))
		die("tabbed: cannot open display\n");

	setup();
	printf("0x%lx\n", win);
	fflush(NULL);

	if(detach) {
		if(fork() == 0)
			fclose(stdout);
		else {
			if(dpy)
				close(ConnectionNumber(dpy));
			return EXIT_SUCCESS;
		}
	}

	run();
	cleanup();
	XCloseDisplay(dpy);

	return EXIT_SUCCESS;
}

