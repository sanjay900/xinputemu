// gip_probe: checks whether Xbox One (GIP) guitars are reachable from a 32-bit process
//
// 1. Lists DOS device names that look GIP/Xbox related and tries to open + read \\.\XboxGIP
// 2. Polls Windows.Gaming.Input RawGameController and prints any input changes
//
// WGI only delivers input to the foreground process, so keep the "gip_probe" window focused.

#include <initguid.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "windows.gaming.input.h"

#define MAX_CONTROLLERS 8
#define MAX_BUTTONS 64
#define MAX_SWITCHES 8
#define MAX_AXES 16

typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameController IRawGameController;
typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameController2 IRawGameController2;
typedef __x_ABI_CWindows_CGaming_CInput_CIRawGameControllerStatics IRawGameControllerStatics;
typedef __FIVectorView_1_Windows__CGaming__CInput__CRawGameController IRawGameControllerVectorView;
typedef __x_ABI_CWindows_CGaming_CInput_CGameControllerSwitchPosition SwitchPosition;

struct wgi_controller
{
    IRawGameController *ctrl;
    UINT32 buttons, switches, axes;
    boolean prev_buttons[MAX_BUTTONS];
    SwitchPosition prev_switches[MAX_SWITCHES];
    DOUBLE prev_axes[MAX_AXES];
    BOOL have_prev;
};

static IRawGameControllerStatics *wgi_statics;
static struct wgi_controller wgi_controllers[MAX_CONTROLLERS];
static UINT32 wgi_count;

static void log_time(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

static void print_hstring(const char *label, HSTRING str)
{
    UINT32 len = 0;
    const WCHAR *raw = WindowsGetStringRawBuffer(str, &len);
    printf("    %s: %ls\n", label, raw ? raw : L"(null)");
}

// ---- \\.\XboxGIP ----

static void list_gip_dos_devices(void)
{
    DWORD size = 1 << 20;
    WCHAR *buf = malloc(size * sizeof(WCHAR));
    DWORD len = QueryDosDeviceW(NULL, buf, size);
    if (!len)
    {
        printf("QueryDosDevice failed: %lu\n", GetLastError());
        free(buf);
        return;
    }
    printf("DOS device names containing 'gip' or 'xbox':\n");
    for (WCHAR *name = buf; *name; name += wcslen(name) + 1)
    {
        WCHAR lower[512];
        lstrcpynW(lower, name, 512);
        CharLowerW(lower);
        if (wcsstr(lower, L"gip") || wcsstr(lower, L"xbox"))
        {
            WCHAR target[512] = L"";
            QueryDosDeviceW(name, target, 512);
            printf("  \\\\.\\%ls -> %ls\n", name, target);
        }
    }
    free(buf);
}

static HANDLE open_gip(void)
{
    static const struct
    {
        DWORD access;
        const char *name;
    } modes[] = {
        {GENERIC_READ | GENERIC_WRITE, "read/write"},
        {GENERIC_READ, "read"},
        {0, "no access (query only)"},
    };
    for (int i = 0; i < 3; i++)
    {
        HANDLE h = CreateFileW(L"\\\\.\\XboxGIP", modes[i].access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            printf("Opened \\\\.\\XboxGIP with %s access\n", modes[i].name);
            if (modes[i].access & GENERIC_READ)
                return h;
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        printf("Open \\\\.\\XboxGIP with %s access failed: error %lu\n", modes[i].name, GetLastError());
    }
    return INVALID_HANDLE_VALUE;
}

// Format from https://gist.github.com/TheNathannator/bcebc77e653f71e77634144940871596
#define GIP_ADD_REENUMERATE_CALLER_CONTEXT 0x40001CD0
#define GIP_HEADER_SIZE 20

static void gip_print_arrival(const BYTE *buf, DWORD got)
{
    const BYTE *p = buf + GIP_HEADER_SIZE;
    if (got < GIP_HEADER_SIZE + 20)
        return;
    printf("    arrival: VID %04x PID %04x firmware %u.%u.%u.%u\n", *(UINT16 *)(p + 8), *(UINT16 *)(p + 10),
           *(UINT16 *)(p + 12), *(UINT16 *)(p + 14), *(UINT16 *)(p + 16), *(UINT16 *)(p + 18));
}

static void gip_print_descriptor(const BYTE *buf, DWORD got)
{
    const BYTE *p = buf + GIP_HEADER_SIZE;
    DWORD len = got - GIP_HEADER_SIZE;
    if (got < GIP_HEADER_SIZE + 36)
        return;
    // Block offsets are relative to the first offset field, which sits 20 bytes into the descriptor
    const BYTE *base = p + 20;
    DWORD base_len = len - 20;
    const UINT16 *offsets = (const UINT16 *)base;
    printf("    descriptor: VID %04x PID %04x\n", *(UINT16 *)p, *(UINT16 *)(p + 2));

    UINT16 off = offsets[5];
    if (off && off < base_len)
    {
        BYTE count = base[off];
        DWORD pos = off + 1;
        for (BYTE i = 0; i < count && pos + 2 <= base_len; i++)
        {
            UINT16 slen = *(UINT16 *)(base + pos);
            pos += 2;
            if (pos + slen > base_len)
                break;
            printf("    class name: %.*s\n", slen, (const char *)(base + pos));
            pos += slen;
        }
    }

    off = offsets[6];
    if (off && off < base_len)
    {
        BYTE count = base[off];
        for (BYTE i = 0; i < count && off + 1 + (i + 1) * 16 <= base_len; i++)
        {
            const BYTE *g = base + off + 1 + i * 16;
            printf("    interface: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n", (unsigned long)*(UINT32 *)g,
                   *(UINT16 *)(g + 4), *(UINT16 *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        }
    }

    off = offsets[3];
    if (off && off < base_len)
    {
        printf("    input commands:");
        for (BYTE i = 0; i < base[off] && off + 1 + i < base_len; i++)
            printf(" %02x", base[off + 1 + i]);
        printf("\n");
    }
}

static DWORD WINAPI gip_read_thread(void *param)
{
    HANDLE h = param;
    BYTE buf[4096];
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD packets = 0;
    BYTE last_input[4096];
    DWORD last_input_len = 0;
    BOOL reenumerated = FALSE;

    for (;;)
    {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(h, buf, sizeof(buf), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
        {
            log_time();
            printf("GIP ReadFile failed: error %lu\n", GetLastError());
            break;
        }
        // Ask the driver to resend arrival/descriptor messages for already connected devices,
        // once a read is pending so they aren't missed
        if (!reenumerated)
        {
            OVERLAPPED ioctl_ov = {0};
            DWORD unused;
            ioctl_ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            BOOL ok = DeviceIoControl(h, GIP_ADD_REENUMERATE_CALLER_CONTEXT, NULL, 0, NULL, 0, NULL, &ioctl_ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = GetOverlappedResult(h, &ioctl_ov, &unused, TRUE);
            log_time();
            if (ok)
                printf("GIP re-enumerate requested\n");
            else
                printf("GIP re-enumerate failed: error %lu\n", GetLastError());
            CloseHandle(ioctl_ov.hEvent);
            reenumerated = TRUE;
        }
        while (WaitForSingleObject(ov.hEvent, 5000) == WAIT_TIMEOUT)
        {
            log_time();
            printf("GIP: no data for 5s (%lu packets so far)\n", packets);
        }
        if (!GetOverlappedResult(h, &ov, &got, FALSE))
        {
            log_time();
            printf("GIP read completed with error %lu\n", GetLastError());
            break;
        }
        packets++;
        // Input packets (0x20) stream constantly, and payload bytes 10-13 are sensor noise on the Riffmaster,
        // so only print input packets when something else changed
        if (got >= 20 && buf[8] == 0x20)
        {
            BOOL changed = got != last_input_len;
            for (DWORD i = 20; i < got && !changed; i++)
                if ((i < 30 || i > 33) && buf[i] != last_input[i])
                    changed = TRUE;
            memcpy(last_input, buf, got);
            last_input_len = got;
            if (!changed)
                continue;
        }
        log_time();
        printf("GIP %lu bytes:", got);
        for (DWORD i = 0; i < got; i++)
            printf(" %02x", buf[i]);
        printf("\n");
        if (got >= GIP_HEADER_SIZE && buf[8] == 0x02)
            gip_print_arrival(buf, got);
        else if (got >= GIP_HEADER_SIZE && buf[8] == 0x04)
            gip_print_descriptor(buf, got);
    }
    CloseHandle(ov.hEvent);
    return 0;
}

// ---- Windows.Gaming.Input ----

static BOOL wgi_init(void)
{
    HSTRING_HEADER header;
    HSTRING name;
    HRESULT hr = WindowsCreateStringReference(RuntimeClass_Windows_Gaming_Input_RawGameController,
                                              wcslen(RuntimeClass_Windows_Gaming_Input_RawGameController), &header, &name);
    if (SUCCEEDED(hr))
        hr = RoGetActivationFactory(name, &IID___x_ABI_CWindows_CGaming_CInput_CIRawGameControllerStatics, (void **)&wgi_statics);
    if (FAILED(hr))
    {
        printf("RawGameController statics unavailable: 0x%08lx\n", hr);
        return FALSE;
    }
    return TRUE;
}

static void wgi_describe(struct wgi_controller *c, UINT32 index)
{
    UINT16 vid = 0, pid = 0;
    IRawGameController2 *c2;
    c->ctrl->lpVtbl->get_HardwareVendorId(c->ctrl, &vid);
    c->ctrl->lpVtbl->get_HardwareProductId(c->ctrl, &pid);
    c->ctrl->lpVtbl->get_ButtonCount(c->ctrl, (INT32 *)&c->buttons);
    c->ctrl->lpVtbl->get_SwitchCount(c->ctrl, (INT32 *)&c->switches);
    c->ctrl->lpVtbl->get_AxisCount(c->ctrl, (INT32 *)&c->axes);
    log_time();
    printf("WGI controller %u: VID %04x PID %04x, %u buttons, %u switches, %u axes\n", index, vid, pid, c->buttons,
           c->switches, c->axes);
    if (SUCCEEDED(c->ctrl->lpVtbl->QueryInterface(c->ctrl, &IID___x_ABI_CWindows_CGaming_CInput_CIRawGameController2,
                                                  (void **)&c2)))
    {
        HSTRING str;
        if (SUCCEEDED(c2->lpVtbl->get_DisplayName(c2, &str)))
        {
            print_hstring("name", str);
            WindowsDeleteString(str);
        }
        if (SUCCEEDED(c2->lpVtbl->get_NonRoamableId(c2, &str)))
        {
            print_hstring("id", str);
            WindowsDeleteString(str);
        }
        c2->lpVtbl->Release(c2);
    }
    if (c->buttons > MAX_BUTTONS)
        c->buttons = MAX_BUTTONS;
    if (c->switches > MAX_SWITCHES)
        c->switches = MAX_SWITCHES;
    if (c->axes > MAX_AXES)
        c->axes = MAX_AXES;
}

static void wgi_refresh(void)
{
    IRawGameControllerVectorView *list;
    UINT32 count = 0;
    if (FAILED(wgi_statics->lpVtbl->get_RawGameControllers(wgi_statics, &list)))
        return;
    list->lpVtbl->get_Size(list, &count);
    if (count > MAX_CONTROLLERS)
        count = MAX_CONTROLLERS;

    for (UINT32 i = 0; i < count; i++)
    {
        IRawGameController *ctrl;
        if (FAILED(list->lpVtbl->GetAt(list, i, &ctrl)))
            continue;
        if (i < wgi_count && wgi_controllers[i].ctrl == ctrl)
        {
            ctrl->lpVtbl->Release(ctrl);
            continue;
        }
        if (i < wgi_count)
            wgi_controllers[i].ctrl->lpVtbl->Release(wgi_controllers[i].ctrl);
        memset(&wgi_controllers[i], 0, sizeof(wgi_controllers[i]));
        wgi_controllers[i].ctrl = ctrl;
        wgi_describe(&wgi_controllers[i], i);
    }
    for (UINT32 i = count; i < wgi_count; i++)
    {
        log_time();
        printf("WGI controller %u removed\n", i);
        wgi_controllers[i].ctrl->lpVtbl->Release(wgi_controllers[i].ctrl);
        wgi_controllers[i].ctrl = NULL;
    }
    wgi_count = count;
    list->lpVtbl->Release(list);
}

static void wgi_poll(void)
{
    for (UINT32 i = 0; i < wgi_count; i++)
    {
        struct wgi_controller *c = &wgi_controllers[i];
        boolean buttons[MAX_BUTTONS] = {0};
        SwitchPosition switches[MAX_SWITCHES] = {0};
        DOUBLE axes[MAX_AXES] = {0};
        UINT64 timestamp;
        if (FAILED(c->ctrl->lpVtbl->GetCurrentReading(c->ctrl, c->buttons, buttons, c->switches, switches, c->axes,
                                                      axes, &timestamp)))
            continue;
        if (!c->have_prev)
        {
            log_time();
            printf("WGI %u initial axes:", i);
            for (UINT32 a = 0; a < c->axes; a++)
                printf(" %.3f", axes[a]);
            printf("\n");
        }
        else
        {
            for (UINT32 b = 0; b < c->buttons; b++)
                if (buttons[b] != c->prev_buttons[b])
                {
                    log_time();
                    printf("WGI %u button %u %s\n", i, b, buttons[b] ? "down" : "up");
                }
            for (UINT32 s = 0; s < c->switches; s++)
                if (switches[s] != c->prev_switches[s])
                {
                    log_time();
                    printf("WGI %u switch %u = %d\n", i, s, switches[s]);
                }
            for (UINT32 a = 0; a < c->axes; a++)
                if (axes[a] - c->prev_axes[a] > 0.02 || c->prev_axes[a] - axes[a] > 0.02)
                {
                    log_time();
                    printf("WGI %u axis %u = %.3f\n", i, a, axes[a]);
                }
        }
        memcpy(c->prev_buttons, buttons, sizeof(buttons));
        memcpy(c->prev_switches, switches, sizeof(switches));
        memcpy(c->prev_axes, axes, sizeof(axes));
        c->have_prev = TRUE;
    }
}

// ---- window ----

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static DWORD ticks;
    switch (msg)
    {
    case WM_TIMER:
        if (wgi_statics)
        {
            if (ticks++ % 50 == 0)
                wgi_refresh();
            wgi_poll();
        }
        return 0;
    case WM_ACTIVATE:
        log_time();
        printf("window %s focus\n", LOWORD(wparam) == WA_INACTIVE ? "lost" : "gained");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("gip_probe (%d-bit)\n\n", (int)sizeof(void *) * 8);

    list_gip_dos_devices();
    printf("\n");
    HANDLE gip = open_gip();
    if (gip != INVALID_HANDLE_VALUE)
        CloseHandle(CreateThread(NULL, 0, gip_read_thread, gip, 0, NULL));
    printf("\n");

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr))
        printf("RoInitialize failed: 0x%08lx\n", hr);
    else
        wgi_init();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"gip_probe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"gip_probe", L"gip_probe - keep this window focused", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 480, 160, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, 1, 20, NULL);
    printf("Polling WGI every 20ms. Close the window to exit.\n");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
