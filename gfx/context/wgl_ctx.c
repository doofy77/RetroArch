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

// Win32/WGL context.

#include "../../driver.h"
#include "../gfx_context.h"
#include "../gl_common.h"
#include "../gfx_common.h"
#include <windows.h>

static HWND g_hwnd;
static HGLRC g_hrc;
static HDC g_hdc;

static bool g_quit;
static bool g_inited;
static unsigned g_interval;

static unsigned g_resize_width;
static unsigned g_resize_height;
static bool g_resized;

static bool g_restore_desktop;

static void gfx_ctx_get_video_size(unsigned *width, unsigned *height);
static void gfx_ctx_destroy(void);

static BOOL (APIENTRY *p_swap_interval)(int);

static void setup_pixel_format(HDC hdc)
{
   PIXELFORMATDESCRIPTOR pfd = {0};
   pfd.nSize        = sizeof(PIXELFORMATDESCRIPTOR);
   pfd.nVersion     = 1;
   pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType   = PFD_TYPE_RGBA;
   pfd.cColorBits   = 32;
   pfd.cDepthBits   = 0;
   pfd.cStencilBits = 0;
   pfd.iLayerType   = PFD_MAIN_PLANE;

   SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
}

static void create_gl_context(HWND hwnd)
{
   g_hdc = GetDC(hwnd);
   setup_pixel_format(g_hdc);

   g_hrc = wglCreateContext(g_hdc);
   if (g_hrc)
   {
      if (wglMakeCurrent(g_hdc, g_hrc))
         g_inited = true;
      else
         g_quit = true;
   }
   else
      g_quit = true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
      WPARAM wparam, LPARAM lparam)
{
   switch (message)
   {
      case WM_SYSCOMMAND:
         // Prevent screensavers, etc, while running.
         switch (wparam)
         {
            case SC_SCREENSAVE:
            case SC_MONITORPOWER:
               return 0;
         }
         break;

      case WM_CREATE:
         create_gl_context(hwnd);
         return 0;

      case WM_CLOSE:
      case WM_DESTROY:
      case WM_QUIT:
         g_quit = true;
         return 0;

      case WM_SIZE:
         // Do not send resize message if we minimize.
         if (wparam != SIZE_MAXHIDE && wparam != SIZE_MINIMIZED)
         {
            g_resize_width  = LOWORD(lparam);
            g_resize_height = HIWORD(lparam);
            g_resized = true;
         }
         return 0;
   }

   return DefWindowProc(hwnd, message, wparam, lparam);
}

static void gfx_ctx_swap_interval(unsigned interval)
{
   g_interval = interval;

   if (g_hrc && p_swap_interval)
   {
      RARCH_LOG("[WGL]: wglSwapInterval(%u)\n", g_interval);
      if (!p_swap_interval(g_interval))
         RARCH_WARN("[WGL]: wglSwapInterval() failed.\n");
   }
}

static void gfx_ctx_check_window(bool *quit,
      bool *resize, unsigned *width, unsigned *height, unsigned frame_count)
{
   (void)frame_count;

   MSG msg;
   while (PeekMessage(&msg, g_hwnd, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   *quit = g_quit;
   if (g_resized)
   {
      *resize = true;
      *width  = g_resize_width;
      *height = g_resize_height;
      g_resized = false;
   }
}

static void gfx_ctx_swap_buffers(void)
{
   SwapBuffers(g_hdc);
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
      SetWindowText(g_hwnd, buf);
}

static void gfx_ctx_get_video_size(unsigned *width, unsigned *height)
{
   if (!g_hwnd)
   {
      RECT screen_rect;
      GetClientRect(GetDesktopWindow(), &screen_rect);
      *width  = screen_rect.right - screen_rect.left;
      *height = screen_rect.bottom - screen_rect.top;
   }
   else
   {
      *width  = g_resize_width;
      *height = g_resize_height;
   }
}

static bool gfx_ctx_init(void)
{
   if (g_inited)
      return false;

   g_quit = false;
   g_restore_desktop = false;

   WNDCLASSEX wndclass = {0};
   wndclass.cbSize = sizeof(wndclass);
   wndclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
   wndclass.lpfnWndProc = WndProc;
   wndclass.hInstance = GetModuleHandle(NULL);
   wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
   wndclass.lpszClassName = "RetroArch";

   if (!RegisterClassEx(&wndclass))
      return false;

   return true;
}

static bool set_fullscreen(unsigned width, unsigned height)
{
   DEVMODE devmode;
   memset(&devmode, 0, sizeof(devmode));
   devmode.dmSize       = sizeof(DEVMODE);
   devmode.dmPelsWidth  = width;
   devmode.dmPelsHeight = height;
   devmode.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

   return ChangeDisplaySettings(&devmode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL;
}

static bool gfx_ctx_set_video_mode(
      unsigned width, unsigned height,
      unsigned bits, bool fullscreen)
{
   (void)bits;

   DWORD style;
   RECT screen_rect;
   GetClientRect(GetDesktopWindow(), &screen_rect);

   g_resize_width  = width;
   g_resize_height = height;

   bool windowed_full = g_settings.video.windowed_fullscreen;
   if (fullscreen)
   {
      if (windowed_full)
         style = WS_EX_TOPMOST | WS_POPUP;
      else
      {
         style = WS_POPUP | WS_VISIBLE;

         AdjustWindowRect(&screen_rect, style, FALSE);
         width  = screen_rect.right - screen_rect.left;
         height = screen_rect.bottom - screen_rect.top;

         if (!set_fullscreen(width, height))
            goto error;

         g_restore_desktop = true;
      }
   }
   else
   {
      style = WS_OVERLAPPEDWINDOW;
      RECT rect   = {0};
      rect.right  = width;
      rect.bottom = height;
      AdjustWindowRect(&rect, style, FALSE);
      width  = rect.right - rect.left;
      height = rect.bottom - rect.top;
   }

   g_hwnd = CreateWindowEx(0, "RetroArch", "RetroArch", style,
         windowed_full ? 0 : CW_USEDEFAULT, windowed_full ? 0 : CW_USEDEFAULT,
         width, height,
         NULL, NULL, NULL, NULL);

   if (!g_hwnd)
      goto error;

   gfx_ctx_update_window_title(true);

   if (!fullscreen || windowed_full)
   {
      ShowCursor(FALSE);
      ShowWindow(g_hwnd, SW_RESTORE);
      UpdateWindow(g_hwnd);
      SetForegroundWindow(g_hwnd);
      SetFocus(g_hwnd);
   }

   // Wait until GL context is created (or failed to do so ...)
   MSG msg;
   while (!g_inited && !g_quit && GetMessage(&msg, g_hwnd, 0, 0))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   if (g_quit)
      goto error;

   p_swap_interval = (BOOL (APIENTRY *)(int))wglGetProcAddress("wglSwapIntervalEXT");

   gfx_ctx_swap_interval(g_interval);

   driver.display_type  = RARCH_DISPLAY_WIN32;
   driver.video_display = 0;
   driver.video_window  = (uintptr_t)g_hwnd;

   return true;

error:
   gfx_ctx_destroy();
   return false;
}

static void gfx_ctx_destroy(void)
{
   if (g_hrc)
   {
      wglMakeCurrent(NULL, NULL);
      wglDeleteContext(g_hrc);
      g_hrc = NULL;
   }

   if (g_hwnd && g_hdc)
   {
      ReleaseDC(g_hwnd, g_hdc);
      g_hdc = NULL;
   }

   if (g_hwnd)
   {
      DestroyWindow(g_hwnd);
      UnregisterClass("RetroArch", GetModuleHandle(NULL));
      g_hwnd = NULL;
   }

   if (g_restore_desktop)
   {
      ChangeDisplaySettings(NULL, 0);
      g_restore_desktop = false;
   }

   g_inited = false;
}

static void gfx_ctx_input_driver(const input_driver_t **input, void **input_data)
{
   void *dinput = input_dinput.init();
   *input       = dinput ? &input_dinput : NULL;
   *input_data  = dinput;
}

static bool gfx_ctx_has_focus(void)
{
   if (!g_inited)
      return false;

   return GetFocus() == g_hwnd;
}

static gfx_ctx_proc_t gfx_ctx_get_proc_address(const char *symbol)
{
   return (gfx_ctx_proc_t)wglGetProcAddress(symbol);
}

static bool gfx_ctx_bind_api(enum gfx_ctx_api api)
{
   return api == GFX_CTX_OPENGL_API;
}

const gfx_ctx_driver_t gfx_ctx_wgl = {
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
   "wgl",
};
