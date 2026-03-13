#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "hidsdi.h"
#include "dumbxinputemu.h"
#include <cguid.h>
#include <time.h>
#include <stdio.h>
#include <hidapi.h>
#include <hidapi_winapi.h>
#include "ps3.h"
#include "pc_reports.h"
#include "ps4.h"
#include "ps5.h"
#include "raphnet_reports.h"
typedef enum
{
    gamepad,
    gh_guitar,
    rb_guitar,
    gh_drum,
    rb_drum,
    live_guitar,
    pro_keys,
    pro_guitar_squire,
    pro_guitar_mustang,
    turntable,
    stage_kit,
    raphnet_wii,
    raphnet_ps2,
} device_type_t;
typedef enum
{
    xinput,
    ps3,
    ps4,
    ps5,
    santroller,
    xbox360
} console_type_t;
#define DIRECTINPUT_VERSION 0x0800
#define DEBUG
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
#define UP 1 << 0
#define DOWN 1 << 1
#define LEFT 1 << 2
#define RIGHT 1 << 3
static const uint8_t dpad_bindings_reverse[] = {UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT};
struct CapsFlags
{
    BOOL wireless, jedi, pov, windows, macos, sdl, ps2needschecking, ps3thirdparty;
    int axes, buttons, subtype;
    device_type_t device_type;
    console_type_t console_type;
};

static struct ControllerMap
{
    hid_device *device;
    int xinput_index;
    BOOL connected, acquired;
    struct CapsFlags caps;
    XINPUT_STATE state;
    XINPUT_VIBRATION vibration;
    BOOL vibration_dirty;
    time_t last_poke;
} controllers[XUSER_MAX_COUNT];

static struct
{
    BOOL enabled;
    int mapped;
} dinput;

/* ========================= Internal functions ============================= */
typedef DWORD(WINAPI *_XInputGetCapabilities)(
    _In_ DWORD dwUserIndex,
    _In_ DWORD dwFlags,
    _Out_ XINPUT_CAPABILITIES *pCapabilities

);

typedef DWORD(WINAPI *_XInputGetCapabilitiesEx)(
    _In_ DWORD unk,
    _In_ DWORD dwUserIndex,
    _In_ DWORD dwFlags,
    _Out_ XINPUT_CAPABILITIES_EX *pCapabilities

);
typedef DWORD(WINAPI *_XInputGetState)(
    _In_ DWORD dwUserIndex,
    _Out_ XINPUT_STATE *pState

);
typedef DWORD(WINAPI *_XInputGetStateEx)(
    _In_ DWORD dwUserIndex,
    _Out_ XINPUT_STATE *pState);

_XInputGetState ProcXInputGetState;
_XInputGetStateEx ProcXInputGetStateEx;
_XInputGetCapabilities ProcXInputGetCapabilities;
_XInputGetCapabilitiesEx ProcXInputGetCapabilitiesEx;
static bool initialized = FALSE;
static bool override_rb = FALSE;
static bool override_wireless = FALSE;
static void dinput_start(void);
static void dinput_update(int index);
static bool dirExists(LPCWSTR path)
{
    DWORD attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

static bool OverrideRbWithGh()
{

    CHAR Buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, Buffer);
    strcat(Buffer, "\\input_config.ini");
    UINT force_gh_guitar = GetPrivateProfileIntA(
        "overrides",
        "force_gh_guitar",
        0,
        Buffer);
    if (force_gh_guitar)
    {
        printf("Overidding RB guitar subtype with GH guitar subtype");
    }
    return force_gh_guitar;
}

static bool ShowConsole()
{

    CHAR Buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, Buffer);
    strcat(Buffer, "\\input_config.ini");
    UINT show_console = GetPrivateProfileIntA(
        "debug",
        "show_console",
        0,
        Buffer);
    return show_console;
}
static bool OverrideWireless()
{

    CHAR Buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, Buffer);
    strcat(Buffer, "\\input_config.ini");
    UINT override_wireless = GetPrivateProfileIntA(
        "overrides",
        "override_wireless",
        0,
        Buffer);
    if (override_wireless)
    {
        printf("Overidding RB guitar subtype with GH guitar subtype");
    }
    return override_wireless;
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

static bool UpdateKey(HKEY hKeyRoot, LPCWSTR subKey, LPCWSTR name, DWORD val)
{
    HKEY hKey;
    LONG result = RegCreateKeyExW(hKeyRoot, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);

    if (result == ERROR_SUCCESS)
    {
        DWORD type;
        DWORD ret;
        DWORD sz = sizeof(ret);
        result = RegGetValueW(hKeyRoot, subKey, name, 0, &type, (LPBYTE)&ret, &sz);
        if (result == ERROR_FILE_NOT_FOUND || ret != 1)
        {
            RegSetValueExW(hKey, name, 0, REG_DWORD, (LPBYTE)&val, sizeof(val));
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
        return false;
    }
    else
    {
        return false;
    }
}

const char *hid_bus_name(hid_bus_type bus_type)
{
    static const char *const HidBusTypeName[] = {
        "Unknown",
        "USB",
        "Bluetooth",
        "I2C",
        "SPI",
    };

    if ((int)bus_type < 0)
        bus_type = HID_API_BUS_UNKNOWN;
    if ((int)bus_type >= (int)(sizeof(HidBusTypeName) / sizeof(HidBusTypeName[0])))
        bus_type = HID_API_BUS_UNKNOWN;

    return HidBusTypeName[bus_type];
}

static void dinput_fill_caps(struct CapsFlags *caps, long vidpid, uint16_t revision)
{
    int i;
    HRESULT hr;
    DIDEVCAPS dinput_caps;
    static const unsigned long wireless_products[] = {
        MAKELONG(0x045e, 0x0291) /* microsoft receiver */,
        MAKELONG(0x045e, 0x02a9) /* microsoft receiver (3rd party) */,
        MAKELONG(0x045e, 0x02a1) /* microsoft controller (xpad) */,
        MAKELONG(0x28de, 0x11ff) /* steam input virtual controller */,
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
    static const unsigned long gh_ps3_drum_products[] = {
        MAKELONG(0x12BA, 0x0120) /* GuitarHero for Playstation (R) 3 */,
    };
    static const unsigned long rb_ps3_drum_products[] = {
        MAKELONG(0x12BA, 0x0210) /* Harmonix Drum Kit for PlayStation(R)3 */,
        MAKELONG(0x12BA, 0x0218) /* Harmonix Drum Kit for PlayStation(R)3 */,
        MAKELONG(0x1BAD, 0x0005) /* Harmonix Drum Controller for Nintendo Wii */,
        MAKELONG(0x1BAD, 0x3110) /* Harmonix Drum Controller for Nintendo Wii */,
        MAKELONG(0x1BAD, 0x3138) /* Harmonix Drum Controller for Nintendo Wii */,
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
    caps->ps2needschecking = false;
    caps->ps3thirdparty = true;
    caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;

    if (vidpid == MAKELONG(0x1209, 0x2882))
    {
        uint8_t type = revision >> 8;
        caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
        caps->device_type = gh_guitar;
        caps->console_type = santroller;
        switch (revision >> 8)
        {
        case STAGE_KIT:
            caps->subtype = XINPUT_DEVSUBTYPE_STAGE_KIT;
            caps->device_type = stage_kit;
            TRACE("Santroller hid stage kit detected!\n");
            break;
        case GAMEPAD:
            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->device_type = gamepad;
            TRACE("Santroller hid gamepad detected!\n");
            break;
        case GUITAR_HERO_GUITAR:
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->device_type = gh_guitar;
            TRACE("Santroller hid gh guitar detected!\n");
            break;
        case ROCK_BAND_GUITAR:
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
            caps->device_type = rb_guitar;
            TRACE("Santroller hid rb guitar detected!\n");
            if (override_rb)
            {
                printf("Overidding RB guitar subtype with GH guitar subtype");
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            break;
        case GUITAR_HERO_DRUMS:
            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            caps->device_type = gh_drum;
            TRACE("Santroller hid gh drum detected!\n");
            break;
        case ROCK_BAND_DRUMS:
            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            caps->device_type = rb_drum;
            TRACE("Santroller hid rb drum detected!\n");
            break;
        case LIVE_GUITAR:
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->device_type = live_guitar;
            TRACE("Santroller hid live guitar detected!\n");
            break;
        case DJ_HERO_TURNTABLE:
            caps->subtype = DJ_HERO_TURNTABLE;
            caps->device_type = turntable;
            TRACE("Santroller hid turntable detected!\n");
            break;
        case ROCK_BAND_PRO_KEYS:
            caps->subtype = XINPUT_DEVSUBTYPE_PRO_KEYS;
            caps->device_type = pro_keys;
            TRACE("Santroller hid pro keys detected!\n");
            break;
        case ROCK_BAND_PRO_GUITAR_MUSTANG:
            caps->subtype = XINPUT_DEVSUBTYPE_PRO_GUITAR;
            caps->device_type = pro_guitar_mustang;
            TRACE("Santroller hid pro guitar mustang detected!\n");
            break;
        case ROCK_BAND_PRO_GUITAR_SQUIRE:
            caps->subtype = XINPUT_DEVSUBTYPE_PRO_GUITAR;
            caps->device_type = pro_guitar_squire;
            TRACE("Santroller hid pro guitar squire detected!\n");
            break;
        }
    }
    // if (vidpid == MAKELONG(0x045e, 0x028e))
    // {
    //     TRACE("Setting subtype to guitar!\n");
    //     TRACE("CRKD guitar detected!\n");
    //     caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    //     caps->crkd = true;
    // }
    if (vidpid == MAKELONG(0x2563, 0x0575))
    {
        TRACE("PS2 usb adapter detected!\n");
        caps->ps2needschecking = true;
        caps->ps3thirdparty = false;
        caps->device_type = gamepad;
        // adapter emulates ps3 controller
        caps->console_type = ps3;
    }
    if (vidpid == MAKELONG(0x12BA, 0x074B))
    {
        caps->device_type = live_guitar;
        caps->console_type = ps3;
    }
    if (vidpid == MAKELONG(0x12BA, 0x074B))
    {
        caps->device_type = live_guitar;
        caps->console_type = ps3;
    }
    if (vidpid == MAKELONG(0x12BA, 0x0140))
    {
        caps->device_type = turntable;
        caps->console_type = ps3;
        caps->subtype = XINPUT_DEVSUBTYPE_TURNTABLE;
    }
    if (vidpid == MAKELONG(0x057e, 0x0306))
    {
        TRACE("Wii controller detected!\n");
        // if (strcmp(instance->tszProductName, "Nintendo Wii Remote Guitar") == 0)
        // {
        //     printf("found wii guitar\r\n");
        //     caps->wiighlinux = true;
        //     caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
        // }
        // // only care about the guitar peripheral
        // if (strcmp(instance->tszProductName, "Nintendo Wii Remote") == 0)
        // {
        //     printf("found wii remote, ignoring\r\n");
        //     return FALSE;
        // }
    }

    TRACE("vidpid: %08x\n", vidpid);
    TRACE("vidpid: %08x\n", vidpid);
    TRACE("axes: %08x\n", dinput_caps.dwAxes);
    TRACE("buttons: %08x\n", dinput_caps.dwButtons);
    // only force wireless adapters to act as guitars on linux
    for (i = 0; i < sizeof(wireless_products) / sizeof(wireless_products[0]); i++)
    {
        if (vidpid == wireless_products[i])
        {
            caps->wireless = TRUE;
            caps->console_type = xbox360;
            if (override_wireless)
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                caps->device_type = gh_guitar;
            }
            break;
        }
    }

    for (i = 0; i < sizeof(ps4_products) / sizeof(ps4_products[0]); i++)
    {
        if (vidpid == ps4_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("PS4 guitar detected!\n");
            if (override_rb)
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            else
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
            }
            caps->device_type = rb_guitar;
            caps->console_type = ps4;
            break;
        }
    }

    for (i = 0; i < sizeof(ps5_products) / sizeof(ps5_products[0]); i++)
    {
        if (vidpid == ps5_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("PS5 guitar detected!\n");
            if (override_rb)
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            else
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
            }
            caps->device_type = rb_guitar;
            caps->console_type = ps5;
            break;
        }
    }

    for (i = 0; i < sizeof(raphnet_wii_products) / sizeof(raphnet_wii_products[0]); i++)
    {
        if (vidpid == raphnet_wii_products[i])
        {
            TRACE("Raphnet wusbmote detected!\n");
            caps->device_type = gamepad;
            caps->console_type = raphnet_wii;
            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
            break;
        }
    }

    for (i = 0; i < sizeof(raphnet_ps2_products) / sizeof(raphnet_ps2_products[0]); i++)
    {
        if (vidpid == raphnet_ps2_products[i])
        {
            TRACE("Raphnet ps2 adapter detected!\n");
            caps->device_type = gamepad;
            caps->console_type = raphnet_ps2;
            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
            break;
        }
    }

    for (i = 0; i < sizeof(rb_ps3_products) / sizeof(rb_ps3_products[0]); i++)
    {
        if (vidpid == rb_ps3_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("RB PS3 guitar detected!\n");
            if (override_rb)
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            else
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
            }
            caps->device_type = rb_guitar;
            caps->console_type = ps3;
            break;
        }
    }

    for (i = 0; i < sizeof(gh_ps3_products) / sizeof(gh_ps3_products[0]); i++)
    {
        if (vidpid == gh_ps3_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("GH PS3 guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->device_type = gh_guitar;
            caps->console_type = ps3;
            break;
        }
    }

    for (i = 0; i < sizeof(rb_ps3_drum_products) / sizeof(rb_ps3_drum_products[0]); i++)
    {
        if (vidpid == rb_ps3_drum_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("RB PS3 drum detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            caps->device_type = rb_drum;
            caps->console_type = ps3;
            break;
        }
    }

    for (i = 0; i < sizeof(gh_ps3_drum_products) / sizeof(gh_ps3_drum_products[0]); i++)
    {
        if (vidpid == gh_ps3_drum_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("GH PS3 drum detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            caps->device_type = gh_drum;
            caps->console_type = ps3;
            break;
        }
    }

    for (i = 0; i < sizeof(gh_xinput_products) / sizeof(gh_xinput_products[0]); i++)
    {
        if (vidpid == gh_xinput_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("XInput GH guitar detected!\n");
            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            caps->device_type = gh_guitar;
            caps->console_type = xbox360;
            break;
        }
    }

    for (i = 0; i < sizeof(rb_xinput_products) / sizeof(rb_xinput_products[0]); i++)
    {
        if (vidpid == rb_xinput_products[i])
        {
            TRACE("Setting subtype to guitar!\n");
            TRACE("XInput RB guitar detected!\n");
            if (override_rb)
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            else
            {
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
            }
            caps->device_type = rb_guitar;
            caps->console_type = xbox360;
            break;
        }
    }
}

void print_device(struct hid_device_info *cur_dev)
{
    printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);
    printf("\n");
    printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
    printf("  Product:      %ls\n", cur_dev->product_string);
    printf("  Release:      %hx\n", cur_dev->release_number);
    printf("  Interface:    %d\n", cur_dev->interface_number);
    printf("  Usage (page): 0x%hx (0x%hx)\n", cur_dev->usage, cur_dev->usage_page);
    printf("  Bus type: %u (%s)\n", (unsigned)cur_dev->bus_type, hid_bus_name(cur_dev->bus_type));
    printf("\n");
    if (dinput.mapped == sizeof(controllers) / sizeof(*controllers))
        return;
    controllers[dinput.mapped].xinput_index = -1;
    controllers[dinput.mapped].connected = TRUE;
    controllers[dinput.mapped].device = hid_open_path(cur_dev->path);
    hid_device *device = controllers[dinput.mapped].device;
    dinput_fill_caps(&controllers[dinput.mapped].caps, MAKELONG(cur_dev->vendor_id, cur_dev->product_id), cur_dev->release_number);
    
    if (controllers[dinput.mapped].caps.console_type == raphnet_wii || controllers[dinput.mapped].caps.console_type == raphnet_ps2)
    {
        // config interface follows gamepad interface
        hid_device *config = hid_open_path(cur_dev->next->path);
        uint8_t data[65] = {0x00, 0x00, 0x00, 0x00};
        uint8_t data2[65] = {0x00, 0x06, 0x00, 0x00};
        for (int i = 0; i < 10; i++)
        {
            hid_send_feature_report(config, data2, sizeof(data2));
            hid_get_feature_report(config, data, sizeof(data));
            if (data[1])
            {
                printf("raphnet type: %d\r\n", data[3]);

                switch (data[3])
                {
                case RNT_TYPE_PSX_DIGITAL:
                case RNT_TYPE_PSX_ANALOG:
                case RNT_TYPE_PSX_NEGCON:
                case RNT_TYPE_PSX_MOUSE:
                case RNT_TYPE_CLASSIC:
                case RNT_TYPE_UDRAW_TABLET:
                case RNT_TYPE_NUNCHUK:
                case RNT_TYPE_CLASSIC_PRO:
                    controllers[dinput.mapped].caps.device_type = gamepad;
                    controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
                    printf("Raphnet controller type: gamepad\r\n");
                    // TODO: check if its a ps2 guitar
                    break;
                case RNT_TYPE_WII_GUITAR:
                    controllers[dinput.mapped].caps.device_type = gh_guitar;
                    controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                    printf("Raphnet controller type: guitar\r\n");
                    break;
                case RNT_TYPE_WII_DRUM:
                    controllers[dinput.mapped].caps.device_type = gh_drum;
                    controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
                    printf("Raphnet controller type: drum\r\n");
                    break;
                }
                break;
            }
            Sleep(100);
        }
        hid_close(config);
    }
    dinput.mapped++;
}

void print_devices(struct hid_device_info *cur_dev)
{
    for (; cur_dev; cur_dev = cur_dev->next)
    {
        print_device(cur_dev);
    }
}

void print_devices_with_descriptor(struct hid_device_info *cur_dev)
{
    for (; cur_dev; cur_dev = cur_dev->next)
    {
        if (strstr(cur_dev->path, "IG_"))
        {
            printf("found xinput, skipping\n");
            continue;
        }
        if (cur_dev->usage != HID_USAGE_GENERIC_JOYSTICK && cur_dev->usage != HID_USAGE_GENERIC_GAMEPAD)
        {
            continue;
        }
        print_device(cur_dev);
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
    return false;
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

static int string_ends_with(const char *str, const char *suffix)
{
    int str_len = strlen(str);
    int suffix_len = strlen(suffix);

    return (str_len >= suffix_len) &&
           (0 == strcmp(str + (str_len - suffix_len), suffix));
}
static void dinput_start(void)
{
    HRESULT hr;
    if (initialized)
        return;
    initialized = TRUE;
    HINSTANCE hinstLib;
    HINSTANCE hinstLibD;

    CONSOLE_SCREEN_BUFFER_INFO csbi;

    // Create a new console window.
    if (ShowConsole() && AllocConsole()) {

        // Set the screen buffer to be larger than normal (this is optional).
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        {
            csbi.dwSize.Y = 1000; // any useful number of lines...
            SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), csbi.dwSize);
        }

        // Redirect "stdin" to the console window.
        freopen("CONIN$", "w", stdin);

        // Redirect "stderr" to the console window.
        freopen("CONOUT$", "w", stderr);

        // Redirect "stdout" to the console window.
        freopen("CONOUT$", "w", stdout);

        // Turn off buffering for "stdout" ("stderr" is unbuffered by default).

        setbuf(stdout, NULL);
    }
    // Enable hidraw for direct access to controllers on linux
    if (KeyExists(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Wine") || KeyExists(HKEY_CURRENT_USER, L"Software\\Wine"))
    {
        // if any of them are updated, pop up a msgbox saying that the prefix needs to be restarted
        static const uint16_t vendors[] = {
            0x12ba,
            0x1209,
            0x1bad,
            0x3651,
            0x289b,
            0x0e6f,
            0x0738,
            0x1430,
            0x1122};
        wchar_t regKey[64];
        bool updated = false;
        for (int i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++)
        {
            swprintf(regKey, sizeof(regKey), L"System\\CurrentControlSet\\Services\\WineBus\\Devices\\%04x", vendors[i]);
            updated |= UpdateKey(HKEY_LOCAL_MACHINE, regKey, L"HidRaw", 1);
        }
        if (updated)
        {
            int msgboxID = MessageBoxW(
                NULL,
                (LPCWSTR)L"Device entries changed, please restart your wine prefix",
                (LPCWSTR)L"Warning",
                MB_ICONWARNING | MB_OK);
        }
    }

    override_rb = OverrideRbWithGh();
    override_wireless = OverrideWireless();
    if (LoadLibraryW(L"wh.dll") != NULL)
    {
        TRACE("found wh\r\n");
    }
    // Get a handle to the DLL module.
    bool hasEx = true;
    hinstLib = LoadLibraryW(L"C:\\Windows\\System32\\xinput1_4.dll");
    if (hinstLib == NULL)
    {
        hasEx = false;
        hinstLib = LoadLibraryW(L"C:\\Windows\\System32\\xinput1_3.dll");
    }
    if (hinstLib == NULL)
    {
        hinstLib = LoadLibraryW(L"C:\\Windows\\System32\\xinput1_2.dll");
    }
    if (hinstLib == NULL)
    {
        hinstLib = LoadLibraryW(L"C:\\Windows\\System32\\xinput1_1.dll");
    }
    if (hinstLib == NULL)
    {
        hinstLib = LoadLibraryW(L"C:\\Windows\\System32\\xinput9_1_0.dll");
    }

    // If the handle is valid, try to get the function address.

    if (hinstLib != NULL)
    {
        TRACE("found dll\r\n");
        ProcXInputGetState = (_XInputGetState)GetProcAddress(hinstLib, "XInputGetState");
        ProcXInputGetStateEx = (_XInputGetStateEx)GetProcAddress(hinstLib, (LPCSTR)100);
        ProcXInputGetCapabilitiesEx = (_XInputGetCapabilitiesEx)GetProcAddress(hinstLib, (LPCSTR)108);
        ;
        ProcXInputGetCapabilities = (_XInputGetCapabilities)GetProcAddress(hinstLib, "XInputGetCapabilities");

        // If the function address is valid, call the function.

        if (NULL != XInputGetState && NULL != XInputGetCapabilitiesEx && hasEx)
        {
            for (int i = 0; i < 4; i++)
            {
                XINPUT_CAPABILITIES_EX caps2;
                if ((ProcXInputGetCapabilitiesEx)(1, i, 0, &caps2) != ERROR_DEVICE_NOT_CONNECTED)
                {
                    TRACE("xinput vid: %04x, pid: %04x!\n", caps2.VendorId, caps2.ProductId);
                    TRACE("found xinput device, proxying\r\n");
                    controllers[dinput.mapped].xinput_index = i;
                    controllers[dinput.mapped].connected = TRUE;
                    controllers[dinput.mapped].caps.subtype = caps2.Capabilities.SubType;
                    dinput_fill_caps(&controllers[dinput.mapped].caps, MAKELONG(caps2.VendorId, caps2.ProductId), caps2.VersionNumber);
                    if (caps2.Capabilities.SubType == XINPUT_DEVSUBTYPE_GUITAR && override_rb)
                    {
                        TRACE("found RB guitar, proxying as GH guitar\r\n");
                        controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                    }
                    if (caps2.VendorId == 0x1BAD && caps2.ProductId == 0x0719)
                    {
                        TRACE("Found clipper / rb4instrumentmapper, proxying as GH guitar!\n");
                        controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                    }
                    if (caps2.VendorId == 0x1430 && caps2.ProductId == 0x4734)
                    {
                        TRACE("Found wiitarthing, proxying as GH guitar!\n");
                        controllers[dinput.mapped].caps.subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                    }
                    dinput.mapped++;
                }
            }
        }
        else if (NULL != XInputGetState && NULL != XInputGetCapabilities)
        {
            TRACE("found procs\r\n");
            for (int i = 0; i < 4; i++)
            {
                XINPUT_CAPABILITIES caps;
                if ((ProcXInputGetCapabilities)(i, 0, &caps) != ERROR_DEVICE_NOT_CONNECTED && caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD)
                {
                    TRACE("found xinput gamepad, proxying\r\n");
                    controllers[dinput.mapped].xinput_index = i;
                    controllers[dinput.mapped].connected = TRUE;
                    dinput.mapped++;
                }
            }
        }
    }
    else
    {

        TRACE("unable to find xinput1_3\r\n");
    }
    dinput.enabled = TRUE;
    struct hid_device_info *devs;
    hid_init();
    devs = hid_enumerate(0x0, 0x0);
    print_devices_with_descriptor(devs);
    hid_free_enumeration(devs);
}
static const uint8_t pickup_vals[] = {0x17, 0x4B, 0x79, 0xAB, 0xE0};
uint8_t ps3_ghl_wakeup[] = {0x00, 0x02, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t ps4_ghl_wakeup[] = {0x30, 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};
static void dinput_update(int index)
{
    HRESULT hr;
    DIJOYSTATE2 data;
    XINPUT_GAMEPAD gamepad_state;

    if (dinput.enabled)
    {
        uint8_t last_pickup = gamepad_state.bLeftTrigger;
        int16_t last_whammy = gamepad_state.sThumbRX;
        struct CapsFlags caps = controllers[index].caps;
        memset(&gamepad_state, 0, sizeof(XINPUT_GAMEPAD));
        if (controllers[index].caps.device_type == live_guitar)
        {
            time_t last = controllers[index].last_poke;
            time_t ltime;
            time(&ltime);
            if (ltime - last > 8)
            {
                controllers[index].last_poke = ltime;
                if (caps.console_type == ps3)
                {
                    hid_send_output_report(controllers[index].device, ps3_ghl_wakeup, sizeof(ps3_ghl_wakeup));
                }
                if (caps.console_type == ps4)
                {
                    hid_send_output_report(controllers[index].device, ps4_ghl_wakeup, sizeof(ps4_ghl_wakeup));
                }
            }
        }
        uint8_t data[64];
        int size = hid_read_timeout(controllers[index].device, data, sizeof(data), 1);
        if (!size)
        {
            return;
        }
        if (controllers[index].caps.console_type == ps3)
        {

            if (!controllers[index].caps.ps3thirdparty)
            {
                PS3Gamepad_Data_t *report = (PS3Gamepad_Data_t *)data;
                if (controllers[index].caps.ps2needschecking)
                {
                    controllers[index].caps.ps2needschecking = false;

                    if (report->dpadLeft)
                    {
                        controllers[index].caps.subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                        controllers[index].caps.device_type = gh_guitar;
                        TRACE("Setting subtype to guitar!\n");
                        TRACE("PS2 guitar detected!\n");
                    }
                }
                if (controllers[index].caps.device_type == gamepad)
                {
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (report->rightShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    if (report->dpadUp)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (report->dpadDown)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (report->dpadLeft)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (report->dpadRight)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                    gamepad_state.sThumbLX = (report->leftStickX << 8) - 32767;
                    gamepad_state.sThumbLY = (report->leftStickY << 8) - 32767;
                    gamepad_state.sThumbRX = (report->rightStickX << 8) - 32767;
                    gamepad_state.sThumbRY = (report->rightStickY << 8) - 32767;
                }
            }
            else
            {
                PS3Dpad_Data_t *report = (PS3Dpad_Data_t *)data;
                uint8_t dpad = report->dpad >= 0x08 ? 0 : dpad_bindings_reverse[report->dpad];
                bool up = dpad & UP;
                bool left = dpad & LEFT;
                bool down = dpad & DOWN;
                bool right = dpad & RIGHT;
                switch (controllers[index].caps.device_type)
                {
                case live_guitar:
                {
                    PS3GHLGuitar_Data_t *report = (PS3GHLGuitar_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (report->rightShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    if (report->leftThumbClick)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    int32_t whammy = (report->whammy << 8) - 32767;
                    int32_t strum = (report->strumBar << 8) - 32767;
                    int32_t tilt = ((report->tilt & 0xFF) << 8) - 32767;
                    gamepad_state.sThumbLX = 0;
                    gamepad_state.sThumbLY = strum;
                    gamepad_state.sThumbRX = whammy;
                    gamepad_state.sThumbRY = tilt;
                    break;
                }
                case gh_guitar:
                {
                    PS3GuitarHeroGuitar_Data_t *report = (PS3GuitarHeroGuitar_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    int32_t whammy = (report->whammy << 9) - 32767;
                    gamepad_state.sThumbRX = whammy;
                    gamepad_state.sThumbLX = (report->slider ^ 0x80) << 8;
                    gamepad_state.sThumbRY = -((report->tilt - 511) << 8);
                    break;
                }
                case rb_guitar:
                {
                    PS3RockBandGuitar_Data_t *report = (PS3RockBandGuitar_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->solo)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                    if (report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    // ps3 rb whammy and pickup resets ot 0x7f when not touched, so ignore that state
                    int32_t whammy = (report->whammy << 8) - 32767;
                    gamepad_state.sThumbRX = whammy == 0x7f ? last_whammy : whammy;
                    gamepad_state.bLeftTrigger = report->pickup == 0x7f ? last_pickup : report->pickup;
                    gamepad_state.sThumbRY = report->tilt ? 32767 : 0;
                    break;
                }
                case rb_drum:
                {
                    PS3RockBandDrums_Data_t *report = (PS3RockBandDrums_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->kick1)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (report->kick2)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                    if (report->padFlag)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
                    if (report->cymbalFlag)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    break;
                }
                case gh_drum:
                {
                    PS3GuitarHeroDrums_Data_t *report = (PS3GuitarHeroDrums_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (report->rightShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                    break;
                }

                case turntable:
                {
                    PS3DJHTurntable_Data_t *report = (PS3DJHTurntable_Data_t *)data;
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (report->x)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    if (report->leftGreen)
                        gamepad_state.bLeftTrigger |= 0b001;
                    if (report->leftRed)
                        gamepad_state.bLeftTrigger |= 0b010;
                    if (report->leftBlue)
                        gamepad_state.bLeftTrigger |= 0b100;
                    if (report->rightGreen)
                        gamepad_state.bRightTrigger |= 0b001;
                    if (report->rightRed)
                        gamepad_state.bRightTrigger |= 0b010;
                    if (report->rightBlue)
                        gamepad_state.bRightTrigger |= 0b100;

                    // scratching on xinput uses a tiny range
                    gamepad_state.sThumbLX = (report->leftTableVelocity >> 1) - 64;
                    gamepad_state.sThumbLY = (report->rightTableVelocity >> 1) - 64;
                    gamepad_state.sThumbRX = (report->effectsKnob << 6);
                    gamepad_state.sThumbRY = -(report->crossfader << 6);
                    break;
                }
                }
            }
        }
        if (controllers[index].caps.console_type == santroller)
        {

            PCGamepad_Data_t *report = (PCGamepad_Data_t *)data;
            uint8_t dpad = report->dpad >= 0x08 ? 0 : dpad_bindings_reverse[report->dpad];
            bool up = dpad & UP;
            bool left = dpad & LEFT;
            bool down = dpad & DOWN;
            bool right = dpad & RIGHT;
            switch (controllers[index].caps.device_type)
            {
            case live_guitar:
            {
                PCGHLGuitar_Data_t *report = (PCGHLGuitar_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->rightShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (report->leftThumbClick)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                int32_t whammy = (report->whammy << 8) - 32767;
                int32_t tilt = ((report->tilt & 0xFF) << 8) - 32767;
                gamepad_state.sThumbLX = 0;
                gamepad_state.sThumbLY = up ? 32767 : down ? -32767
                                                           : 0;
                gamepad_state.sThumbRX = whammy;
                gamepad_state.sThumbRY = tilt;
                break;
            }
            case gh_guitar:
            {
                PCGuitarHeroGuitar_Data_t *report = (PCGuitarHeroGuitar_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                int32_t whammy = (report->whammy << 9) - 32767;
                gamepad_state.sThumbRX = whammy;
                gamepad_state.sThumbLX = (report->slider ^ 0x80) << 8;
                gamepad_state.sThumbRY = -((report->tilt - 511) << 8);
                break;
            }
            case rb_guitar:
            {
                PCRockBandGuitar_Data_t *report = (PCRockBandGuitar_Data_t *)data;
                if (report->a || report->soloGreen)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b || report->soloRed)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x || report->soloYellow)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y || report->soloBlue)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder || report->soloOrange)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->soloGreen || report->soloRed || report->soloOrange || report->soloYellow || report->soloBlue)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                // ps3 rb whammy and pickup resets ot 0x7f when not touched, so ignore that state
                int32_t whammy = (report->whammy << 8) - 32767;
                gamepad_state.sThumbRX = whammy == 0x7f ? last_whammy : whammy;
                gamepad_state.bLeftTrigger = report->pickup == 0x7f ? last_pickup : report->pickup;
                gamepad_state.sThumbRY = report->tilt ? 32767 : 0;
                break;
            }
            case rb_drum:
            {
                PCRockBandDrums_Data_t *report = (PCRockBandDrums_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->kick1)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->kick2)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                if (report->padFlag)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
                if (report->cymbalFlag)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                break;
            }
            case gh_drum:
            {
                PCGuitarHeroDrums_Data_t *report = (PCGuitarHeroDrums_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->rightShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                break;
            }

            case turntable:
            {
                PCTurntable_Data_t *report = (PCTurntable_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                if (report->leftGreen)
                    gamepad_state.bLeftTrigger |= 0b001;
                if (report->leftRed)
                    gamepad_state.bLeftTrigger |= 0b010;
                if (report->leftBlue)
                    gamepad_state.bLeftTrigger |= 0b100;
                if (report->rightGreen)
                    gamepad_state.bRightTrigger |= 0b001;
                if (report->rightRed)
                    gamepad_state.bRightTrigger |= 0b010;
                if (report->rightBlue)
                    gamepad_state.bRightTrigger |= 0b100;

                // scratching on xinput uses a tiny range
                gamepad_state.sThumbLX = (report->leftTableVelocity >> 1) - 64;
                gamepad_state.sThumbLY = (report->rightTableVelocity >> 1) - 64;
                gamepad_state.sThumbRX = (report->effectsKnob << 6);
                gamepad_state.sThumbRY = -(report->crossfader << 6);
                break;
            }
            case gamepad:
            {
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->rightShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (report->dpadUp)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (report->dpadDown)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (report->dpadLeft)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (report->dpadRight)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                gamepad_state.sThumbLX = (report->leftStickX << 8) - 32767;
                gamepad_state.sThumbLY = (report->leftStickY << 8) - 32767;
                gamepad_state.sThumbRX = (report->rightStickX << 8) - 32767;
                gamepad_state.sThumbRY = (report->rightStickY << 8) - 32767;
                break;
            }
            }
        }
        if (controllers[index].caps.console_type == raphnet_wii)
        {
            switch (controllers[index].caps.device_type)
            {
            case gh_guitar:
            {
                RaphnetGuitar_Data_t *report = (RaphnetGuitar_Data_t *)data;
                if (report->green)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->red)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->yellow)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->blue)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->orange)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (report->down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (report->plus)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->minus)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                gamepad_state.sThumbLX = report->slider;
                gamepad_state.sThumbRX = report->whammy;
                break;
            }
            case gh_drum:
            {
                RaphnetDrum_Data_t *report = (RaphnetDrum_Data_t *)data;
                if (report->green)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->red)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->yellow)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->blue)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->orange)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->plus)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->minus)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                break;
            }
            case gamepad:
            {
                RaphnetGamepad_Data_t *report = (RaphnetGamepad_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->rightShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (report->up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (report->down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (report->left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (report->right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->select)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                gamepad_state.sThumbLX = report->leftJoyX;
                gamepad_state.sThumbLY = report->leftJoyY;
                gamepad_state.sThumbRX = report->rightJoyX;
                gamepad_state.sThumbRY = report->rightJoyY;
                break;
            }
            }
        }
        if (caps.console_type == ps4)
        {

            PS4Dpad_Data_t *report = (PS4Dpad_Data_t *)data;
            uint8_t dpad = report->dpad >= 0x08 ? 0 : dpad_bindings_reverse[report->dpad];
            bool up = dpad & UP;
            bool left = dpad & LEFT;
            bool down = dpad & DOWN;
            bool right = dpad & RIGHT;
            switch (caps.device_type)
            {
            case rb_guitar:
            {
                PS4RockBandGuitar_Data_t *report = (PS4RockBandGuitar_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->solo)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                int32_t whammy = (report->whammy << 8) - 32767;
                gamepad_state.sThumbRX = whammy;
                gamepad_state.bLeftTrigger = pickup_vals[report->pickup];
                gamepad_state.sThumbRY = (report->tilt << 7);
                break;
            }
            case rb_drum:
            {
                PS4RockBandDrums_Data_t *report = (PS4RockBandDrums_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->kick1)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->kick2)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                if (report->yellowCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP;
                if (report->blueCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_DOWN;
                if (report->greenCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

                break;
            }
            case live_guitar:
            {
                PS4GHLGuitar_Data_t *report = (PS4GHLGuitar_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->rightShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (report->leftThumbClick)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                int32_t whammy = (report->whammy << 8) - 32767;
                int32_t strum = (report->strumBar << 8) - 32767;
                int32_t tilt = ((report->tilt & 0xFF) << 8) - 32767;
                gamepad_state.sThumbLX = 0;
                gamepad_state.sThumbLY = strum;
                gamepad_state.sThumbRX = whammy;
                gamepad_state.sThumbRY = tilt;
                break;
            }
            }
        }
        if (caps.console_type == ps5)
        {

            PS5Dpad_Data_t *report = (PS5Dpad_Data_t *)data;
            uint8_t dpad = report->dpad >= 0x08 ? 0 : dpad_bindings_reverse[report->dpad];
            bool up = dpad & UP;
            bool left = dpad & LEFT;
            bool down = dpad & DOWN;
            bool right = dpad & RIGHT;
            switch (caps.device_type)
            {
            case rb_guitar:
            {
                PS5RockBandGuitar_Data_t *report = (PS5RockBandGuitar_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->solo)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (report->leftShoulder)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                int32_t whammy = (report->whammy << 8) - 32767;
                gamepad_state.sThumbRX = whammy;
                gamepad_state.bLeftTrigger = pickup_vals[report->pickup];
                gamepad_state.sThumbRY = (report->tilt << 7);
                break;
            }
            case rb_drum:
            {
                PS5RockBandDrums_Data_t *report = (PS5RockBandDrums_Data_t *)data;
                if (report->a)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                if (report->b)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                if (report->x)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                if (report->y)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                if (report->kick1)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                if (report->kick2)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
                if (report->start)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                if (report->back)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                if (report->yellowCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP;
                if (report->blueCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_DOWN;
                if (report->greenCymbalVelocity)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

                break;
            }
            }
        }
    }
    else
        memset(&gamepad_state, 0, sizeof(XINPUT_GAMEPAD));

    if (memcmp(&controllers[index].state.Gamepad, &gamepad_state, sizeof(XINPUT_GAMEPAD)))
    {
        controllers[index].state.Gamepad = gamepad_state;
        controllers[index].state.dwPacketNumber++;
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

DWORD dumb_XInputGetStateEx(DWORD index, XINPUT_STATE *state, DWORD caller_version)
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
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (controllers[index].xinput_index != -1)
    {
        int ret = (ProcXInputGetStateEx)(controllers[index].xinput_index, state);
        if (ret == ERROR_DEVICE_NOT_CONNECTED)
        {
            controllers[index].connected = false;
        }
        return ret;
    }

    dinput_update(index);
    if (controllers[index].caps.ps2needschecking)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    *state = controllers[index].state;

    return ERROR_SUCCESS;
}

/* ============================ Dll Functions =============================== */

DWORD dumb_XInputGetState(DWORD index, XINPUT_STATE *state, DWORD caller_version)
{
    if (!initialized)
    {
        DPRINT("Force-enabling dumb XInput\n");
        dumb_XInputEnable(TRUE);
    }

    if (index >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].connected)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (controllers[index].xinput_index != -1)
    {
        int ret = (ProcXInputGetState)(controllers[index].xinput_index, state);
        if (ret == ERROR_DEVICE_NOT_CONNECTED)
        {
            controllers[index].connected = false;
        }
        return ret;
    }

    DWORD ret;

    // TRACE("dumb_XInputGetState: %d\n", index);

    ret = dumb_XInputGetStateEx(index, state, caller_version);
    if (ret != ERROR_SUCCESS)
        return ret;

    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

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
        // dinput_send_effect(index, power);
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

    if (controllers[index].xinput_index != -1)
    {
        int ret = (ProcXInputGetCapabilities)(controllers[index].xinput_index, flags, capabilities);
        capabilities->SubType = controllers[index].caps.subtype;
        if (controllers[index].caps.device_type == rb_drum)
            capabilities->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
        if (controllers[index].caps.device_type == live_guitar)
            capabilities->Flags |= XINPUT_CAPS_NO_NAVIGATION;
        if (ret == ERROR_DEVICE_NOT_CONNECTED)
        {
            controllers[index].connected = false;
        }
        return ret;
    }

    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;

    capabilities->Flags = 0;
    if (controllers[index].caps.jedi || controllers[index].caps.device_type == rb_drum)
        capabilities->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
    if (!controllers[index].caps.pov || controllers[index].caps.device_type == live_guitar)
        capabilities->Flags |= XINPUT_CAPS_NO_NAVIGATION;

    dinput_update(index);

    capabilities->SubType = controllers[index].caps.subtype;
    capabilities->Vibration = controllers[index].vibration;
    capabilities->Gamepad = controllers[index].state.Gamepad;
    TRACE("dumb_XInputGetCapabilities (%d)\n", capabilities->SubType);

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
