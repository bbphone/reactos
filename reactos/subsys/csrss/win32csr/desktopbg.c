/* $Id: desktopbg.c,v 1.9 2004/08/17 14:57:52 weiden Exp $
 *
 * reactos/subsys/csrss/win32csr/desktopbg.c
 *
 * Desktop background window functions
 *
 * ReactOS Operating System
 */

/*
 * There is a problem with size of LPC_MESSAGE structure. In the old ReactOS
 * headers it doesn't contain the data field and so it has a different size.
 * We must use this workaround to get our Data field 0-sized.
 */

#include <windef.h>
#include <winnt.h>
#undef ANYSIZE_ARRAY
#define ANYSIZE_ARRAY 0
#include <ddk/ntapi.h>

#include <windows.h>
#include <csrss/csrss.h>

#include "api.h"
#include "desktopbg.h"

#define NDEBUG
#include <debug.h>

#define DESKTOP_WINDOW_ATOM 32880

#ifndef WM_APP
#define WM_APP 0x8000
#endif
#define PM_SHOW_DESKTOP (WM_APP + 1)
#define PM_HIDE_DESKTOP (WM_APP + 2)

typedef struct tagDTBG_THREAD_DATA
{
  HDESK Desktop;
  HANDLE Event;
  NTSTATUS Status;
} DTBG_THREAD_DATA, *PDTBG_THREAD_DATA;

typedef struct tagPRIVATE_NOTIFY_DESKTOP
{
  NMHDR hdr;
  union
  {
    struct /* PM_SHOW_DESKTOP */
    {
      int Width;
      int Height;
    } ShowDesktop;
  };
} PRIVATE_NOTIFY_DESKTOP, *PPRIVATE_NOTIFY_DESKTOP;

static BOOL Initialized = FALSE;
static HWND VisibleDesktopWindow = NULL;

static LRESULT CALLBACK
DtbgWindowProc(HWND Wnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
  switch(Msg)
    {
      case WM_ERASEBKGND:
        return 1;

      case WM_PAINT:
      {
        PAINTSTRUCT PS;
        RECT rc;
        HDC hDC;
        
        if(GetUpdateRect(Wnd, &rc, FALSE) &&
           (hDC = BeginPaint(Wnd, &PS)))
        {
          PaintDesktop(hDC);
          EndPaint(Wnd, &PS);
        }
        return 0;
      }

      case WM_SETCURSOR:
	return (LRESULT) SetCursor(LoadCursorW(0, (LPCWSTR)IDC_ARROW));

      case WM_NCCREATE:
        return (LRESULT) TRUE;

      case WM_CREATE:
        return 0;

      case WM_NOTIFY:
      {
        PPRIVATE_NOTIFY_DESKTOP nmh = (PPRIVATE_NOTIFY_DESKTOP)lParam;
        
        /* Use WM_NOTIFY for private messages since it can't be sent between
           processes! */
        switch(nmh->hdr.code)
        {
          case PM_SHOW_DESKTOP:
          {
            LRESULT Result;

            Result = ! SetWindowPos(Wnd,
                                    NULL, 0, 0,
                                    nmh->ShowDesktop.Width,
                                    nmh->ShowDesktop.Height,
                                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOREDRAW);
            UpdateWindow(Wnd);
            VisibleDesktopWindow = Wnd;
            return Result;
          }

          case PM_HIDE_DESKTOP:
          {
            LRESULT Result;

            Result = ! SetWindowPos(Wnd,
                                    NULL, 0, 0, 0, 0,
                                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE |
                                    SWP_HIDEWINDOW);
            UpdateWindow(Wnd);
            VisibleDesktopWindow = NULL;
            return Result;
          }
          
          default:
            DPRINT("Unknown notification code 0x%x sent to the desktop window!\n", nmh->code);
            return 0;
        }
      }
    }

  return 0;
}

static BOOL FASTCALL
DtbgInit()
{
  WNDCLASSEXW Class;
  HWINSTA WindowStation;
  ATOM ClassAtom;

  /* Attach to window station */
  WindowStation = OpenWindowStationW(L"WinSta0", FALSE, GENERIC_ALL);
  if (NULL == WindowStation)
    {
      DPRINT1("Failed to open window station\n");
      return FALSE;
    }
  if (! SetProcessWindowStation(WindowStation))
    {
      DPRINT1("Failed to set process window station\n");
      return FALSE;
    }

  /* 
   * Create the desktop window class
   */
  Class.cbSize = sizeof(WNDCLASSEXW);
  Class.style = 0;
  Class.lpfnWndProc = DtbgWindowProc;
  Class.cbClsExtra = 0;
  Class.cbWndExtra = 0;
  Class.hInstance = (HINSTANCE) GetModuleHandleW(NULL);
  Class.hIcon = NULL;
  Class.hCursor = NULL;
  Class.hbrBackground = GetSysColorBrush(COLOR_BACKGROUND);
  Class.lpszMenuName = NULL;
  Class.lpszClassName = (LPCWSTR) DESKTOP_WINDOW_ATOM;
  ClassAtom = RegisterClassExW(&Class);
  if ((ATOM) 0 == ClassAtom)
    {
      DPRINT1("Unable to register desktop background class (error %d)\n",
              GetLastError());
      return FALSE;
    }
  VisibleDesktopWindow = NULL;

  return TRUE;
}

static DWORD STDCALL
DtbgDesktopThread(PVOID Data)
{
  HWND BackgroundWnd;
  MSG msg;
  PDTBG_THREAD_DATA ThreadData = (PDTBG_THREAD_DATA) Data;

  if (! SetThreadDesktop(ThreadData->Desktop))
    {
      DPRINT1("Failed to set thread desktop\n");
      ThreadData->Status = STATUS_UNSUCCESSFUL;
      SetEvent(ThreadData->Event);
      return 1;
    }
  BackgroundWnd = CreateWindowW((LPCWSTR) DESKTOP_WINDOW_ATOM,
                                L"",
                                WS_POPUP | WS_CLIPCHILDREN,
                                0,
                                0,
                                0,
                                0,
                                NULL,
                                NULL,
                                (HINSTANCE) GetModuleHandleW(NULL),
                                NULL);
  if (NULL == BackgroundWnd)
    {
      DPRINT1("Failed to create desktop background window\n");
      ThreadData->Status = STATUS_UNSUCCESSFUL;
      SetEvent(ThreadData->Event);
      return 1;
    }

  ThreadData->Status = STATUS_SUCCESS;
  SetEvent(ThreadData->Event);

  while (GetMessageW(&msg, NULL, 0, 0))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

  return 1;
}

CSR_API(CsrCreateDesktop)
{
  HDESK Desktop;
  DTBG_THREAD_DATA ThreadData;
  HANDLE ThreadHandle;

  DPRINT("CsrCreateDesktop\n");

  Reply->Header.MessageSize = sizeof(CSRSS_API_REPLY);
  Reply->Header.DataSize = sizeof(CSRSS_API_REPLY) - LPC_MESSAGE_BASE_SIZE;

  if (! Initialized)
    {
      Initialized = TRUE;
      if (! DtbgInit())
        {
          return Reply->Status = STATUS_UNSUCCESSFUL;
        }
    }

  Desktop = OpenDesktopW(Request->Data.CreateDesktopRequest.DesktopName,
                         0, FALSE, GENERIC_ALL);
  if (NULL == Desktop)
    {
      DPRINT1("Failed to open desktop %S\n",
              Request->Data.CreateDesktopRequest.DesktopName);
      return Reply->Status = STATUS_UNSUCCESSFUL;
    }

  ThreadData.Desktop = Desktop;
  ThreadData.Event = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (NULL == ThreadData.Event)
    {
      DPRINT1("Failed to create event (error %d)\n", GetLastError());
      return Reply->Status = STATUS_UNSUCCESSFUL;
    }
  ThreadHandle = CreateThread(NULL,
                              0,
                              DtbgDesktopThread,
                              (PVOID) &ThreadData,
                              0,
                              NULL);
  if (NULL == ThreadHandle)
    {
      CloseHandle(ThreadData.Event);
      DPRINT1("Failed to create desktop window thread.\n");
      return Reply->Status = STATUS_UNSUCCESSFUL;
    }
  CloseHandle(ThreadHandle);

  WaitForSingleObject(ThreadData.Event, INFINITE);
  CloseHandle(ThreadData.Event);

  Reply->Status = ThreadData.Status;

  return Reply->Status;
}

CSR_API(CsrShowDesktop)
{
  PRIVATE_NOTIFY_DESKTOP nmh;
  DPRINT("CsrShowDesktop\n");

  Reply->Header.MessageSize = sizeof(CSRSS_API_REPLY);
  Reply->Header.DataSize = sizeof(CSRSS_API_REPLY) - LPC_MESSAGE_BASE_SIZE;

  nmh.hdr.hwndFrom = Request->Data.ShowDesktopRequest.DesktopWindow;
  nmh.hdr.idFrom = 0;
  nmh.hdr.code = PM_SHOW_DESKTOP;
  
  nmh.ShowDesktop.Width = (int)Request->Data.ShowDesktopRequest.Width;
  nmh.ShowDesktop.Height = (int)Request->Data.ShowDesktopRequest.Height;

  Reply->Status = SendMessageW(Request->Data.ShowDesktopRequest.DesktopWindow,
                               WM_NOTIFY,
                               (WPARAM)nmh.hdr.hwndFrom,
                               (LPARAM)&nmh)
                  ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;

  return Reply->Status;
}

CSR_API(CsrHideDesktop)
{
  PRIVATE_NOTIFY_DESKTOP nmh;
  DPRINT("CsrHideDesktop\n");

  Reply->Header.MessageSize = sizeof(CSRSS_API_REPLY);
  Reply->Header.DataSize = sizeof(CSRSS_API_REPLY) - LPC_MESSAGE_BASE_SIZE;

  nmh.hdr.hwndFrom = Request->Data.ShowDesktopRequest.DesktopWindow;
  nmh.hdr.idFrom = 0;
  nmh.hdr.code = PM_HIDE_DESKTOP;

  Reply->Status = SendMessageW(Request->Data.ShowDesktopRequest.DesktopWindow,
                               WM_NOTIFY,
                               (WPARAM)nmh.hdr.hwndFrom,
                               (LPARAM)&nmh)
                  ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;

  return Reply->Status;
}

BOOL FASTCALL
DtbgIsDesktopVisible(VOID)
{
  if (NULL != VisibleDesktopWindow && ! IsWindowVisible(VisibleDesktopWindow))
    {
      VisibleDesktopWindow = NULL;
    }

  return NULL != VisibleDesktopWindow;
}

/* EOF */
