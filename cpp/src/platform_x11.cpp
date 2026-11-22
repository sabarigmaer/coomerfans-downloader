#include "platform_x11.hpp"

#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <poll.h>

#include <chrono>
#include <clocale>
#include <cstdio>

#include "imgui.h"

static double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool X11Window::init(const char* title, int w, int h) {
  std::setlocale(LC_ALL, "");
  dpy_ = XOpenDisplay(nullptr);
  if (!dpy_) { std::fprintf(stderr, "Cannot open X display (is DISPLAY set?)\n"); return false; }
  int scr = DefaultScreen(dpy_);
  int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
  XVisualInfo* vi = glXChooseVisual(dpy_, scr, attrs);
  if (!vi) { std::fprintf(stderr, "No suitable GLX visual\n"); return false; }

  XSetWindowAttributes swa = {};
  swa.colormap = XCreateColormap(dpy_, RootWindow(dpy_, scr), vi->visual, AllocNone);
  swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   StructureNotifyMask | FocusChangeMask | LeaveWindowMask | ExposureMask;
  win_ = XCreateWindow(dpy_, RootWindow(dpy_, scr), 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual,
                       CWColormap | CWEventMask, &swa);
  w_ = w; h_ = h;

  XStoreName(dpy_, win_, title);
  XClassHint ch = {(char*)"coomerfans-downloader", (char*)"CoomerFansDownloader"};
  XSetClassHint(dpy_, win_, &ch);
  XSizeHints sh = {};
  sh.flags = PMinSize;
  sh.min_width = 820; sh.min_height = 600;
  XSetWMNormalHints(dpy_, win_, &sh);
  wmDelete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy_, win_, &wmDelete_, 1);

  if (XSupportsLocale()) {
    XSetLocaleModifiers("");
    xim_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    if (xim_)
      xic_ = XCreateIC(xim_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win_,
                       XNFocusWindow, win_, nullptr);
  }

  XMapWindow(dpy_, win_);
  ctx_ = glXCreateContext(dpy_, vi, nullptr, True);
  XFree(vi);
  if (!ctx_ || !glXMakeCurrent(dpy_, win_, ctx_)) { std::fprintf(stderr, "Failed to create GL context\n"); return false; }

  typedef void (*SwapIntervalFn)(Display*, GLXDrawable, int);
  if (auto fn = (SwapIntervalFn)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT")) fn(dpy_, win_, 1);
  last_ = nowSec();
  return true;
}

void X11Window::shutdown() {
  if (!dpy_) return;
  if (xic_) XDestroyIC(xic_);
  if (xim_) XCloseIM(xim_);
  if (ctx_) { glXMakeCurrent(dpy_, None, nullptr); glXDestroyContext(dpy_, ctx_); }
  if (win_) XDestroyWindow(dpy_, win_);
  XCloseDisplay(dpy_);
  dpy_ = nullptr;
}

static ImGuiKey mapKey(KeySym ks) {
  if (ks >= XK_a && ks <= XK_z) return (ImGuiKey)(ImGuiKey_A + (ks - XK_a));
  if (ks >= XK_A && ks <= XK_Z) return (ImGuiKey)(ImGuiKey_A + (ks - XK_A));
  if (ks >= XK_0 && ks <= XK_9) return (ImGuiKey)(ImGuiKey_0 + (ks - XK_0));
  switch (ks) {
    case XK_Tab: return ImGuiKey_Tab;
    case XK_Left: return ImGuiKey_LeftArrow;
    case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow;
    case XK_Down: return ImGuiKey_DownArrow;
    case XK_Page_Up: return ImGuiKey_PageUp;
    case XK_Page_Down: return ImGuiKey_PageDown;
    case XK_Home: return ImGuiKey_Home;
    case XK_End: return ImGuiKey_End;
    case XK_Insert: return ImGuiKey_Insert;
    case XK_Delete: return ImGuiKey_Delete;
    case XK_BackSpace: return ImGuiKey_Backspace;
    case XK_space: return ImGuiKey_Space;
    case XK_Return: return ImGuiKey_Enter;
    case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_Escape: return ImGuiKey_Escape;
    default: return ImGuiKey_None;
  }
}

void X11Window::handle(XEvent& ev) {
  ImGuiIO& io = ImGui::GetIO();
  switch (ev.type) {
    case ConfigureNotify: w_ = ev.xconfigure.width; h_ = ev.xconfigure.height; break;
    case ClientMessage:
      if ((Atom)ev.xclient.data.l[0] == wmDelete_) closed_ = true;
      break;
    case MotionNotify: io.AddMousePosEvent((float)ev.xmotion.x, (float)ev.xmotion.y); break;
    case LeaveNotify: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
    case FocusIn: io.AddFocusEvent(true); if (xic_) XSetICFocus(xic_); break;
    case FocusOut: io.AddFocusEvent(false); if (xic_) XUnsetICFocus(xic_); break;
    case ButtonPress:
    case ButtonRelease: {
      bool down = ev.type == ButtonPress;
      io.AddMousePosEvent((float)ev.xbutton.x, (float)ev.xbutton.y);
      switch (ev.xbutton.button) {
        case 1: io.AddMouseButtonEvent(0, down); break;
        case 3: io.AddMouseButtonEvent(1, down); break;
        case 2: io.AddMouseButtonEvent(2, down); break;
        case 4: if (down) io.AddMouseWheelEvent(0, 1); break;
        case 5: if (down) io.AddMouseWheelEvent(0, -1); break;
        case 6: if (down) io.AddMouseWheelEvent(1, 0); break;
        case 7: if (down) io.AddMouseWheelEvent(-1, 0); break;
      }
      break;
    }
    case KeyPress:
    case KeyRelease: {
      bool down = ev.type == KeyPress;
      unsigned st = ev.xkey.state;
      io.AddKeyEvent(ImGuiMod_Ctrl, st & ControlMask);
      io.AddKeyEvent(ImGuiMod_Shift, st & ShiftMask);
      io.AddKeyEvent(ImGuiMod_Alt, st & Mod1Mask);
      io.AddKeyEvent(ImGuiMod_Super, st & Mod4Mask);
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      ImGuiKey k = mapKey(ks);
      if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
      if (down && !(st & (ControlMask | Mod1Mask))) {
        char buf[64] = {};
        int n = 0;
        Status status;
        if (xic_) n = Xutf8LookupString(xic_, &ev.xkey, buf, sizeof buf - 1, &ks, &status);
        else n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, nullptr);
        if (n > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127) { buf[n] = 0; io.AddInputCharactersUTF8(buf); }
      }
      break;
    }
  }
}

bool X11Window::pump(int timeoutMs) {
  if (XPending(dpy_) == 0) {
    struct pollfd p = {ConnectionNumber(dpy_), POLLIN, 0};
    poll(&p, 1, timeoutMs);
  }
  while (XPending(dpy_)) {
    XEvent ev;
    XNextEvent(dpy_, &ev);
    if (XFilterEvent(&ev, None)) continue;
    handle(ev);
  }
  return !closed_;
}

void X11Window::beginFrame() {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)w_, (float)h_);
  double t = nowSec();
  io.DeltaTime = (float)(t - last_ > 0 ? t - last_ : 1.0 / 60);
  last_ = t;
}

void X11Window::swap() { glXSwapBuffers(dpy_, win_); }
