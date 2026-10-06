"""X11 through ctypes (standard library): lrb windows, their titles, keys, close.

For tests that need the window itself (the bar, window shortcuts, the close
button), which DevTools input doesn't reach. Needs an X11 display (XWayland
works) and lrb started with --ozone-platform=x11. Events are sent to lrb's
windows only, never to the focused window.
"""

import ctypes, ctypes.util
import glob
import os
import time

# A terminal session may lack DISPLAY even with a desktop running (e.g.
# GNOME on Wayland, its Xwayland on :0 with an auth file in the runtime
# dir). Set them here, before lrb is started, so it inherits them too.
if not os.environ.get("DISPLAY") and os.path.exists("/tmp/.X11-unix/X0"):
    os.environ["DISPLAY"] = ":0"
    auth = sorted(glob.glob(os.path.join(
        os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"),
        ".mutter-Xwaylandauth.*")))
    if auth and not os.environ.get("XAUTHORITY"):
        os.environ["XAUTHORITY"] = auth[0]
x = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11"))
x.XOpenDisplay.restype = ctypes.c_void_p
x.XDefaultRootWindow.restype = ctypes.c_ulong; x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XInternAtom.restype = ctypes.c_ulong; x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
x.XKeysymToKeycode.restype = ctypes.c_ubyte; x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
class ClassHint(ctypes.Structure): _fields_ = [("res_name", ctypes.c_char_p), ("res_class", ctypes.c_char_p)]
x.XGetClassHint.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ClassHint)]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                         ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
x.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long, ctypes.c_int,
    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]
class ClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("data", ctypes.c_long * 5)]
class KeyEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("root", ctypes.c_ulong), ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("x", ctypes.c_int), ("y", ctypes.c_int), ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
                ("state", ctypes.c_uint), ("keycode", ctypes.c_uint), ("same_screen", ctypes.c_int)]
class ButtonEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("root", ctypes.c_ulong), ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("x", ctypes.c_int), ("y", ctypes.c_int), ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
                ("state", ctypes.c_uint), ("button", ctypes.c_uint), ("same_screen", ctypes.c_int)]
class XEvent(ctypes.Union): _fields_ = [("xclient", ClientMessage), ("xkey", KeyEvent), ("xbutton", ButtonEvent), ("pad", ctypes.c_long * 24)]
x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(XEvent)]
x.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
d = x.XOpenDisplay(None)
if not d:
    raise RuntimeError(f"no X11 display (DISPLAY={os.environ.get('DISPLAY')!r}): the X11 "
                       "tests need a desktop session (XWayland works)")
root = x.XDefaultRootWindow(d)
def title(w):
    utf8 = x.XInternAtom(d, b"UTF8_STRING", 0); name = x.XInternAtom(d, b"_NET_WM_NAME", 0)
    t, f, n, after, data = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
    if x.XGetWindowProperty(d, w, name, 0, 1024, 0, utf8, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n), ctypes.byref(after), ctypes.byref(data)) == 0 and data.value:
        return ctypes.string_at(data.value, n.value).decode(errors="replace")
    return ""
def windows():
    found = []
    def walk(w):
        hint = ClassHint()
        if x.XGetClassHint(d, w, ctypes.byref(hint)) and hint.res_class == b"lrb":
            found.append(w)
        r, p, kids, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if x.XQueryTree(d, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kids), ctypes.byref(n)):
            for i in range(n.value): walk(kids[i])
    walk(root)
    return [(w, title(w)) for w in found]
def _send_key(w, keysym, state):
    for t in (2, 3):  # KeyPress, KeyRelease
        e = XEvent(); k = e.xkey
        k.type = t; k.display = d; k.window = w; k.root = root; k.time = 0; k.state = state; k.same_screen = 1
        k.keycode = x.XKeysymToKeycode(d, keysym)
        x.XSendEvent(d, w, 1, 1 if t == 2 else 2, ctypes.byref(e))
    x.XFlush(d)
def close(w):
    e = XEvent(); c = e.xclient; c.type = 33; c.window = w; c.format = 32
    c.message_type = x.XInternAtom(d, b"WM_PROTOCOLS", 0); c.data[0] = x.XInternAtom(d, b"WM_DELETE_WINDOW", 0)
    x.XSendEvent(d, w, 0, 0, ctypes.byref(e)); x.XFlush(d)
XK_Left, Mod1Mask = 0xff51, 8
def activate(w):
    """Asks the window manager to focus `w`, as a taskbar would
    (_NET_ACTIVE_WINDOW, source 2). Chromium's X11 backend drops key events
    for a window without focus, and window managers may not focus a new
    window while the user works in another (focus-stealing prevention)."""
    e = XEvent(); c = e.xclient; c.type = 33; c.window = w; c.format = 32
    c.message_type = x.XInternAtom(d, b"_NET_ACTIVE_WINDOW", 0)
    c.data[0] = 2; c.data[1] = 0; c.data[2] = 0
    x.XSendEvent(d, root, 0, (1 << 20) | (1 << 19), ctypes.byref(e))  # SubstructureRedirect|Notify
    # And directly: the window manager may ignore the request (an idle
    # session, focus-stealing prevention).
    x.XSetInputFocus(d, w, 2, 0)  # RevertToParent, CurrentTime
    x.XFlush(d)
def key(w, keysym, state):
    """Focuses `w` (see activate), then sends the key (press and release)."""
    activate(w)
    time.sleep(0.2)
    _send_key(w, keysym, state)


def click(w, px, py, button=1):
    """A mouse click at (px, py) in `w` (window coordinates). Unlike a click
    sent through DevTools, it goes through the window, so keyboard focus
    moves to what was clicked (the page), as for a real click."""
    activate(w)
    time.sleep(0.2)
    for t, mask in ((4, 1 << 2), (5, 1 << 3)):  # ButtonPress, ButtonRelease
        e = XEvent(); b = e.xbutton
        b.type = t; b.display = d; b.window = w; b.root = root; b.time = 0
        b.x = px; b.y = py; b.button = button; b.same_screen = 1
        x.XSendEvent(d, w, 1, mask, ctypes.byref(e))
        x.XFlush(d)
        time.sleep(0.1)


def find(name):
    """Windows titled `name`, whatever their class (lrb's dialogs, e.g.
    Settings), deepest last."""
    found = []
    def walk(w):
        if title(w) == name:
            found.append(w)
        r, p, kids, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if x.XQueryTree(d, w, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kids), ctypes.byref(n)):
            for i in range(n.value):
                walk(kids[i])
    walk(root)
    return found
