/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2012 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

// X/EGL context. Mostly used for testing GLES code paths.
// Should be its own file as it has lots of X11 stuff baked into it as well.

#include "../../driver.h"
#include "../gfx_context.h"
#include "../gl_common.h"
#include "../gfx_common.h"
#include "x11_common.h"

#include <signal.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display *g_dpy;
static Window   g_win;
static Colormap g_cmap;
static Atom g_quit_atom;
static bool g_has_focus;

static EGLContext g_egl_ctx;
static EGLSurface g_egl_surf;
static EGLDisplay g_egl_dpy;
static EGLConfig g_config;

static XF86VidModeModeInfo g_desktop_mode;
static bool g_should_reset_mode;

static volatile sig_atomic_t g_quit;
static bool g_inited;
static unsigned g_interval;
static enum gfx_ctx_api g_api;

static void sighandler(int sig)
{
   (void)sig;
   g_quit = 1;
}

static Bool egl_wait_notify(Display *d, XEvent *e, char *arg)
{
   (void)d;
   (void)e;
   return e->type == MapNotify && e->xmap.window == g_win;
}

static void gfx_ctx_get_video_size(unsigned *width, unsigned *height);
static void gfx_ctx_destroy(void);

static void gfx_ctx_swap_interval(unsigned interval)
{
   g_interval = interval;
   if (g_egl_dpy)
   {
      RARCH_LOG("[X/EGL]: eglSwapInterval(%u)\n", g_interval);
      if (!eglSwapInterval(g_egl_dpy, g_interval))
         RARCH_ERR("[X/EGL]: eglSwapInterval() failed.\n");
   }
}

static void gfx_ctx_check_window(bool *quit,
      bool *resize, unsigned *width, unsigned *height, unsigned frame_count)
{
   (void)frame_count;

   unsigned new_width = *width, new_height = *height;
   gfx_ctx_get_video_size(&new_width, &new_height);

   if (new_width != *width || new_height != *height)
   {
      *resize = true;
      *width  = new_width;
      *height = new_height;
   }

   XEvent event;
   while (XPending(g_dpy))
   {
      XNextEvent(g_dpy, &event);
      switch (event.type)
      {
         case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == g_quit_atom)
               g_quit = true;
            break;

         case DestroyNotify:
            g_quit = true;
            break;

         case MapNotify:
            g_has_focus = true;
            break;

         case UnmapNotify:
            g_has_focus = false;
            break;
      }
   }

   *quit = g_quit;
}

static void gfx_ctx_swap_buffers(void)
{
   eglSwapBuffers(g_egl_dpy, g_egl_surf);
}

static void gfx_ctx_set_resize(unsigned width, unsigned height)
{
   (void)width;
   (void)height;
}

static void gfx_ctx_update_window_title(bool reset)
{
   if (reset)
      gfx_window_title_reset();

   char buf[128];
   if (gfx_window_title(buf, sizeof(buf)))
      XStoreName(g_dpy, g_win, buf);
}

static void gfx_ctx_get_video_size(unsigned *width, unsigned *height)
{
   if (!g_dpy || g_win == None)
   {
      Display *dpy = XOpenDisplay(NULL);
      if (dpy)
      {
         int screen = DefaultScreen(dpy);
         *width  = DisplayWidth(dpy, screen);
         *height = DisplayHeight(dpy, screen);
         XCloseDisplay(dpy);
      }
      else
      {
         *width  = 0;
         *height = 0;
      }
   }
   else
   {
      XWindowAttributes target;
      XGetWindowAttributes(g_dpy, g_win, &target);

      *width  = target.width;
      *height = target.height;
   }
}

static bool gfx_ctx_init(void)
{
   if (g_inited)
      return false;

#define EGL_ATTRIBS_BASE \
   EGL_SURFACE_TYPE,    EGL_WINDOW_BIT, \
   EGL_RED_SIZE,        8, \
   EGL_GREEN_SIZE,      8, \
   EGL_BLUE_SIZE,       8, \
   EGL_DEPTH_SIZE,      0, \
   EGL_STENCIL_SIZE,    0

   static const EGLint egl_attribs_gl[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE,
   };

   static const EGLint egl_attribs_gles[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE,
   };

   static const EGLint egl_attribs_vg[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENVG_BIT,
      EGL_NONE,
   };

   const EGLint *attrib_ptr;
   switch (g_api)
   {
      case GFX_CTX_OPENGL_API:
         attrib_ptr = egl_attribs_gl;
         break;
      case GFX_CTX_OPENGL_ES_API:
         attrib_ptr = egl_attribs_gles;
         break;
      case GFX_CTX_OPENVG_API:
         attrib_ptr = egl_attribs_vg;
         break;
      default:
         attrib_ptr = NULL;
   }

   g_quit = 0;

   g_dpy = XOpenDisplay(NULL);
   if (!g_dpy)
      goto error;

   g_egl_dpy = eglGetDisplay(g_dpy);
   if (!g_egl_dpy)
      goto error;

   EGLint egl_major, egl_minor;
   if (!eglInitialize(g_egl_dpy, &egl_major, &egl_minor))
      goto error;

   RARCH_LOG("[X/EGL]: EGL version: %d.%d\n", egl_major, egl_minor);

   EGLint num_configs;
   if (!eglChooseConfig(g_egl_dpy, attrib_ptr, &g_config, 1, &num_configs)
         || num_configs == 0 || !g_config)
      goto error;

   return true;

error:
   gfx_ctx_destroy();
   return false;
}

static bool gfx_ctx_set_video_mode(
      unsigned width, unsigned height,
      unsigned bits, bool fullscreen)
{
   (void)bits;

   struct sigaction sa = {{0}};
   sa.sa_handler = sighandler;
   sa.sa_flags   = SA_RESTART;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   XVisualInfo temp = {0};
   XSetWindowAttributes swa = {0};
   XVisualInfo *vi = NULL;
   bool windowed_full = g_settings.video.windowed_fullscreen;
   bool true_full = false;

   EGLint vid;
   if (!eglGetConfigAttrib(g_egl_dpy, g_config, EGL_NATIVE_VISUAL_ID, &vid))
      goto error;

   temp.visualid = vid;

   EGLint num_visuals;
   vi = XGetVisualInfo(g_dpy, VisualIDMask, &temp, &num_visuals);
   if (!vi)
      goto error;

   swa.colormap = g_cmap = XCreateColormap(g_dpy, RootWindow(g_dpy, vi->screen),
         vi->visual, AllocNone);
   swa.event_mask = StructureNotifyMask;
   swa.override_redirect = fullscreen ? True : False;

   if (fullscreen && !windowed_full)
   {
      if (x11_enter_fullscreen(g_dpy, width, height, &g_desktop_mode))
      {
         g_should_reset_mode = true;
         true_full = true;
      }
   }

   g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, vi->screen),
         0, 0, width ? width : 200, height ? height : 200, 0,
         vi->depth, InputOutput, vi->visual, 
         CWBorderPixel | CWColormap | CWEventMask | (true_full ? CWOverrideRedirect : 0), &swa);
   XSetWindowBackground(g_dpy, g_win, 0);

   // GLES 2.0. Don't use for any other API.
   static const EGLint egl_ctx_gles_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };

   g_egl_ctx = eglCreateContext(g_egl_dpy, g_config, EGL_NO_CONTEXT,
         (g_api == GFX_CTX_OPENGL_ES_API) ? egl_ctx_gles_attribs : NULL);

   if (!g_egl_ctx)
      goto error;

   g_egl_surf = eglCreateWindowSurface(g_egl_dpy, g_config, g_win, NULL);
   if (!g_egl_surf)
      goto error;

   if (!eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx))
      goto error;

   gfx_ctx_update_window_title(true);
   x11_hide_mouse(g_dpy, g_win);

   if (true_full)
   {
      XMapRaised(g_dpy, g_win);
      XGrabKeyboard(g_dpy, g_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
   }
   else if (fullscreen) // We attempted true fullscreen, but failed. Attempt using windowed fullscreen.
   {
      XMapRaised(g_dpy, g_win);
      RARCH_WARN("[X/EGL]: Using windowed fullscreen.\n");
      x11_windowed_fullscreen(g_dpy, g_win);
   }
   else
      XMapWindow(g_dpy, g_win);

   XEvent event;
   XIfEvent(g_dpy, &event, egl_wait_notify, NULL);
   XSetInputFocus(g_dpy, g_win, RevertToNone, CurrentTime);

   g_quit_atom = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
   if (g_quit_atom)
      XSetWMProtocols(g_dpy, g_win, &g_quit_atom, 1);

   gfx_ctx_swap_interval(g_interval);

   XFree(vi);
   g_has_focus = true;
   g_inited    = true;

   driver.display_type  = RARCH_DISPLAY_X11;
   driver.video_display = (uintptr_t)g_dpy;
   driver.video_window  = (uintptr_t)g_win;

   return true;

error:
   if (vi)
      XFree(vi);

   gfx_ctx_destroy();
   return false;
}

static void gfx_ctx_destroy(void)
{
   if (g_egl_dpy)
   {
      if (g_egl_ctx)
      {
         eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         eglDestroyContext(g_egl_dpy, g_egl_ctx);
      }

      if (g_egl_surf)
         eglDestroySurface(g_egl_dpy, g_egl_surf);
      eglTerminate(g_egl_dpy);
   }

   g_egl_ctx  = NULL;
   g_egl_surf = NULL;
   g_egl_dpy  = NULL;
   g_config   = 0;

   if (g_win)
   {
      XUnmapWindow(g_dpy, g_win);
      XDestroyWindow(g_dpy, g_win);
      g_win = None;
   }

   if (g_cmap)
   {
      XFreeColormap(g_dpy, g_cmap);
      g_cmap = None;
   }

   if (g_should_reset_mode)
   {
      x11_exit_fullscreen(g_dpy, &g_desktop_mode);
      g_should_reset_mode = false;
   }

   if (g_dpy)
   {
      XCloseDisplay(g_dpy);
      g_dpy = NULL;
   }

   g_inited = false;
}

static void gfx_ctx_input_driver(const input_driver_t **input, void **input_data)
{
   void *xinput = input_x.init();
   *input       = xinput ? &input_x : NULL;
   *input_data  = xinput;
}

static bool gfx_ctx_has_focus(void)
{
   if (!g_inited)
      return false;

   Window win;
   int rev;
   XGetInputFocus(g_dpy, &win, &rev);

   return win == g_win && g_has_focus;
}

static gfx_ctx_proc_t gfx_ctx_get_proc_address(const char *symbol)
{
   return eglGetProcAddress(symbol);
}

static bool gfx_ctx_bind_api(enum gfx_ctx_api api)
{
   g_api = api;
   switch (api)
   {
      case GFX_CTX_OPENGL_API:
         return eglBindAPI(EGL_OPENGL_API);
      case GFX_CTX_OPENGL_ES_API:
         return eglBindAPI(EGL_OPENGL_ES_API);
      case GFX_CTX_OPENVG_API:
         return eglBindAPI(EGL_OPENVG_API);
      default:
         return false;
   }
}

const gfx_ctx_driver_t gfx_ctx_x_egl = {
   gfx_ctx_init,
   gfx_ctx_destroy,
   gfx_ctx_bind_api,
   gfx_ctx_swap_interval,
   gfx_ctx_set_video_mode,
   gfx_ctx_get_video_size,
   NULL,
   gfx_ctx_update_window_title,
   gfx_ctx_check_window,
   gfx_ctx_set_resize,
   gfx_ctx_has_focus,
   gfx_ctx_swap_buffers,
   gfx_ctx_input_driver,
   gfx_ctx_get_proc_address,
   "x-egl",
};
