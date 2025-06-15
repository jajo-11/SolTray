// SolTray.cpp : Defines the entry point for the application.
//

#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <SDKDDKVer.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <WinUser.h>
#include <strsafe.h>
#include <tchar.h>
#include <timeapi.h>


#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#include "resource.h"

#pragma comment(lib, "Dxva2.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Winmm.lib")

#define WM_USER_SHELLICON WM_USER + 1
#define WM_TASKBAR_CREATE RegisterWindowMessage(_T("TaskbarCreated"))

HWND hWnd;
HINSTANCE hInst;
NOTIFYICONDATAW structNID;
NOTIFYICONIDENTIFIER idNID;
BOOL Enabled;
HANDLE monitor_handle;

DWORD current_brightness = 0;
DWORD max_brightness = 100;
DWORD min_brightness = 0;

DWORD last_tooltip_update = 0;

HHOOK mouse_hook;


void debug_printf(LPCTSTR msg, ...) {
#ifdef _DEBUG
  va_list args;
  constexpr size_t buff_size = 2048;
  wchar_t buff[buff_size];
  va_start(args, msg);
  StringCbVPrintfExW((wchar_t *)&buff, buff_size, NULL, NULL,
                     STRSAFE_FILL_BEHIND_NULL, msg, args);
  va_end(args);
  OutputDebugString((wchar_t *)&buff);
#endif
}

void print_last_error(void) {
#ifdef _DEBUG
  DWORD err = GetLastError();
  LPWSTR msg;
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, 0,
                err, NULL, (LPWSTR)&msg, 255, NULL);
  debug_printf(L"Error: %s", msg);
  LocalFree(msg);
#endif
}

// Undocumented stuff to enable dark Mode
// whatch this https://github.com/microsoft/WindowsAppSDK/issues/41 I guess
typedef enum _PreferredAppMode {
  Default,
  AllowDark,
  ForceDark,
  ForceLight,
  Max
} PreferredAppMode;

typedef PreferredAppMode(WINAPI *SetPreferredAppModeFunc)(PreferredAppMode);

void enableDarkMode() {
  HMODULE hUxtheme =
      LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (hUxtheme) {
    auto setPreferredAppMode = (SetPreferredAppModeFunc)GetProcAddress(
        hUxtheme, MAKEINTRESOURCEA(135));
    if (setPreferredAppMode) {
      setPreferredAppMode(AllowDark);
    }
    FreeLibrary(hUxtheme);
  }
}

bool isDarkModeEnabled() {
  DWORD value = 1; // default to light
  HKEY hKey;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    DWORD size = sizeof(value);
    RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                     (LPBYTE)&value, &size);
    RegCloseKey(hKey);
  }
  return value == 0; // 0 = dark mode
}

int __stdcall monitor_enum_proc(HMONITOR hMonitor, HDC _device_context,
                                LPRECT clip, LPARAM data) {
  debug_printf(L"Hello from monitor_enum_proc\n");
  DWORD numPhysicalMonitors = 0;
  if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor,
                                               &numPhysicalMonitors)) {
    return false;
  }
  debug_printf(L"Found %I32d Displays\n", numPhysicalMonitors);
  auto monitor_handles = std::unique_ptr<PHYSICAL_MONITOR>(
      new PHYSICAL_MONITOR[numPhysicalMonitors]);
  if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysicalMonitors,
                                       monitor_handles.get())) {
    return false;
  }
  debug_printf(L"Got Monitors\n");
  for (DWORD i = 0; i < numPhysicalMonitors; i++) {
    debug_printf(L"%s\n",
                 monitor_handles.get()[i].szPhysicalMonitorDescription);
    monitor_handle = monitor_handles.get()[i].hPhysicalMonitor;
    return true;
  }
  return false;
}

bool get_monitor_handle() {
  return EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, 0);
}

bool get_monitor_brightness() {
  if (monitor_handle == NULL && !get_monitor_handle()) {
    debug_printf(L"Failed to get Monitor");
    return false;
  }

  return GetMonitorBrightness(monitor_handle, &min_brightness, &current_brightness,
                              &max_brightness);
}

bool set_monitor_brightness(int brightness) {
  if (monitor_handle == NULL && !get_monitor_handle()) {
    debug_printf(L"Failed to get Monitor");
    return false;
  }

  current_brightness = max(0, min(brightness, 100));
  return SetMonitorBrightness(monitor_handle, current_brightness);
}

void set_monitor_brightness_tooltip(int brightness) {
  if (!set_monitor_brightness(brightness))
    monitor_handle = NULL;
  StringCbPrintfW(structNID.szTip, 128, L"SolTray (%d%%)", current_brightness);
  if (!Shell_NotifyIconW(NIM_MODIFY, &structNID)) {
    print_last_error();
    return;
  }
  last_tooltip_update = timeGetTime();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT Message, WPARAM wParam,
    LPARAM lParam) {
  POINT lpClickPoint;
  LPNMHDR lpNmhdr;

  if (Message == WM_TASKBAR_CREATE) {
    if (!Shell_NotifyIconW(NIM_ADD, &structNID)) {
      return -4;
    }
  }

  switch (Message) {
  case WM_DESTROY:
    Shell_NotifyIconW(NIM_DELETE, &structNID);
    UnhookWindowsHookEx(mouse_hook);
    PostQuitMessage(0);
    break;
  case WM_MOUSEWHEEL:
    debug_printf(L"Scroling\n");
    break;
  case WM_USER_SHELLICON:
    switch (LOWORD(lParam)) {
    case WM_CONTEXTMENU: {
      GetCursorPos(&lpClickPoint);
      HMENU hMenu = LoadMenuW(hInst, MAKEINTRESOURCEW(IDR_MENU));
      if (!hMenu) {
        print_last_error();
        return -5;
      }

      HMENU hSubMenu = GetSubMenu(hMenu, 0);
      if (!hSubMenu) {
        print_last_error();
        DestroyMenu(hMenu);
        return -6;
      }

      SetForegroundWindow(hWnd);
      TrackPopupMenu(hSubMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_BOTTOMALIGN,
                     lpClickPoint.x, lpClickPoint.y, 0, hWnd, NULL);
      SendMessageW(hWnd, WM_NULL, 0, 0);

      DestroyMenu(hMenu);
    } break;
    case WM_LBUTTONDOWN:
      set_monitor_brightness_tooltip(current_brightness > 0 ? 0 : 100);
      break;
    case WM_MOUSEMOVE:
    {
      //DWORD current_time = timeGetTime();
      //if (current_time - last_tooltip_update > 100) {
      //  if (!get_monitor_brightness())
      //    monitor_handle = NULL;
      //  StringCbPrintfW(structNID.szTip, 128, L"SolTray (%d%%)",
      //                  current_brightness);
      //  if (!Shell_NotifyIconW(NIM_MODIFY, &structNID)) {
      //    print_last_error();
      //    return -7;
      //  }
      //  last_tooltip_update = current_time;
      //}
      break;
    }
    default:
      debug_printf(L"0x%x\n", lParam);
      break;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(hWnd);
    break;
  case WM_COMMAND:
    switch (wParam) {
    case ID_POPUP_QUIT:
      DestroyWindow(hwnd);
      break;
    case ID_POPUP_0:
      set_monitor_brightness_tooltip(0);
      break;
    case ID_POPUP_25:
      set_monitor_brightness_tooltip(25);
      break;
    case ID_POPUP_50:
      set_monitor_brightness_tooltip(50);
      break;
    case ID_POPUP_75:
      set_monitor_brightness_tooltip(75);
      break;
    case ID_POPUP_100:
      set_monitor_brightness_tooltip(100);
      break;
    default:
      break;
    }
    break;
  default:
    return DefWindowProcW(hwnd, Message, wParam, lParam);
  }
  return 0;
}

LRESULT CALLBACK cbMouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0) {
    return CallNextHookEx(NULL, nCode, wParam, lParam);
  }

  if (nCode == 0 && LOWORD(wParam) == WM_MOUSEWHEEL) {
    RECT iconLocation;
    HRESULT result = Shell_NotifyIconGetRect(&idNID, &iconLocation);
    if (result == S_OK) {
      POINT mouse_pos;
      GetCursorPos(&mouse_pos);
      if (PtInRect(&iconLocation, mouse_pos)) {
        auto info = (LPMSLLHOOKSTRUCT) lParam;
        int zDelta = GET_WHEEL_DELTA_WPARAM(info->mouseData) / 24;
        set_monitor_brightness_tooltip(current_brightness + zDelta);
      }
    }
  }

  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  auto hMutexInstance = CreateMutexW(NULL, FALSE, L"SolTrayMutex");
  if (hMutexInstance == NULL && (GetLastError() == ERROR_ALREADY_EXISTS ||
                                 GetLastError() == ERROR_ACCESS_DENIED)) {
    return 0;
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  hInst = hInstance;

  WNDCLASSEXW wc{.cbSize = sizeof(WNDCLASSEXW),
                 .style = CS_HREDRAW | CS_VREDRAW,
                 .lpfnWndProc = WndProc,
                 .cbClsExtra = NULL,
                 .cbWndExtra = NULL,
                 .hInstance = hInstance,
                 .hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_SOLTRAY)),
                 .hCursor = LoadCursorW(NULL, IDC_ARROW),
                 .hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH),
                 .lpszMenuName = NULL,
                 .lpszClassName = L"SolTray",
                 .hIconSm = LoadIcon(hInstance, MAKEINTRESOURCEW(IDI_SOLTRAY))};

  if (!RegisterClassExW(&wc)) {
    print_last_error();
    return -1;
  }

  hWnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"SolTray", L"SolTray",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance,
                         NULL);
  if (hWnd == NULL) {
    print_last_error();
    return -2;
  }

  enableDarkMode();

  GUID nidGUID = {0xffe22b0e, 0xa792, 0x4c81, {0x9b, 0x20, 0xeb, 0x8e, 0x8f, 0x26, 0x1e, 0x41}};

  int icon_id = isDarkModeEnabled() ? IDI_SOLTRAY_DARK : IDI_SOLTRAY;

  structNID = {
      .cbSize = sizeof(NOTIFYICONDATAW),
      .hWnd = hWnd,
      .uID = IDI_SOLTRAY,
      .uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | NIF_GUID,
      .uCallbackMessage = WM_USER_SHELLICON,
      .hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(icon_id)),
      .szTip = L"SolTray Tip",
      .uVersion = NOTIFYICON_VERSION_4,
      .guidItem = nidGUID,
  };

  if (!Shell_NotifyIconW(NIM_ADD, &structNID)) {
    print_last_error();
    return -3;
  }

  if (!Shell_NotifyIconW(NIM_SETVERSION, &structNID)) {
    print_last_error();
    return -8;
  }

  idNID = {
      .cbSize = sizeof(NOTIFYICONIDENTIFIER),
      .guidItem = nidGUID,
  };

  if (!get_monitor_brightness())
    monitor_handle = NULL;


  StringCbPrintfW(structNID.szTip, 128, L"SolTray (%d%%)", current_brightness);
  if (!Shell_NotifyIconW(NIM_MODIFY, &structNID)) {
    print_last_error();
    return -7;
  }
  last_tooltip_update = timeGetTime();

  mouse_hook =
      SetWindowsHookExW(WH_MOUSE_LL, cbMouseHook, NULL, NULL);
  if (mouse_hook == NULL) {
    print_last_error();
    return -9;
  }


  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return msg.wParam;
}   