/* launcher.c - X11 application launcher */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include <err.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* fork, execvp, pledge (OpenBSD) */
#include <sys/types.h>  /* pid_t */

#include "config.h"

static unsigned long
parse_color(Display *dpy, int screen, const char *color)
{
	XColor xcolor;
	Colormap cmap;

	cmap = DefaultColormap(dpy, screen);
	if (!XParseColor(dpy, cmap, color, &xcolor)) {
		warnx("invalid color: %s", color);
		return BlackPixel(dpy, screen);
	}
	if (!XAllocColor(dpy, cmap, &xcolor)) {
		warnx("cannot allocate color: %s", color);
		return BlackPixel(dpy, screen);
	}
	return xcolor.pixel;
}

static int
draw(Display *dpy, XftDraw *draw, XftFont *font, XftColor *xft_fg,
    const char *input, int cursor, int inlen, int scroll_x)
{
	XGlyphInfo ext_full, ext_cursor;
	int x, y, text_w, avail_w;

	if (draw == NULL)
		return scroll_x;

	XClearWindow(dpy, XftDrawDrawable(draw));

	x = 10;
	y = (win_height + font->ascent - font->descent) / 2;

	/* total width of full string */
	if (inlen > 0) {
		XftTextExtentsUtf8(dpy, font, (const XftChar8 *)input,
		    inlen, &ext_full);
		text_w = ext_full.xOff;
	} else {
		memset(&ext_full, 0, sizeof(ext_full));
		text_w = 0;
	}

	/* width up to cursor */
	if (cursor > 0) {
		XftTextExtentsUtf8(dpy, font, (const XftChar8 *)input,
		    cursor, &ext_cursor);
	} else {
		memset(&ext_cursor, 0, sizeof(ext_cursor));
	}

	avail_w = (int)win_width - 20; /* padding left+right */

	/* adjust scroll to keep cursor visible */
	if (ext_cursor.xOff - scroll_x > avail_w) {
		scroll_x = ext_cursor.xOff - avail_w;
	} else if (ext_cursor.xOff - scroll_x < 0) {
		scroll_x = ext_cursor.xOff;
	}
	if (scroll_x < 0)
		scroll_x = 0;
	if (scroll_x > text_w)
		scroll_x = text_w;

	/* draw text shifted left by scroll_x */
	if (inlen > 0) {
		XftDrawStringUtf8(draw, xft_fg, font, x - scroll_x, y,
		    (const XftChar8 *)input, inlen);
	}

	/* cursor position (after scroll) */
	x = 10 + ext_cursor.xOff - scroll_x;
	XftDrawRect(draw, xft_fg, x, y - font->ascent, 2, font->height);

	return scroll_x;
}

static void
run_command(const char *input)
{
	pid_t pid;

	if (input == NULL || *input == '\0')
		return;

	pid = fork();
	if (pid < 0) {
		warn("fork");
		return;
	}
	if (pid == 0) {
		/* child */
		setsid();
		execl("/bin/sh", "sh", "-c", input, (char *)NULL);
		/* only reached on error */
		warn("execl /bin/sh");
		_exit(127);
	}

	/* parent: don't wait */
}


int
main(void)
{
	Display *dpy;
	Window win;
	Visual *vis;
	Colormap cmap;
	XEvent ev;
	XSizeHints hints;
	XftFont *font;
	XftColor xft_fg;
	XftDraw *xftdraw;
	KeySym key;
	char input[input_max];
	char buf[32];
	int screen, x, y;
	int running, focused, cursor, inlen, scroll_x;
	unsigned long bg, border;
	Atom wm_delete_window;

	/* ignore SIGCHLD to avoid zombies from launched processes */
	signal(SIGCHLD, SIG_IGN);

	/* locale not strictly needed here, but harmless */
	setlocale(LC_CTYPE, "");

	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "cannot open display");

#ifdef __OpenBSD__
	/* X11 client that can fork/exec */
	if (pledge("stdio rpath inet unix proc exec", NULL) == -1)
		err(1, "pledge");
#endif

	screen = DefaultScreen(dpy);
	vis = DefaultVisual(dpy, screen);
	cmap = DefaultColormap(dpy, screen);

	bg = parse_color(dpy, screen, bg_color);
	border = parse_color(dpy, screen, fg_color);

	/* load font and color */
	font = XftFontOpenName(dpy, screen, font_name);
	if (font == NULL)
		errx(1, "cannot load font: %s", font_name);

	if (!XftColorAllocName(dpy, vis, cmap, fg_color, &xft_fg)) {
		XftFontClose(dpy, font);
		errx(1, "cannot allocate xft color: %s", fg_color);
	}

	/* center window on screen */
	x = (DisplayWidth(dpy, screen) - (int)win_width) / 2;
	y = (DisplayHeight(dpy, screen) - (int)win_height) / 2;

	win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
	    x, y, win_width, win_height, 2, border, bg);

	/* tell WM to respect our position */
	memset(&hints, 0, sizeof(hints));
	hints.flags = PPosition | USPosition;
	hints.x = x;
	hints.y = y;
	XSetWMNormalHints(dpy, win, &hints);

	/* allow WM close button to work */
	wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete_window, 1);

	XSelectInput(dpy, win,
	    ExposureMask | KeyPressMask | FocusChangeMask);

	XMapWindow(dpy, win);

	xftdraw = XftDrawCreate(dpy, win, vis, cmap);
	if (xftdraw == NULL)
		errx(1, "cannot create XftDraw");

	input[0] = '\0';
	cursor = 0;
	inlen = 0;
	scroll_x = 0;
	running = 1;
	focused = 0;

	while (running) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
			if (!focused) {
				XSetInputFocus(dpy, win,
				    RevertToParent, CurrentTime);
				focused = 1;
			}
			scroll_x = draw(dpy, xftdraw, font, &xft_fg,
			    input, cursor, inlen, scroll_x);
			break;
		case FocusOut:
			/* running = 0; */
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete_window)
				running = 0;
			break;
		case KeyPress:
			key = XLookupKeysym(&ev.xkey, 0);

			if (key == XK_Escape) {
				running = 0;
			} else if (key == XK_Return || key == XK_KP_Enter) {
				if (inlen > 0) {
					input[inlen] = '\0';

					/* still print for debugging / piping */
					printf("%s\n", input);
					fflush(stdout);

					run_command(input);
				}
				running = 0;
			} else if (key == XK_BackSpace) {
				if (cursor > 0) {
					memmove(&input[cursor - 1],
					    &input[cursor],
					    (size_t)(inlen - cursor) + 1);
					cursor--;
					inlen--;
					scroll_x = draw(dpy, xftdraw, font,
					    &xft_fg, input, cursor, inlen,
					    scroll_x);
				}
			} else if (key == XK_Left) {
				if (cursor > 0)
					cursor--;
				scroll_x = draw(dpy, xftdraw, font,
				    &xft_fg, input, cursor, inlen, scroll_x);
			} else if (key == XK_Right) {
				if (cursor < inlen)
					cursor++;
				scroll_x = draw(dpy, xftdraw, font,
				    &xft_fg, input, cursor, inlen, scroll_x);
			} else if (key == XK_Home) {
				if (cursor != 0) {
					cursor = 0;
					scroll_x = draw(dpy, xftdraw, font,
					    &xft_fg, input, cursor, inlen,
					    scroll_x);
				}
			} else if (key == XK_End) {
				if (cursor != inlen) {
					cursor = inlen;
					scroll_x = draw(dpy, xftdraw, font,
					    &xft_fg, input, cursor, inlen,
					    scroll_x);
				}
			} else if ((ev.xkey.state & ControlMask) && key == XK_u) {
				/* Ctrl-U: clear line */
				cursor = 0;
				inlen = 0;
				input[0] = '\0';
				scroll_x = 0;
				scroll_x = draw(dpy, xftdraw, font,
				    &xft_fg, input, cursor, inlen, scroll_x);
			} else {
				int len = XLookupString(&ev.xkey, buf,
				    (int)sizeof(buf), NULL, NULL);
				if (len > 0 &&
				    inlen + len < (int)input_max - 1) {
					memmove(&input[cursor + len],
					    &input[cursor],
					    (size_t)(inlen - cursor) + 1);
					memcpy(&input[cursor], buf,
					    (size_t)len);
					cursor += len;
					inlen += len;
					scroll_x = draw(dpy, xftdraw, font,
					    &xft_fg, input, cursor, inlen,
					    scroll_x);
				}
			}
			break;
		}
	}

	XftDrawDestroy(xftdraw);
	XftColorFree(dpy, vis, cmap, &xft_fg);
	XftFontClose(dpy, font);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);

	return 0;
}
