#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "hidsdi.h"
#include "dumbxinputemu.h"
#include <cguid.h>
#include "isxinput.h"

#include <stdio.h>

#define DIRECTINPUT_VERSION 0x0800
// #define DEBUG
#include "dinput.h"

#ifndef TRACE
// Available only in Wine
#define TRACE(format, ...)                                    \
    do                                                        \
    {                                                         \
        printf("TRACE[%d] " format, __LINE__, ##__VA_ARGS__); \
        fflush(stdout);                                       \
    } while (0)
#define DPRINT(format, ...)                                 \
    do                                                      \
    {                                                       \
        printf("ERR[%d] " format, __LINE__, ##__VA_ARGS__); \
        fflush(stdout);                                     \
    } while (0)
// #define TRACE(...) do { } while(0)
// #define DPRINT(...) do { } while(0)
#define FIXME(...) \
    do             \
    {              \
    } while (0)
#define WARN(...) \
    do            \
    {             \
    } while (0)
#define ERR(format, ...)                                    \
    do                                                      \
    {                                                       \
        printf("ERR[%d] " format, __LINE__, ##__VA_ARGS__); \
        fflush(stdout);                                     \
    } while (0)
#endif

struct CapsFlags
{
    BOOL wireless, jedi, pov, crkd, santroller, ps3rb, ps4rb, ps2gh, ps2, ps5rb, ps3gh, ps2needschecking, rb360, gh360, windows, macos, raphwii, raphpsx, seenwhammy, sdl;
    int axes, buttons, subtype;
};

static struct ControllerMap
{
    LPDIRECTINPUTDEVICE8A device;
    BOOL connected, acquired;
    struct CapsFlags caps;
    XINPUT_STATE_EX state_ex;
    XINPUT_VIBRATION vibration;
    BOOL vibration_dirty;

    DIEFFECT effect_data;
    LPDIRECTINPUTEFFECT effect_instance;
} controllers[XUSER_MAX_COUNT];

static struct
{
    LPDIRECTINPUT8A iface;
    BOOL enabled;
    int mapped;
} dinput;

/* ========================= Internal functions ============================= */

static bool initialized = FALSE;
static void dinput_start(void);
static void dinput_update(int index);
static bool dirExists(LPCWSTR path)
{
    DWORD attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

static bool IsMacos()
{
    LPCWSTR library = L"Z:\\Library";
    if (dirExists(library))
    {
        return true;
    }
}

static bool KeyExists(HKEY hKeyRoot, LPCWSTR subKey)
{
    HKEY hKey;
    LONG result = RegOpenKeyExW(hKeyRoot, subKey, 0, KEY_READ, &hKey);

    if (result == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return true;
    }
    else
    {
        return false;
    }
}

static bool SDLEnabled()
{
    uint32_t data;
    size_t size = sizeof(data);
    HKEY hKey;
    LONG result = RegGetValueW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WineBus", L"Enable SDL", RRF_RT_DWORD, NULL, (LPDWORD)&data, (LPDWORD)&size);

    if (result == ERROR_SUCCESS)
    {
        return data;
    }
    else
    {
        // SDL enabled by default
        return true;
    }
}

static BOOL dinput_is_good(const LPDIRECTINPUTDEVICE8A device, struct CapsFlags *caps, const GUID *guidProduct)
{
    HRESULT hr;
    DIDEVCAPS dinput_caps;
    static const unsigned long wireless_products[] = {
        MAKELONG(0x045e, 0x0291) /* microsoft receiver */,
        MAKELONG(0x045e, 0x02a9) /* microsoft receiver (3rd party) */,
        MAKELONG(0x045e, 0x02a1) /* microsoft controller (xpad) */,
        MAKELONG(0x045e, 0x0719) /* microsoft controller */,
        MAKELONG(0x28de, 0x11ff) /* steam input virtual controller */,
        MAKELONG(0x0738, 0x4556) /* mad catz */,
        MAKELONG(0x0e6f, 0x0003) /* logitech */,
        MAKELONG(0x0e6f, 0x0005) /* eclipse */,
        MAKELONG(0x0e6f, 0x0006) /* edge */,
        MAKELONG(0x102c, 0xff0c) /* joytech */
    };
    static const unsigned long ps4_products[] = {
        MAKELONG(0x0e6f, 0x024a) /* PS4 Riffmaster */,
        MAKELONG(0x0e6f, 0x0173) /* PDP Jaguar */,
        MAKELONG(0x0738, 0x8261) /* MadCatz Stratocaster */,
        MAKELONG(0x3651, 0x5500) /* PS4 CRKD SG - Dongle */,
        MAKELONG(0x3651, 0x1500) /* PS4 CRKD SG - Wired */
    };
    static const unsigned long ps5_products[] = {
        MAKELONG(0x0e6f, 0x0249) /* PS5 Riffmaster */,
        MAKELONG(0x3651, 0x5600) /* PS5 CRKD SG - Dongle */,
        MAKELONG(0x3651, 0x1600) /* PS5 CRKD SG - Wired */
    };
    static const unsigned long raphnet_wii_products[] = {
        MAKELONG(0x289b, 0x0080) /* 1-player WUSBMote v2.2 (w/advXarch) */,
        MAKELONG(0x289b, 0x0081) /* 2-player WUSBMote v2.2 (w/advXarch) */,
        MAKELONG(0x289b, 0x0028) /* 1-player WUSBMote v2.0 (w/advXarch) */,
        MAKELONG(0x289b, 0x0029) /* 2-player WUSBMote v2.0 (w/advXarch) */,
        MAKELONG(0x289b, 0x002B) /* 1-player WUSBMote v2.1 (w/advXarch) */,
        MAKELONG(0x289b, 0x002C) /* 2-player WUSBMote v2.1 (w/advXarch) */,
    };
    static const unsigned long raphnet_ps2_products[] = {
        MAKELONG(0x289b, 0x0044) /* PS1/PS2 controller to USB adapter (w/advXarch) */,
        MAKELONG(0x289b, 0x0045) /* PS1/PS2 controller to USB adapter (2-player mode) */,
        MAKELONG(0x289b, 0x0046) /* PS1/PS2 controller to USB adapter (3-player mode) */,
        MAKELONG(0x289b, 0x0047) /* PS1/PS2 controller to USB adapter (4-player mode) */,
    };
    static const unsigned long rb_ps3_products[] = {
        MAKELONG(0x12BA, 0x0200) /* Harmonix Guitar for PlayStation®3 */,
        MAKELONG(0x1BAD, 0x0004) /* Harmonix Guitar Controller for Nintendo Wii (RB1) */,
        MAKELONG(0x1BAD, 0x3010) /* Harmonix Guitar Controller for Nintendo Wii (RB2+) */,
    };
    static const unsigned long gh_ps3_products[] = {
        MAKELONG(0x12BA, 0x0100) /* Guitar Hero3 for PlayStation (R) 3 */,
        MAKELONG(0x1430, 0x474C) /* Guitar Hero for PC/MAC */,
    };
    static const unsigned long gh_xinput_products[] = {
        MAKELONG(0x1430, 0x4734) /* World Tour Kiosk */,
        MAKELONG(0x3651, 0x1000) /* CRKD SG Legacy Mode */,
        MAKELONG(0x0351, 0x1000) /* CRKD Xbox Black Tribal PC Mode */,
        MAKELONG(0x0351, 0x2000) /* CRKD Xbox Blueberry Burst PC Mode */,
        MAKELONG(0x1430, 0x4748) /* Xplorer */,
        MAKELONG(0x1430, 0x0705) /* GH5 Guitar */,
        MAKELONG(0x1430, 0x0706) /* WoR Guitar */,
    };
    static const unsigned long rb_xinput_products[] = {
        MAKELONG(0x1BAD, 0x0002) /* RB Stratocaster */,
        MAKELONG(0x0738, 0x9806) /* RB Precision Bass */
    };
    caps->windows = !(KeyExists(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Wine") || KeyExists(HKEY_CURRENT_USER, L"Software\\Wine"));
    caps->macos = IsMacos();
    if (caps->windows)
    {
        TRACE("Running on windows\r\n");
    }
    else
    {
        TRACE("Running on wine\r\n");
        if (caps->macos)
        {
            TRACE("Running on macos\r\n");
            // macos doesn't use sdl style mappings for santroller things
            caps->sdl = false;
        }
        else
        {
            TRACE("Running on linux\r\n");
            caps->sdl = SDLEnabled();
            if (caps->sdl)
            {
                TRACE("SDL Enabled, you may have problems with tilt\r\n");
            }
            else
            {

                TRACE("SDL Disabled\r\n");
            }
        }
    }

    int i;

    dinput_caps.dwSize = sizeof(dinput_caps);
    hr = IDirectInputDevice_GetCapabilities(device, &dinput_caps);
    if (FAILED(hr))
        return FALSE;

    if (dinput_caps.dwAxes < 2 || dinput_caps.dwButtons < 8)
        return FALSE;
    caps->axes = dinput_caps.dwAxes;
    caps->buttons = dinput_caps.dwButtons;
    caps->wireless = FALSE;
    caps->jedi = !!(dinput_caps.dwFlags & DIDC_FORCEFEEDBACK);
    caps->pov = !!dinput_caps.dwPOVs;
    caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->santroller = false;
    caps->crkd = false;
    caps->ps2 = false;
    caps->ps3rb = false;
    caps->ps4rb = false;
    caps->ps5rb = false;
    caps->ps3gh = false;
    caps->rb360 = false;
    caps->gh360 = false;
    caps->ps2gh = false;
    caps->ps2 = false;
    caps->ps2needschecking = false;
    caps->seenwhammy = false;

    if (dinput_caps.dwAxes == 0x02 && dinput_caps.dwButtons == 0x0a && caps->windows && IsXInputDevice(guidProduct))
    {
        TRACE("Setting subtype to guitar!\n");
        TRACE("Assuming xinput controller with 2 axes and 10 buttons is a gh guitar \n");
        caps->gh360 = true;
        caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    }
    else if (dinput_caps.dwAxes == 0x03 && dinput_caps.dwButtons == 0x0a && caps->windows && IsXInputDevice(guidProduct))
    {
        TRACE("Setting subtype to guitar!\n");
        TRACE("Assuming xinput controller with 3 axes and 10 buttons is a rb guitar \n");
        caps->rb360 = true;
        caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    }
    else if (guidProduct->Data1 == MAKELONG(0x1209, 0x2882))
    {
        TRACE("Setting subtype to guitar!\n");
        TRACE("Santroller hid guitar detected!\n");
        caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
        caps->santroller = true;
    }
    // if (guidProduct->Data1 == MAKELONG(0x045e, 0x028e))
    // {
    //     TRACE("Setting subtype to guitar!\n");
    //     TRACE("CRKD guitar detected!\n");
    //     caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    //     caps->crkd = true;
    // }
    if (guidProduct->Data1 == MAKELONG(0x1BAD, 0x0719))
    {
        TRACE("Setting subtype to guitar!\n");
        TRACE("clipper / rb4instrumentmapper detected!\n");
        caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    }

    if (guidProduct->Data1 == MAKELONG(0x2563, 0x0575))
    {
        TRACE("PS2 usb adapter detected!\n");
        caps->ps2 = true;
        caps->ps2needschecking = true;
    }

    if (guidProduct->Data1 == MAKELONG(0x1BAD, 0x0130))
    {
        TRACE("Setting subtype to drums!\n");
        TRACE("XInput drums detected!\n");
        caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
    }
    TRACE("vidpid: %08x\n", guidProduct->Data1);
    TRACE("vidpid: %08x\n", guidProduct->Data1);
    TRACE("axes: %08x\n", dinput_caps.dwAxes);
    TRACE("buttons: %08x\n", dinput_caps.dwButtons);
    // only force wireless adapters to act as guitars on
    if (!caps->windows)
    {
        for (i = 0; i < sizeof(wireless_products) / sizeof(wireless_products[0]); i++)
        {
            if (guidProduct->Data1 == wireless_products[i])
            {
                caps->wireless = TRUE;
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                break;
            }
        }
    }

    for (i = 0; i < sizeof(ps4_products) / sizeof(ps4_products[0]); i++)
    {
        if (guidProduct->Data1 == ps4_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("PS4 guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->ps4rb = true;
            break;
        }
    }

    for (i = 0; i < sizeof(ps5_products) / sizeof(ps5_products[0]); i++)
    {
        if (guidProduct->Data1 == ps5_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("PS5 guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->ps5rb = true;
            break;
        }
    }

    for (i = 0; i < sizeof(raphnet_wii_products) / sizeof(raphnet_wii_products[0]); i++)
    {
        if (guidProduct->Data1 == raphnet_wii_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("Raphnet wusbmote detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->raphwii = true;
            break;
        }
    }

    for (i = 0; i < sizeof(raphnet_ps2_products) / sizeof(raphnet_ps2_products[0]); i++)
    {
        if (guidProduct->Data1 == raphnet_ps2_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("Raphnet ps2 adapter detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->raphpsx = true;
            break;
        }
    }

    for (i = 0; i < sizeof(rb_ps3_products) / sizeof(rb_ps3_products[0]); i++)
    {
        if (guidProduct->Data1 == rb_ps3_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("RB PS3 guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->ps3rb = true;
            break;
        }
    }

    for (i = 0; i < sizeof(gh_ps3_products) / sizeof(gh_ps3_products[0]); i++)
    {
        if (guidProduct->Data1 == gh_ps3_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("GH PS3 guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->ps3gh = true;
            break;
        }
    }

    for (i = 0; i < sizeof(gh_xinput_products) / sizeof(gh_xinput_products[0]); i++)
    {
        if (guidProduct->Data1 == gh_xinput_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("XInput GH guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            // on windows, 360 guitars don't actually show every axis, so we need to skip axis count checks
            caps->gh360 = true;
            break;
        }
    }

    for (i = 0; i < sizeof(rb_xinput_products) / sizeof(rb_xinput_products[0]); i++)
    {
        if (guidProduct->Data1 == rb_xinput_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("XInput RB guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            // on windows, 360 guitars don't actually show every axis, so we need to skip axis count checks
            caps->rb360 = true;
            break;
        }
    }

    return TRUE;
}

static BOOL dinput_set_range(const LPDIRECTINPUTDEVICE8A device)
{
    HRESULT hr;
    DIPROPRANGE property;

    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;
    property.diph.dwObj = 0;
    property.lMin = -32767;
    property.lMax = +32767;

    hr = IDirectInputDevice_SetProperty(device, DIPROP_RANGE, &property.diph);
    if (FAILED(hr))
    {
        WARN("Failed to set axis range (0x%x)\n", hr);
        return FALSE;
    }
    return TRUE;
}

long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    long out = (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    if (out > out_max)
    {
        out = out_max;
    }
    if (out < out_min)
    {
        out = out_min;
    }
    return out;
}

static void dinput_joystate_to_xinput(DIJOYSTATE2 *js, XINPUT_GAMEPAD_EX *gamepad, struct CapsFlags *caps)
{
    static const int xbox_buttons[] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_GUIDE,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB};

    static const int santroller_buttons[] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        0x00,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
        0x00,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        0x00,
        0x00,
        0x00,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_GUIDE,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB};

    static const int ps2_gh_buttons[] = {
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        0x00,
        0x00, // tilt
        0x00, // solo
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        0x00,
        0x00,
        0x00

    };

    static const int ps2_buttons[] = {
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        0x00, // l2
        0x00, // r2
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        0x00,
        0x00,
        0x00

    };

    static const int ps3_buttons[] = {
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        0x00,                      // tilt
        XINPUT_GAMEPAD_LEFT_THUMB, // solo
        0x00,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_GUIDE,
        0x00

    };
    static const int raph_wii_buttons[] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_DPAD_DOWN, // tilt
        XINPUT_GAMEPAD_START,     // solo
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_DPAD_UP,
        0x00,
        0x00,
        0x00,
        0x00

    };
    static const int raph_psx_buttons[] = {
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_BACK,
        0x00,
        0x00,
        0x00, // TILT
        XINPUT_GAMEPAD_A,
        0x00,
        0x00,
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT

    };
    static const int ps4_buttons[] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        0x00,                      // tilt
        XINPUT_GAMEPAD_LEFT_THUMB, // solo
        0x00,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_GUIDE,
        XINPUT_GAMEPAD_START // map p1 to start so clicking in the joystick works

    };
    static const int ps3_gh_buttons[] = {
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        0x00, // tilt
        0x00, // solo
        0x00,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_GUIDE,
        0x00

    };
    int i, buttons;

    gamepad->dwPaddingReserved = 0;
    gamepad->wButtons = 0x0000;
    /* First the D-Pad which is recognized as a POV in dinput */
    if (caps->pov && !caps->raphwii && !caps->raphpsx)
    {
        switch (js->rgdwPOV[0])
        {
        case 0:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            break;
        case 4500:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP; /* fall through */
        case 9000:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            break;
        case 13500:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; /* fall through */
        case 18000:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            break;
        case 22500:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; /* fall through */
        case 27000:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            break;
        case 31500:
            gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP;
        }
    }
    if (caps->ps2needschecking)
    {
        // can only check after we have seen an input
        if (gamepad->wButtons || js->lX || js->lY || js->lZ || js->lRx || js->lRy || js->lRz)
        {
            caps->ps2needschecking = false;
            TRACE("pov: %d\r\n", gamepad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
            if (gamepad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
            {
                caps->ps2gh = true;
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                TRACE("Setting subtype to guitar!\n");
                TRACE("PS2 guitar detected!\n");
            }
        }
    }
    /* Buttons */
    if (caps->ps2gh)
    {
        // gh guitars hold dpad left so we need to mask that out
        gamepad->wButtons &= ~XINPUT_GAMEPAD_DPAD_LEFT;
        buttons = min(caps->buttons, sizeof(ps2_gh_buttons) / sizeof(*ps2_gh_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= ps2_gh_buttons[i];
    }
    else if (caps->ps2)
    {
        buttons = min(caps->buttons, sizeof(ps2_buttons) / sizeof(*ps2_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= ps2_buttons[i];
    }
    else if (caps->raphwii)
    {
        buttons = min(caps->buttons, sizeof(raph_wii_buttons) / sizeof(*raph_wii_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= raph_wii_buttons[i];
    }
    else if (caps->raphpsx)
    {
        buttons = min(caps->buttons, sizeof(raph_psx_buttons) / sizeof(*raph_psx_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= raph_psx_buttons[i];
    }
    else if (caps->ps3rb)
    {
        buttons = min(caps->buttons, sizeof(ps3_buttons) / sizeof(*ps3_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= ps3_buttons[i];
    }
    else if (caps->ps4rb || caps->ps5rb)
    {
        buttons = min(caps->buttons, sizeof(ps4_buttons) / sizeof(*ps4_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= ps4_buttons[i];
    }
    else if (caps->ps3gh)
    {
        buttons = min(caps->buttons, sizeof(ps3_gh_buttons) / sizeof(*ps3_gh_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= ps3_gh_buttons[i];
    }
    else if (caps->santroller && !caps->sdl)
    {
        buttons = min(caps->buttons, sizeof(santroller_buttons) / sizeof(*santroller_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= santroller_buttons[i];
    }
    else
    {
        buttons = min(caps->buttons, sizeof(xbox_buttons) / sizeof(*xbox_buttons));
        for (i = 0; i < buttons; i++)
            if (js->rgbButtons[i] & 0x80)
                gamepad->wButtons |= xbox_buttons[i];
    }
    if (caps->ps2gh)
    {
        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = INT16_MIN;
        gamepad->sThumbRY = 0;
    }
    else if (caps->santroller)
    {
        // Santroller guitars have whammy and slider flipped in their HID reports
        gamepad->sThumbLX = -js->lY;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = js->lX;
        gamepad->sThumbRY = (js->lZ * 2) - 32768;
    }
    else if (caps->ps3rb)
    {
        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = 0;
        gamepad->sThumbRY = 0;
        gamepad->sThumbRX = (js->lZ);
        if (js->rgbButtons[5] & 0x80)
        {
            gamepad->sThumbRY = 32767;
        }
        else
        {
            gamepad->sThumbRY = 0;
        }
    }
    else if (caps->raphwii)
    {

        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRY = 0;
        gamepad->sThumbRX = (js->lZ);
    }
    else if (caps->raphpsx)
    {
        gamepad->sThumbRX = (js->lZ);
        if (js->rgbButtons[8] & 0x80)
        {
            gamepad->sThumbRY = 32767;
        }
        else
        {
            gamepad->sThumbRY = 0;
        }
    }
    else if (caps->ps4rb || caps->ps5rb)
    {
        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = (js->lZ);
        gamepad->sThumbRY = js->lRz;
    }
    else if (caps->ps3gh)
    {
        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = 0;
        gamepad->sThumbRY = 0;
        gamepad->sThumbRX = (js->lZ * 2) - 32768;
        gamepad->sThumbRY = map(js->lRy, 8192, -8192, -32767, 32767);
    }
    else if (caps->crkd)
    {
        gamepad->sThumbLX = 0;
        gamepad->sThumbLY = 0;
        gamepad->sThumbRX = 0;
        gamepad->sThumbRY = 0;
        // CRKD guitars have whammy on the Z axis (LT)
        gamepad->sThumbLX = gamepad->sThumbLY = gamepad->sThumbRY = 0;
        gamepad->sThumbRX = (js->lZ * 2) - 32768;
        gamepad->bLeftTrigger = 0;
    }
    else if (caps->rb360 && caps->windows)
    {
        LONG whammy = js->rglSlider[0];
        if (whammy > INT16_MAX)
        {
            whammy = INT16_MAX;
        }
        if (whammy < INT16_MIN)
        {
            whammy = INT16_MIN;
        }
        if (whammy)
        {
            caps->seenwhammy = true;
        }
        if (!caps->seenwhammy)
        {
            whammy = INT16_MIN;
        }
        /* Axes */
        gamepad->sThumbLX = js->lX;
        gamepad->sThumbLY = -js->lY;
        gamepad->sThumbRX = whammy;
        gamepad->sThumbRY = js->lRz;
    }
    else if (caps->gh360 && caps->windows)
    {
        LONG whammy = js->rglSlider[0];
        if (whammy > INT16_MAX)
        {
            whammy = INT16_MAX;
        }
        if (whammy < INT16_MIN)
        {
            whammy = INT16_MIN;
        }
        if (whammy)
        {
            caps->seenwhammy = true;
        }
        if (!caps->seenwhammy)
        {
            whammy = INT16_MIN;
        }
        /* Axes */
        gamepad->sThumbLX = js->lX;
        gamepad->sThumbLY = -js->lY;
        gamepad->sThumbRX = whammy;
        gamepad->sThumbRY = js->lRz;
    }
    else
    {
        /* Axes */
        gamepad->sThumbLX = js->lX;
        gamepad->sThumbLY = -js->lY;
        if (caps->axes >= 4)
        {
            gamepad->sThumbRX = js->lRx;
            gamepad->sThumbRY = -js->lRy;
        }
        else
        {
            gamepad->sThumbRX = gamepad->sThumbRY = 0;
        }

        /* Both triggers */
        if (caps->axes >= 6)
        {
            gamepad->bLeftTrigger = (255 * (long)(js->lZ + 32767)) / 65535;
            gamepad->bRightTrigger = (255 * (long)(js->lRz + 32767)) / 65535;
        }
        else
            gamepad->bLeftTrigger = gamepad->bRightTrigger = 0;
    }
}

static void dinput_fill_effect(DIEFFECT *effect)
{
    static DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    static LONG direction[2] = {0, 0};

    effect->dwSize = sizeof(effect);
    effect->dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect->dwDuration = INFINITE;
    effect->dwGain = 0;
    effect->dwTriggerButton = DIEB_NOTRIGGER;
    effect->cAxes = sizeof(axes) / sizeof(axes[0]);
    effect->rgdwAxes = axes;
    effect->rglDirection = direction;
}

static void dinput_send_effect(int index, int power)
{
    HRESULT hr;
    DIPERIODIC periodic;
    DIEFFECT *effect = &controllers[index].effect_data;
    LPDIRECTINPUTEFFECT *instance = &controllers[index].effect_instance;

    if (!*instance)
        dinput_fill_effect(effect);

    effect->cbTypeSpecificParams = sizeof(periodic);
    effect->lpvTypeSpecificParams = &periodic;

    periodic.dwMagnitude = power;
    periodic.dwPeriod = DI_SECONDS; /* 1 second */
    periodic.lOffset = 0;
    periodic.dwPhase = 0;

    if (!*instance)
    {
        hr = IDirectInputDevice8_CreateEffect(controllers[index].device, &GUID_Square,
                                              effect, instance, NULL);
        if (FAILED(hr))
        {
            WARN("Failed to create effect (0x%x)\n", hr);
            return;
        }
        if (!*instance)
        {
            WARN("Effect not returned???\n");
            return;
        }

        hr = IDirectInputEffect_SetParameters(*instance, effect, DIEP_AXES | DIEP_DIRECTION | DIEP_NODOWNLOAD);
        if (FAILED(hr))
        {
            // TODO: I may just introduce memory leak
            // IUnknown_Release(*instance);
            *instance = NULL;
            WARN("Failed to configure effect (0x%x)\n", hr);
            return;
        }
    }

    hr = IDirectInputEffect_SetParameters(*instance, effect, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (FAILED(hr))
    {
        WARN("Failed to play effect (0x%x)\n", hr);
        return;
    }
}

static BOOL CALLBACK dinput_enum_callback(const DIDEVICEINSTANCEA *instance, void *context)
{
    LPDIRECTINPUTDEVICE8A device;
    HRESULT hr;

    TRACE("Device %s\n", instance->tszProductName);
    if (strstr(instance->tszProductName, "(js)") != NULL)
    {
        // Skip 'js' devices, use only evdev
        if (getenv("XINPUT_NO_IGNORE_JS") == NULL)
            // ... unless above env. variable is defined
            return DIENUM_CONTINUE;
    }

    if (getenv("XINPUT_IGNORE_EVDEV") != NULL)
    {
        // Skip 'event' devices if asked to
        if (strstr(instance->tszProductName, "(event)") != NULL)
            return DIENUM_CONTINUE;
    }

    if (dinput.mapped == sizeof(controllers) / sizeof(*controllers))
        return DIENUM_STOP;

    hr = IDirectInput_CreateDevice(dinput.iface, &instance->guidInstance, &device, NULL);
    if (FAILED(hr))
        return DIENUM_CONTINUE;

    if (!dinput_is_good(device, &controllers[dinput.mapped].caps, &instance->guidProduct))
    {
        IDirectInput_Release(device);
        return DIENUM_CONTINUE;
    }

    if (!dinput_set_range(device))
    {
        IDirectInput_Release(device);
        return DIENUM_CONTINUE;
    }

    controllers[dinput.mapped].connected = TRUE;
    controllers[dinput.mapped].device = device;
    dinput.mapped++;

    return DIENUM_CONTINUE;
}

static void dinput_start(void)
{
    HRESULT hr;
    if (initialized)
        return;
    initialized = TRUE;
#ifdef DEBUG
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    // Create a new console window.
    if (!AllocConsole())
        return;

    // Set the screen buffer to be larger than normal (this is optional).
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    {
        csbi.dwSize.Y = 1000; // any useful number of lines...
        SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), csbi.dwSize);
    }

    // Redirect "stdin" to the console window.
    if (!freopen("CONIN$", "w", stdin))
        return;

    // Redirect "stderr" to the console window.
    if (!freopen("CONOUT$", "w", stderr))
        return;

    // Redirect "stdout" to the console window.
    if (!freopen("CONOUT$", "w", stdout))
        return;

    // Turn off buffering for "stdout" ("stderr" is unbuffered by default).

    setbuf(stdout, NULL);
#endif

    hr = DirectInput8Create(GetModuleHandleA(NULL), 0x0800, &IID_IDirectInput8A,
                            (void **)&dinput.iface, NULL);
    if (FAILED(hr))
    {
        ERR("Failed to create dinput8 interface, no xinput controller support (0x%x)\n", hr);
        return;
    }

    hr = IDirectInput8_EnumDevices(dinput.iface, DI8DEVCLASS_GAMECTRL,
                                   dinput_enum_callback, NULL, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr))
    {
        ERR("Failed to enumerate dinput8 devices, no xinput controller support (0x%x)\n", hr);
        return;
    }

    dinput.enabled = TRUE;
}

static void dinput_update(int index)
{
    HRESULT hr;
    DIJOYSTATE2 data;
    XINPUT_GAMEPAD_EX gamepad;

    // DPRINT("dinput_update: %d\n", index);

    if (dinput.enabled)
    {
        if (!controllers[index].acquired)
        {
            IDirectInputDevice8_SetDataFormat(controllers[index].device, &c_dfDIJoystick2);
            hr = IDirectInputDevice8_Acquire(controllers[index].device);
            if (FAILED(hr))
            {
                WARN("Failed to acquire game controller (0x%x)\n", hr);
                return;
            }
            controllers[index].acquired = TRUE;
        }

        IDirectInputDevice8_Poll(controllers[index].device);
        hr = IDirectInputDevice_GetDeviceState(controllers[index].device, sizeof(data), &data);
        if (FAILED(hr))
        {
            if (hr == DIERR_INPUTLOST)
                controllers[index].acquired = FALSE;
            WARN("Failed to get game controller state (0x%x)\n", hr);
            return;
        }
        dinput_joystate_to_xinput(&data, &gamepad, &controllers[index].caps);
    }
    else
        memset(&gamepad, 0, sizeof(XINPUT_GAMEPAD_EX));

    if (memcmp(&controllers[index].state_ex.Gamepad, &gamepad, sizeof(XINPUT_GAMEPAD_EX)))
    {
        controllers[index].state_ex.Gamepad = gamepad;
        controllers[index].state_ex.dwPacketNumber++;
    }
}

void dumb_Init(DWORD version)
{
    // Does nothing
}

void dumb_Cleanup()
{
    // Does nothing as well
}

/* ============================ Dll Functions =============================== */

DWORD dumb_XInputGetState(DWORD index, XINPUT_STATE *state, DWORD caller_version)
{
    union
    {
        XINPUT_STATE state;
        XINPUT_STATE_EX state_ex;
    } xinput;
    DWORD ret;

    // TRACE("dumb_XInputGetState: %d\n", index);

    ret = dumb_XInputGetStateEx(index, &xinput.state_ex, caller_version);
    if (ret != ERROR_SUCCESS)
        return ret;

    /* The main difference between this and the Ex version is the media guide button */
    *state = xinput.state;
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD dumb_XInputGetStateEx(DWORD index, XINPUT_STATE_EX *state_ex, DWORD caller_version)
{
    // TRACE("dumb_XInputGetStateEx (%u %p)\n", index, state_ex);

    if (!initialized)
    {
        DPRINT("Force-enabling dumb XInput\n");
        dumb_XInputEnable(TRUE);
    }

    if (index >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].connected)
        return ERROR_DEVICE_NOT_CONNECTED;

    dinput_update(index);
    if (controllers[index].caps.ps2needschecking)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    // broforce does not pass a correct XINPUT_STATE_EX, so only copy the old struct size
    *state_ex = controllers[index].state_ex;

    return ERROR_SUCCESS;
}

DWORD dumb_XInputSetState(DWORD index, XINPUT_VIBRATION *vibration, DWORD caller_version)
{
    TRACE("dumb_XInputSetState (%u %p)\n", index, vibration);

    if (index >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].connected)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Check if we really have to do all the process */
    if (!controllers[index].vibration_dirty &&
        !memcmp(&controllers[index].vibration, vibration, sizeof(*vibration)))
        return ERROR_SUCCESS;

    controllers[index].vibration = *vibration;
    controllers[index].vibration_dirty = !dinput.enabled;

    if (dinput.enabled && controllers[index].caps.jedi)
    {
        int power;
        /* FIXME: we can't set the speed of each motor so do an average */
        power = DI_FFNOMINALMAX * (vibration->wLeftMotorSpeed + vibration->wRightMotorSpeed) / 2 / 0xFFFF;

        TRACE("Vibration left/right speed %d/%d translated to %d\n\n",
              vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed, power);
        dinput_send_effect(index, power);
    }

    return ERROR_SUCCESS;
}

void dumb_XInputEnable(BOOL enable)
{
    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    TRACE("dumb_XInputEnable (%d)\n", enable);
    dinput_start();

    dinput.enabled = enable;
    if (enable)
    {
        int i;
        /* Apply the last vibration status that was sent to the controller
         * while xinput was disabled. */
        for (i = 0; i < sizeof(controllers) / sizeof(*controllers); i++)
        {
            if (controllers[i].connected && controllers[i].vibration_dirty)
                XInputSetState(i, &controllers[i].vibration);
        }
    }
}

/* Not defined anywhere ??? */
#define XINPUT_CAPS_FFB_SUPPORTED 0x0001
#define XINPUT_CAPS_WIRELESS 0x0002
#define XINPUT_CAPS_NO_NAVIGATION 0x0010

DWORD dumb_XInputGetCapabilities(DWORD index, DWORD flags,
                                 XINPUT_CAPABILITIES *capabilities, DWORD caller_version)
{
    TRACE("dumb_XInputGetCapabilities (%u %d %p)\n", index, flags, capabilities);
    if (!initialized)
        dumb_XInputEnable(TRUE);

    if (index >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].connected)
        return ERROR_DEVICE_NOT_CONNECTED;

    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;

    capabilities->Flags = 0;
    if (controllers[index].caps.jedi)
        capabilities->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
    if (!controllers[index].caps.pov)
        capabilities->Flags |= XINPUT_CAPS_NO_NAVIGATION;

    dinput_update(index);

    capabilities->SubType = controllers[index].caps.subtype;
    capabilities->Vibration = controllers[index].vibration;
    capabilities->Gamepad = *(XINPUT_GAMEPAD *)&controllers[index].state_ex.Gamepad;

    return ERROR_SUCCESS;
}

DWORD dumb_XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID *pDSoundRenderGuid,
                                           GUID *pDSoundCaptureGuid, DWORD caller_version)
{
    TRACE("dumb_XInputGetDSoundAudioDeviceGuids");
    if (dwUserIndex > 3)
        return ERROR_DEVICE_NOT_CONNECTED;
    *pDSoundRenderGuid = GUID_NULL;
    *pDSoundCaptureGuid = GUID_NULL;
    return ERROR_SUCCESS;
}

DWORD dumb_XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                              PXINPUT_KEYSTROKE pKeystroke, DWORD caller_version)
{
    FIXME("(index %u, reserved %u, keystroke %p) Stub!\n", dwUserIndex,
          dwReserved, pKeystroke);
    if (dwUserIndex > 3)
        return ERROR_DEVICE_NOT_CONNECTED;
    return ERROR_EMPTY;
}

DWORD dumb_XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                                       XINPUT_BATTERY_INFORMATION *pBatteryInformation, DWORD caller_version)
{
    TRACE("dumb_XInputGetBatteryInformation");
    pBatteryInformation->BatteryLevel = BATTERY_LEVEL_FULL;
    pBatteryInformation->BatteryType = BATTERY_TYPE_WIRED;
    return ERROR_SUCCESS;
}

DWORD dumb_XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId,
                                   UINT *pRenderCount, LPWSTR pCaptureDeviceId,
                                   UINT *pCaptureCount, DWORD caller_version)
{
    TRACE("dumb_XInputGetAudioDeviceIds");
    return ERROR_DEVICE_NOT_CONNECTED;
}
