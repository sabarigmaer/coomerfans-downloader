#pragma once
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

/** Minimal Xlib + GLX window that feeds Dear ImGui's IO (no GLFW/SDL needed). */
class X11Window {
 public:
  bool init(const char* title, int w, int h);
  void shutdown();
  /** Wait up to timeoutMs for events and forward them to ImGui. Returns false once the window is closed. */
  bool pump(int timeoutMs);
  void beginFrame();  // display size + delta time
  void swap();
  int width() const { return w_; }
  int height() const { return h_; }

 private:
  void handle(XEvent& ev);
  Display* dpy_ = nullptr;
  Window win_ = 0;
  GLXContext ctx_ = nullptr;
  XIM xim_ = nullptr;
  XIC xic_ = nullptr;
  Atom wmDelete_ = 0;
  int w_ = 0, h_ = 0;
  bool closed_ = false;
  double last_ = 0;
};
