// LD_PRELOAD shim: the GameMaker runner resolves keys via XLookupString, which follows the active
// layout, so under "ru" W/A/S/D/Z/X/C become Cyrillic and the game ignores them. Always use group 0.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int XLookupString(XKeyEvent *ev, char *buf, int len, KeySym *ks, XComposeStatus *st)
{
    static int (*real)(XKeyEvent *, char *, int, KeySym *, XComposeStatus *);
    if (!real)
        real = (int (*)(XKeyEvent *, char *, int, KeySym *, XComposeStatus *))dlsym(RTLD_NEXT, "XLookupString");
    XKeyEvent e = *ev;
    e.state &= ~(3u << 13); // XKB group bits
    return real(&e, buf, len, ks, st);
}
