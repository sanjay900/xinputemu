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
#include <pthread.h>
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
    xbox360,
    santroller_2,
    linux_sdl,
    xbox_one,
    generic_hid
} console_type_t;
#define DIRECTINPUT_VERSION 0x0800
#define DEBUG
#include "dinput.h"
#include "gip_reports.h"
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
// Largest input report read from a HID device, generic gamepads are parsed using their descriptor so can be any size
#define GENERIC_HID_MAX_REPORT 512
#define GENERIC_HID_MAX_BUTTONS 32
// Not real XInput buttons, a digital trigger press for pads without analog triggers
#define GENERIC_BUTTON_LT 0x10000
#define GENERIC_BUTTON_RT 0x20000
#ifndef HID_USAGE_SIMULATION_ACCELERATOR
#define HID_USAGE_SIMULATION_ACCELERATOR ((USAGE)0xC4)
#endif
#ifndef HID_USAGE_SIMULATION_BRAKE
#define HID_USAGE_SIMULATION_BRAKE ((USAGE)0xC5)
#endif

typedef enum
{
    generic_lx,
    generic_ly,
    generic_rx,
    generic_ry,
    generic_lt,
    generic_rt,
    // one axis for both triggers, like DirectInput gives Xbox 360 controllers
    generic_combined_triggers,
    generic_hat,
    generic_value_count
} generic_value_role_t;

typedef struct
{
    BOOL present;
    USAGE page, usage;
    LONG min, max;
    USHORT bits;
} generic_value_t;

struct GenericHid
{
    PHIDP_PREPARSED_DATA preparsed;
    USHORT report_length;
    // the input report holding the gamepad's controls, 0 when the device doesn't use report IDs
    UCHAR report_id;
    generic_value_t values[generic_value_count];
    uint32_t button_map[GENERIC_HID_MAX_BUTTONS];
};

struct CapsFlags
{
    BOOL wireless, jedi, pov, windows, macos, sdl, ps2needschecking, ps3thirdparty, dpad_as_buttons, y_axis_inverted;
    int axes, buttons, subtype;
    uint16_t vid, pid;
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
    uint64_t gip_device_id;
    uint8_t gip_last_sequence;
    struct GenericHid *generic;
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

// For devices whose instrument type can't be queried without raw HID access, read it from input_config.ini
static void GetSubtypeOverride(const char *section, const char *legacy_key, int *out_subtype, device_type_t *out_device_type)
{
    CHAR Buffer[MAX_PATH];
    CHAR SubtypeStr[64];
    GetCurrentDirectoryA(MAX_PATH, Buffer);
    strcat(Buffer, "\\input_config.ini");

    // Default to Guitar Hero guitar
    *out_subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
    *out_device_type = gh_guitar;

    GetPrivateProfileStringA(section, "subtype", "", SubtypeStr, sizeof(SubtypeStr), Buffer);
    if (SubtypeStr[0] == '\0' && legacy_key)
    {
        GetPrivateProfileStringA("overrides", legacy_key, "", SubtypeStr, sizeof(SubtypeStr), Buffer);
    }

    if (SubtypeStr[0] != '\0')
    {
        if (strcasecmp(SubtypeStr, "guitar") == 0 || strcasecmp(SubtypeStr, "gh_guitar") == 0 || strcasecmp(SubtypeStr, "guitar_alternate") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            *out_device_type = gh_guitar;
        }
        else if (strcasecmp(SubtypeStr, "rb_guitar") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_GUITAR;
            *out_device_type = rb_guitar;
        }
        else if (strcasecmp(SubtypeStr, "drums") == 0 || strcasecmp(SubtypeStr, "drum") == 0 || strcasecmp(SubtypeStr, "gh_drum") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            *out_device_type = gh_drum;
        }
        else if (strcasecmp(SubtypeStr, "rb_drum") == 0 || strcasecmp(SubtypeStr, "rb_drums") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            *out_device_type = rb_drum;
        }
        else if (strcasecmp(SubtypeStr, "gamepad") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
            *out_device_type = gamepad;
        }
        else if (strcasecmp(SubtypeStr, "live_guitar") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            *out_device_type = live_guitar;
        }
        else if (strcasecmp(SubtypeStr, "turntable") == 0)
        {
            *out_subtype = XINPUT_DEVSUBTYPE_TURNTABLE;
            *out_device_type = turntable;
        }
    }
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

static bool IsWine(void)
{
    return KeyExists(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Wine") || KeyExists(HKEY_CURRENT_USER, L"Software\\Wine");
}

static bool SDLEnabled(void)
{
    DWORD data;
    DWORD size = sizeof(data);
    LONG result = RegGetValueW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\WineBus", L"Enable SDL", RRF_RT_DWORD, NULL, (LPBYTE)&data, &size);

    if (result == ERROR_SUCCESS)
    {
        return data != 0;
    }
    else
    {
        // SDL enabled by default
        return true;
    }
}

static bool IsVendorHidRaw(uint16_t vid)
{
    if (!IsWine())
        return true;
    if (!SDLEnabled())
        return true;

    wchar_t regKey[64];
    swprintf(regKey, sizeof(regKey) / sizeof(regKey[0]), L"System\\CurrentControlSet\\Services\\WineBus\\Devices\\%04x", vid);
    DWORD data = 0;
    DWORD size = sizeof(data);
    LONG result = RegGetValueW(HKEY_LOCAL_MACHINE, regKey, L"HidRaw", RRF_RT_DWORD, NULL, (LPBYTE)&data, &size);
    if (result == ERROR_SUCCESS && data == 1)
        return true;

    return false;
}

static void parse_wine_sdl_descriptor(hid_device *device, int *out_axes, int *out_buttons, BOOL *out_pov)
{
    *out_axes = 4;
    *out_pov = TRUE;
    *out_buttons = 13;
    uint8_t desc[512];
    int len = hid_get_report_descriptor(device, desc, sizeof(desc));
    if (len <= 0) return;

    int axes = 0;
    int hats = 0;
    int buttons = 0;

    for (int i = 0; i < len; i++)
    {
        if (i + 1 < len && desc[i] == 0x09 && desc[i + 1] == 0x39)
        {
            hats++;
        }
        if (i + 1 < len && desc[i] == 0x09 && desc[i + 1] >= 0x30 && desc[i + 1] <= 0x38)
        {
            axes++;
        }
        if (i + 1 < len && desc[i] == 0x05 && desc[i + 1] == 0x09)
        {
            for (int j = i + 2; j < len && j < i + 32; j++)
            {
                if (desc[j] == 0x29 && j + 1 < len)
                {
                    buttons = desc[j + 1];
                    break;
                }
                else if (desc[j] == 0x2a && j + 2 < len)
                {
                    buttons = desc[j + 1] | (desc[j + 2] << 8);
                    break;
                }
            }
        }
    }

    if (axes > 0) *out_axes = axes;
    *out_pov = (hats > 0);
    if (buttons > 0) *out_buttons = buttons;
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

#define SONY_VID 0x054c
#define SONY_DS3_PID 0x0268
#define SONY_DS4_PID_1 0x05c4
#define SONY_DS4_PID_2 0x09cc
#define SONY_DS4_PID_3 0x0ba0
#define SONY_DS5_PID 0x0ce6
#define SONY_DS5_EDGE_PID 0x0df2
#define REDOCTANE_XBOX_VID 0x1430
#define PS4_GHLIVE_DONGLE_PID 0x07bb

#define PS_USAGE_PS3 0x01
#define PS_USAGE_PS4 0x02
#define PS_USAGE_PS5 0x04

// PlayStation sticks rest at 0x80, with Y going down; XInput wants Y going up
static inline SHORT ps_axis(uint8_t value)
{
    return (SHORT)(value * 257 - 32768);
}

static inline SHORT ps_axis_inverted(uint8_t value)
{
    return ps_axis(UINT8_MAX - value);
}

// The PS3 (third party), PS4 and PS5 gamepad reports share field names, only their layout differs.
// The d-pad is a hat in all of them, so it is passed in already decoded.
#define PS_FILL_GAMEPAD(gamepad, report, up, down, left, right)              \
    do                                                                       \
    {                                                                        \
        if ((report)->a)                                                     \
            (gamepad).wButtons |= XINPUT_GAMEPAD_A;                          \
        if ((report)->b)                                                     \
            (gamepad).wButtons |= XINPUT_GAMEPAD_B;                          \
        if ((report)->x)                                                     \
            (gamepad).wButtons |= XINPUT_GAMEPAD_X;                          \
        if ((report)->y)                                                     \
            (gamepad).wButtons |= XINPUT_GAMEPAD_Y;                          \
        if ((report)->leftShoulder)                                          \
            (gamepad).wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;              \
        if ((report)->rightShoulder)                                         \
            (gamepad).wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;             \
        if ((report)->leftThumbClick)                                        \
            (gamepad).wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;                 \
        if ((report)->rightThumbClick)                                       \
            (gamepad).wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;                \
        if ((report)->start)                                                 \
            (gamepad).wButtons |= XINPUT_GAMEPAD_START;                      \
        if ((report)->back)                                                  \
            (gamepad).wButtons |= XINPUT_GAMEPAD_BACK;                       \
        if ((report)->guide)                                                 \
            (gamepad).wButtons |= XINPUT_GAMEPAD_GUIDE;                      \
        if (up)                                                              \
            (gamepad).wButtons |= XINPUT_GAMEPAD_DPAD_UP;                    \
        if (down)                                                            \
            (gamepad).wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;                  \
        if (left)                                                            \
            (gamepad).wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;                  \
        if (right)                                                           \
            (gamepad).wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;                 \
        (gamepad).bLeftTrigger = (report)->leftTrigger;                      \
        (gamepad).bRightTrigger = (report)->rightTrigger;                    \
        (gamepad).sThumbLX = ps_axis((report)->leftStickX);                  \
        (gamepad).sThumbLY = ps_axis_inverted((report)->leftStickY);         \
        (gamepad).sThumbRX = ps_axis((report)->rightStickX);                 \
        (gamepad).sThumbRY = ps_axis_inverted((report)->rightStickY);        \
    } while (0)

// Third party PlayStation controllers declare vendor defined feature reports that consoles look for,
// the same thing Santroller and SDL use to recognise them without knowing their VID/PID:
// 0x2621 for PS3, 0x2721 / 0xA783 for PS4 and 0x2821 / 0xA883 for PS5
static int find_ps_usages(hid_device *device)
{
    uint8_t desc[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
    int len = hid_get_report_descriptor(device, desc, sizeof(desc));
    uint32_t usage_page = 0;
    int pending = 0;
    int found = 0;
    int i = 0;
    while (i < len)
    {
        uint8_t prefix = desc[i];
        if (prefix == 0xFE)
        {
            // long item, nothing in it that matters here
            if (i + 1 >= len)
                break;
            i += 3 + desc[i + 1];
            continue;
        }
        int size = prefix & 0x03;
        if (size == 3)
            size = 4;
        if (i + 1 + size > len)
            break;
        uint32_t value = 0;
        for (int j = 0; j < size; j++)
            value |= (uint32_t)desc[i + 1 + j] << (8 * j);
        switch (prefix & 0xFC)
        {
        case 0x04: // Usage Page
            usage_page = value;
            break;
        case 0x08: // Usage, a 4 byte one carries its own page
        {
            uint32_t page = size == 4 ? value >> 16 : usage_page;
            uint16_t usage = value & 0xFFFF;
            if (page >= 0xFF00)
            {
                if (usage == 0x2621)
                    pending |= PS_USAGE_PS3;
                else if (usage == 0x2721 || usage == 0xA783)
                    pending |= PS_USAGE_PS4;
                else if (usage == 0x2821 || usage == 0xA883)
                    pending |= PS_USAGE_PS5;
            }
            break;
        }
        case 0xB0: // Feature
            found |= pending;
            pending = 0;
            break;
        case 0x80: // Input
        case 0x90: // Output
        case 0xA0: // Collection
        case 0xC0: // End Collection
            pending = 0;
            break;
        }
        i += 1 + size;
    }
    return found;
}

// Third party PS4 / PS5 controllers describe what they are in feature report 3, see ps4_parse_capabilities in Santroller
static bool ps_read_device_type(hid_device *device, console_type_t console, uint8_t *out_device_type)
{
    uint8_t data[64] = {0x03};
    int len = hid_get_feature_report(device, data, sizeof(data));
    const uint8_t *caps = NULL;
#define PS_CAPS_MAGIC(b) (console == ps5 ? (b) == 0x28 : ((b) == 0x27 || (b) == 0xA7))
    if (len >= 6 && PS_CAPS_MAGIC(data[2]))
        caps = &data[2];
    else if (len >= 4 && PS_CAPS_MAGIC(data[0]))
        caps = &data[0];
    else if (len >= 5 && PS_CAPS_MAGIC(data[1]))
        caps = &data[1];
#undef PS_CAPS_MAGIC
    if (!caps)
        return false;
    *out_device_type = caps[3];
    return true;
}

static bool detect_playstation_controller(struct CapsFlags *caps, long vidpid, hid_device *device)
{
    uint16_t vid = LOWORD(vidpid);
    uint16_t pid = HIWORD(vidpid);
    console_type_t console = xinput;
    bool third_party = false;
    if (vid == SONY_VID && pid == SONY_DS3_PID)
        console = ps3;
    else if (vid == SONY_VID && (pid == SONY_DS4_PID_1 || pid == SONY_DS4_PID_2 || pid == SONY_DS4_PID_3))
        console = ps4;
    else if (vid == SONY_VID && (pid == SONY_DS5_PID || pid == SONY_DS5_EDGE_PID))
        console = ps5;
    else
    {
        int usages = find_ps_usages(device);
        third_party = true;
        if (usages & PS_USAGE_PS5)
            console = ps5;
        else if (usages & PS_USAGE_PS4)
            console = ps4;
        else if (usages & PS_USAGE_PS3)
            console = ps3;
    }
    if (console == xinput)
        return false;

    caps->console_type = console;
    caps->device_type = gamepad;
    caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;

    if (console == ps3)
    {
        caps->ps3thirdparty = third_party;
        if (!third_party)
        {
            // a DualShock 3 sends nothing until it is told to start reporting
            uint8_t enable[] = {0xF4, 0x42, 0x0c, 0x00, 0x00};
            if (hid_send_feature_report(device, enable, sizeof(enable)) < 0)
                TRACE("Failed to enable DualShock 3 reports: %ls\n", hid_error(device));
        }
        TRACE("%s PS3 gamepad detected!\n", third_party ? "Third party" : "DualShock 3");
        return true;
    }

    uint8_t ps_device_type = 0x00;
    if (third_party && ps_read_device_type(device, console, &ps_device_type))
    {
        switch (ps_device_type)
        {
        case 0x01:
            if (console == ps4 && vid == REDOCTANE_XBOX_VID && pid == PS4_GHLIVE_DONGLE_PID)
            {
                caps->device_type = live_guitar;
                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
            }
            else
            {
                caps->device_type = rb_guitar;
                caps->subtype = override_rb ? XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE : XINPUT_DEVSUBTYPE_GUITAR;
            }
            break;
        case 0x02:
            caps->device_type = rb_drum;
            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
            break;
        default:
            // wheels, fight sticks and the like still send the standard gamepad report
            break;
        }
    }
    TRACE("%s %s controller detected (device type %02x)!\n", third_party ? "Third party" : "First party",
          console == ps4 ? "PS4" : "PS5", ps_device_type);
    return true;
}

static const struct
{
    const char *name;
    uint32_t button;
} generic_button_names[] = {
    {"a", XINPUT_GAMEPAD_A},
    {"b", XINPUT_GAMEPAD_B},
    {"x", XINPUT_GAMEPAD_X},
    {"y", XINPUT_GAMEPAD_Y},
    {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"lt", GENERIC_BUTTON_LT},
    {"rt", GENERIC_BUTTON_RT},
    {"back", XINPUT_GAMEPAD_BACK},
    {"start", XINPUT_GAMEPAD_START},
    {"ls", XINPUT_GAMEPAD_LEFT_THUMB},
    {"rs", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"guide", XINPUT_GAMEPAD_GUIDE},
    {"up", XINPUT_GAMEPAD_DPAD_UP},
    {"down", XINPUT_GAMEPAD_DPAD_DOWN},
    {"left", XINPUT_GAMEPAD_DPAD_LEFT},
    {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"none", 0},
};

// There is no standard button order for HID gamepads, so this can be changed in input_config.ini, either for
// every generic gamepad under [generic] or for one under [generic_VVVV_PPPP], e.g.
// buttons=x,a,b,y,lb,rb,lt,rt,back,start,ls,rs,guide
// The default is the order DualShock 4 / DualSense style pads (and Logitech pads in DirectInput mode) use:
// square, cross, circle, triangle, L1, R1, L2, R2, share, options, L3, R3, PS
// where the first entry is HID button 1
static void generic_load_button_map(struct GenericHid *generic, uint16_t vid, uint16_t pid)
{
    CHAR path[MAX_PATH];
    CHAR section[32];
    CHAR map[512];
    GetCurrentDirectoryA(MAX_PATH, path);
    strcat(path, "\\input_config.ini");
    snprintf(section, sizeof(section), "generic_%04x_%04x", vid, pid);
    GetPrivateProfileStringA(section, "buttons", "", map, sizeof(map), path);
    if (map[0] == '\0')
        GetPrivateProfileStringA("generic", "buttons", "x,a,b,y,lb,rb,lt,rt,back,start,ls,rs,guide", map, sizeof(map), path);
    TRACE("Generic gamepad button map: %s\n", map);

    memset(generic->button_map, 0, sizeof(generic->button_map));
    int button = 0;
    char *context = NULL;
    for (char *token = strtok_r(map, ",", &context); token && button < GENERIC_HID_MAX_BUTTONS; token = strtok_r(NULL, ",", &context), button++)
    {
        while (*token == ' ' || *token == '\t')
            token++;
        char *end = token + strlen(token);
        while (end > token && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        bool known = false;
        for (size_t i = 0; i < sizeof(generic_button_names) / sizeof(generic_button_names[0]); i++)
        {
            if (strcasecmp(token, generic_button_names[i].name) == 0)
            {
                generic->button_map[button] = generic_button_names[i].button;
                known = true;
                break;
            }
        }
        if (!known)
            TRACE("Unknown button '%s' in generic button map, ignoring it\n", token);
    }
}

static void generic_set_value(struct GenericHid *generic, generic_value_role_t role, const HIDP_VALUE_CAPS *caps, USAGE usage)
{
    generic_value_t *value = &generic->values[role];
    value->present = TRUE;
    value->page = caps->UsagePage;
    value->usage = usage;
    value->bits = caps->BitSize;
    value->min = caps->LogicalMin;
    value->max = caps->LogicalMax;
    // plenty of descriptors give an unsigned maximum that only fits when read as signed, e.g. 0xFFFF as -1
    if (value->max <= value->min && value->bits > 0 && value->bits < 32)
    {
        value->min = 0;
        value->max = (LONG)((1UL << value->bits) - 1);
    }
}

// Work out which of the device's axes are the sticks and triggers, the same guesses SDL makes for unknown gamepads
static bool generic_hid_open(struct ControllerMap *controller, const char *path)
{
    HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        TRACE("Generic gamepad: unable to open %s (%lu)\n", path, GetLastError());
        return false;
    }
    PHIDP_PREPARSED_DATA preparsed = NULL;
    BOOLEAN got_preparsed = HidD_GetPreparsedData(handle, &preparsed);
    CloseHandle(handle);
    if (!got_preparsed)
    {
        TRACE("Generic gamepad: unable to get preparsed data for %s\n", path);
        return false;
    }

    HIDP_CAPS caps;
    if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS || caps.InputReportByteLength > GENERIC_HID_MAX_REPORT)
    {
        TRACE("Generic gamepad: unusable input report (%u bytes)\n", caps.InputReportByteLength);
        HidD_FreePreparsedData(preparsed);
        return false;
    }

    struct GenericHid *generic = calloc(1, sizeof(*generic));
    generic->preparsed = preparsed;
    generic->report_length = caps.InputReportByteLength;

    USHORT value_count = caps.NumberInputValueCaps;
    HIDP_VALUE_CAPS *value_caps = calloc(value_count ? value_count : 1, sizeof(*value_caps));
    if (value_count && HidP_GetValueCaps(HidP_Input, value_caps, &value_count, preparsed) != HIDP_STATUS_SUCCESS)
        value_count = 0;

    USHORT button_count = caps.NumberInputButtonCaps;
    HIDP_BUTTON_CAPS *button_caps = calloc(button_count ? button_count : 1, sizeof(*button_caps));
    if (button_count && HidP_GetButtonCaps(HidP_Input, button_caps, &button_count, preparsed) != HIDP_STATUS_SUCCESS)
        button_count = 0;

    // Only one input report is read, prefer whichever holds the X axis, otherwise the buttons
    bool found_report = false;
    for (USHORT i = 0; i < value_count && !found_report; i++)
    {
        USAGE first = value_caps[i].IsRange ? value_caps[i].Range.UsageMin : value_caps[i].NotRange.Usage;
        USAGE last = value_caps[i].IsRange ? value_caps[i].Range.UsageMax : first;
        if (value_caps[i].UsagePage == HID_USAGE_PAGE_GENERIC && first <= HID_USAGE_GENERIC_X && last >= HID_USAGE_GENERIC_X)
        {
            generic->report_id = value_caps[i].ReportID;
            found_report = true;
        }
    }
    for (USHORT i = 0; i < button_count && !found_report; i++)
    {
        if (button_caps[i].UsagePage == HID_USAGE_PAGE_BUTTON)
        {
            generic->report_id = button_caps[i].ReportID;
            found_report = true;
        }
    }

    const HIDP_VALUE_CAPS *axes[0x40] = {0};
    const HIDP_VALUE_CAPS *accelerator = NULL;
    const HIDP_VALUE_CAPS *brake = NULL;
    for (USHORT i = 0; i < value_count; i++)
    {
        if (value_caps[i].ReportID != generic->report_id)
            continue;
        USAGE first = value_caps[i].IsRange ? value_caps[i].Range.UsageMin : value_caps[i].NotRange.Usage;
        USAGE last = value_caps[i].IsRange ? value_caps[i].Range.UsageMax : first;
        for (USAGE usage = first; usage <= last && usage >= first; usage++)
        {
            if (value_caps[i].UsagePage == HID_USAGE_PAGE_GENERIC && usage < 0x40 && !axes[usage])
                axes[usage] = &value_caps[i];
            if (value_caps[i].UsagePage == HID_USAGE_PAGE_SIMULATION && usage == HID_USAGE_SIMULATION_ACCELERATOR)
                accelerator = &value_caps[i];
            if (value_caps[i].UsagePage == HID_USAGE_PAGE_SIMULATION && usage == HID_USAGE_SIMULATION_BRAKE)
                brake = &value_caps[i];
        }
    }

#define GENERIC_AXIS(role, usage)                                      \
    do                                                                 \
    {                                                                  \
        if (axes[usage])                                               \
            generic_set_value(generic, role, axes[usage], usage);      \
    } while (0)
    GENERIC_AXIS(generic_lx, HID_USAGE_GENERIC_X);
    GENERIC_AXIS(generic_ly, HID_USAGE_GENERIC_Y);
    GENERIC_AXIS(generic_hat, HID_USAGE_GENERIC_HATSWITCH);
    if (axes[HID_USAGE_GENERIC_RZ])
    {
        // most HID gamepads: right stick on Z / Rz, analog triggers (if any) on Rx / Ry or brake / accelerator
        GENERIC_AXIS(generic_rx, HID_USAGE_GENERIC_Z);
        GENERIC_AXIS(generic_ry, HID_USAGE_GENERIC_RZ);
        GENERIC_AXIS(generic_lt, HID_USAGE_GENERIC_RX);
        GENERIC_AXIS(generic_rt, HID_USAGE_GENERIC_RY);
    }
    else if (axes[HID_USAGE_GENERIC_RX] && axes[HID_USAGE_GENERIC_RY])
    {
        // Xbox style: right stick on Rx / Ry and both triggers sharing Z
        GENERIC_AXIS(generic_rx, HID_USAGE_GENERIC_RX);
        GENERIC_AXIS(generic_ry, HID_USAGE_GENERIC_RY);
        GENERIC_AXIS(generic_combined_triggers, HID_USAGE_GENERIC_Z);
    }
    else
    {
        GENERIC_AXIS(generic_rx, HID_USAGE_GENERIC_Z);
    }
#undef GENERIC_AXIS
    if (brake)
        generic_set_value(generic, generic_lt, brake, HID_USAGE_SIMULATION_BRAKE);
    if (accelerator)
        generic_set_value(generic, generic_rt, accelerator, HID_USAGE_SIMULATION_ACCELERATOR);
    free(value_caps);
    free(button_caps);

    generic_load_button_map(generic, controller->caps.vid, controller->caps.pid);
    controller->generic = generic;

    static const char *role_names[] = {"lx", "ly", "rx", "ry", "lt", "rt", "combined triggers", "hat"};
    TRACE("Generic gamepad: report %u, %u bytes\n", generic->report_id, generic->report_length);
    for (int i = 0; i < generic_value_count; i++)
    {
        if (generic->values[i].present)
            TRACE("  %s: page %02x usage %02x (%ld - %ld, %u bits)\n", role_names[i], generic->values[i].page,
                  generic->values[i].usage, generic->values[i].min, generic->values[i].max, generic->values[i].bits);
    }
    return true;
}

// Reads a value scaled to 0 - 65535, false when it is missing or outside its range (e.g. a centred hat)
static bool generic_read_value(struct GenericHid *generic, generic_value_role_t role, PCHAR report, uint32_t *out)
{
    generic_value_t *value = &generic->values[role];
    if (!value->present)
        return false;
    ULONG raw;
    if (HidP_GetUsageValue(HidP_Input, value->page, 0, value->usage, &raw, generic->preparsed, report, generic->report_length) != HIDP_STATUS_SUCCESS)
        return false;
    LONG signed_value = (LONG)raw;
    if (value->min < 0 && value->bits > 0 && value->bits < 32 && (raw & (1UL << (value->bits - 1))))
        signed_value = (LONG)(raw | ~((1UL << value->bits) - 1));
    if (signed_value < value->min || signed_value > value->max || value->max == value->min)
        return false;
    if (role == generic_hat)
    {
        // hats aren't scaled, the caller wants the direction
        *out = (uint32_t)(signed_value - value->min);
        return true;
    }
    *out = (uint32_t)(((int64_t)signed_value - value->min) * 65535 / ((int64_t)value->max - value->min));
    return true;
}

static void generic_hid_update(struct GenericHid *generic, const uint8_t *data, int size, XINPUT_GAMEPAD *gamepad)
{
    uint8_t report[GENERIC_HID_MAX_REPORT] = {0};
    if (generic->report_id)
    {
        // anything other than the gamepad's report keeps the last state
        if (data[0] != generic->report_id)
            return;
        memcpy(report, data, min(size, generic->report_length));
    }
    else
    {
        // hidapi drops the 0 report ID windows puts in front of reports from devices without report IDs, but HidP wants it
        memcpy(report + 1, data, min(size, generic->report_length - 1));
    }

    uint32_t value;
    if (generic_read_value(generic, generic_lx, (PCHAR)report, &value))
        gamepad->sThumbLX = (SHORT)(value - 32768);
    if (generic_read_value(generic, generic_ly, (PCHAR)report, &value))
        gamepad->sThumbLY = (SHORT)(32767 - value);
    if (generic_read_value(generic, generic_rx, (PCHAR)report, &value))
        gamepad->sThumbRX = (SHORT)(value - 32768);
    if (generic_read_value(generic, generic_ry, (PCHAR)report, &value))
        gamepad->sThumbRY = (SHORT)(32767 - value);
    if (generic_read_value(generic, generic_lt, (PCHAR)report, &value))
        gamepad->bLeftTrigger = value >> 8;
    if (generic_read_value(generic, generic_rt, (PCHAR)report, &value))
        gamepad->bRightTrigger = value >> 8;
    if (generic_read_value(generic, generic_combined_triggers, (PCHAR)report, &value))
    {
        // above the middle is the left trigger, below is the right
        if (value > 32768)
            gamepad->bLeftTrigger = (value - 32768) >> 7;
        else
            gamepad->bRightTrigger = min((32768 - value) >> 7, 255);
    }

    if (generic_read_value(generic, generic_hat, (PCHAR)report, &value))
    {
        LONG positions = generic->values[generic_hat].max - generic->values[generic_hat].min + 1;
        // a 4 position hat only has the cardinal directions
        if (positions == 4)
            value *= 2;
        if (value < 8)
        {
            uint8_t dpad = dpad_bindings_reverse[value];
            if (dpad & UP)
                gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            if (dpad & DOWN)
                gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (dpad & LEFT)
                gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (dpad & RIGHT)
                gamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }
    }

    USAGE usages[128];
    ULONG usage_count = sizeof(usages) / sizeof(usages[0]);
    if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages, &usage_count, generic->preparsed, (PCHAR)report, generic->report_length) == HIDP_STATUS_SUCCESS)
    {
        for (ULONG i = 0; i < usage_count; i++)
        {
            if (usages[i] < 1 || usages[i] > GENERIC_HID_MAX_BUTTONS)
                continue;
            uint32_t button = generic->button_map[usages[i] - 1];
            if (button == GENERIC_BUTTON_LT)
                gamepad->bLeftTrigger = 255;
            else if (button == GENERIC_BUTTON_RT)
                gamepad->bRightTrigger = 255;
            else
                gamepad->wButtons |= (WORD)button;
        }
    }
}

#define SANTROLLER_DEVICE_REPORT_ID_FEATURES        0x10
#define SANTROLLER_DEVICE_FEATURES_SET_REPORT_SIZE  2
#define SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE  3

#define SANTROLLER_DEVICE_CAPABILITY_STANDARD_PLAYER_LEDS 0x01
#define SANTROLLER_DEVICE_CAPABILITY_JOYSTICK_RGB         0x02
#define SANTROLLER_DEVICE_CAPABILITY_INSTRUMENT_LED       0x04
#define SANTROLLER_DEVICE_CAPABILITY_RUMBLE               0x08
#define SANTROLLER_DEVICE_CAPABILITY_DPAD_AS_BUTTONS      0x10
#define SANTROLLER_DEVICE_CAPABILITY_Y_AXIS_INVERTED      0x20

static void dinput_fill_caps(struct CapsFlags *caps, long vidpid, uint16_t revision, hid_device *device)
{
    int i;
    HRESULT hr;
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
    caps->dpad_as_buttons = false;
    caps->y_axis_inverted = false;
    caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->vid = LOWORD(vidpid);
    caps->pid = HIWORD(vidpid);
    if (vidpid == MAKELONG(0x1209, 0x2883)) {
        GetSubtypeOverride("santroller", "santroller_subtype", &caps->subtype, &caps->device_type);
        caps->console_type = santroller_2;
        // Only query features when we own the HID handle; XInput-proxied devices already report their subtype
        hid_device *query_dev = device;
        if (query_dev) {
            int written = 0;
            const uint8_t featuresGetCommand[SANTROLLER_DEVICE_FEATURES_SET_REPORT_SIZE] = { SANTROLLER_DEVICE_REPORT_ID_FEATURES, 0 };
            for (int attempt = 0; attempt < 8; ++attempt) {
                written = hid_write(query_dev, featuresGetCommand, sizeof(featuresGetCommand));
                if (written == sizeof(featuresGetCommand)) {
                    break;
                }
            }

            if (written >= 2) {
                uint8_t data[SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE];
                for (int i = 0; i < 100; ++i) {
                    Sleep(1);
                    int read = hid_read_timeout(query_dev, data, sizeof(data), 1);
                    if (read < 0) {
                        break;
                    }
                    if (read == 0) {
                        continue;
                    }
                    if (read == SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE && data[0] == SANTROLLER_DEVICE_REPORT_ID_FEATURES) {
                        uint8_t sub_type = data[1];
                        uint8_t capabilities = data[2];

                        caps->dpad_as_buttons = (capabilities & SANTROLLER_DEVICE_CAPABILITY_DPAD_AS_BUTTONS) != 0;
                        caps->y_axis_inverted = (sub_type == GAMEPAD) && ((capabilities & SANTROLLER_DEVICE_CAPABILITY_Y_AXIS_INVERTED) != 0);

                        switch (sub_type) {
                        case STAGE_KIT:
                            caps->subtype = XINPUT_DEVSUBTYPE_STAGE_KIT;
                            caps->device_type = stage_kit;
                            TRACE("Santroller 2 stage kit detected!\n");
                            break;
                        case GAMEPAD:
                            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
                            caps->device_type = gamepad;
                            TRACE("Santroller 2 gamepad detected!\n");
                            break;
                        case DANCE_PAD:
                            caps->subtype = XINPUT_DEVSUBTYPE_DANCE_PAD;
                            caps->device_type = gamepad;
                            TRACE("Santroller 2 dance pad detected!\n");
                            break;
                        case GUITAR_HERO_GUITAR:
                        case GUITAR_HERO_GUITAR_WT:
                        case FORTNITE_GUITAR:
                        case FORTNITE_GUITAR_STRUM:
                            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                            caps->device_type = gh_guitar;
                            TRACE("Santroller 2 gh guitar detected!\n");
                            break;
                        case ROCK_BAND_GUITAR:
                            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR;
                            caps->device_type = rb_guitar;
                            TRACE("Santroller 2 rb guitar detected!\n");
                            if (override_rb)
                            {
                                printf("Overriding RB guitar subtype with GH guitar subtype\n");
                                caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                            }
                            break;
                        case GUITAR_HERO_DRUMS:
                        case FORTNITE_DRUMS:
                            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
                            caps->device_type = gh_drum;
                            TRACE("Santroller 2 gh drum detected!\n");
                            break;
                        case ROCK_BAND_DRUMS:
                            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
                            caps->device_type = rb_drum;
                            TRACE("Santroller 2 rb drum detected!\n");
                            break;
                        case LIVE_GUITAR:
                            caps->subtype = XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE;
                            caps->device_type = live_guitar;
                            TRACE("Santroller 2 live guitar detected!\n");
                            break;
                        case DJ_HERO_TURNTABLE:
                            caps->subtype = XINPUT_DEVSUBTYPE_TURNTABLE;
                            caps->device_type = turntable;
                            TRACE("Santroller 2 turntable detected!\n");
                            break;
                        case ROCK_BAND_PRO_KEYS:
                            caps->subtype = XINPUT_DEVSUBTYPE_PRO_KEYS;
                            caps->device_type = pro_keys;
                            TRACE("Santroller 2 pro keys detected!\n");
                            break;
                        case ROCK_BAND_PRO_GUITAR_MUSTANG:
                            caps->subtype = XINPUT_DEVSUBTYPE_PRO_GUITAR;
                            caps->device_type = pro_guitar_mustang;
                            TRACE("Santroller 2 pro guitar mustang detected!\n");
                            break;
                        case ROCK_BAND_PRO_GUITAR_SQUIRE:
                            caps->subtype = XINPUT_DEVSUBTYPE_PRO_GUITAR;
                            caps->device_type = pro_guitar_squire;
                            TRACE("Santroller 2 pro guitar squire detected!\n");
                            break;
                        case TAIKO:
                            caps->subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
                            caps->device_type = gh_drum;
                            TRACE("Santroller 2 taiko detected!\n");
                            break;
                        case ARCADE:
                            caps->subtype = XINPUT_DEVSUBTYPE_ARCADE_PAD;
                            caps->device_type = gamepad;
                            TRACE("Santroller 2 arcade detected!\n");
                            break;
                        default:
                            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
                            caps->device_type = gamepad;
                            TRACE("Santroller 2 subtype %d detected!\n", sub_type);
                            break;
                        }
                        break;
                    }
                }
            }
        }
    }
    if (vidpid == MAKELONG(0x1209, 0x2882))
    {
        uint8_t type = revision >> 8;
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

    if (device && !IsVendorHidRaw(LOWORD(vidpid)))
    {
        caps->console_type = linux_sdl;
        parse_wine_sdl_descriptor(device, &caps->axes, &caps->buttons, &caps->pov);
        TRACE("Configured device %08lx as linux_sdl (axes: %d, buttons: %d, hat: %d)\n",
              vidpid, caps->axes, caps->buttons, caps->pov);
    }
    else if (device && caps->console_type == xinput)
    {
        // nothing above claimed it, see if it is a PlayStation gamepad (or an instrument we don't know the VID/PID of)
        if (!detect_playstation_controller(caps, vidpid, device))
        {
            // read it as a plain HID gamepad, using its report descriptor
            caps->console_type = generic_hid;
            caps->device_type = gamepad;
            caps->subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
            TRACE("Unrecognised HID device %08lx, treating it as a generic gamepad\n", vidpid);
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
    hid_set_nonblocking(device, 1);
    dinput_fill_caps(&controllers[dinput.mapped].caps, MAKELONG(cur_dev->vendor_id, cur_dev->product_id), cur_dev->release_number, device);
    if (controllers[dinput.mapped].caps.console_type == generic_hid && !generic_hid_open(&controllers[dinput.mapped], cur_dev->path))
    {
        TRACE("Unable to read %04hx:%04hx as a generic gamepad, it will not report any input\n", cur_dev->vendor_id, cur_dev->product_id);
        controllers[dinput.mapped].caps.console_type = xinput;
    }

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
        bool already_proxied = false;
        for (int i = 0; i < dinput.mapped; i++)
        {
            if (controllers[i].connected && controllers[i].xinput_index != -1 &&
                controllers[i].caps.vid == cur_dev->vendor_id && controllers[i].caps.pid == cur_dev->product_id)
            {
                already_proxied = true;
                break;
            }
        }
        if (already_proxied)
        {
            printf("found device %04hx:%04hx already proxied as xinput, skipping\n", cur_dev->vendor_id, cur_dev->product_id);
            continue;
        }
        if (!(cur_dev->usage == HID_USAGE_GENERIC_JOYSTICK || cur_dev->usage == HID_USAGE_GENERIC_GAMEPAD) || cur_dev->usage_page != HID_USAGE_PAGE_GENERIC)
        {
            continue;
        }
        print_device(cur_dev);
    }
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
        uint8_t data[GENERIC_HID_MAX_REPORT];
        int size = hid_read(controllers[index].device, data, sizeof(data));
        if (size <= 0)
        {
            return;
        }
        if (controllers[index].caps.console_type == santroller_2)
        {
            // Santroller 2 reports are based on xinput but use a HID style hat unless dpad_as_buttons is set
            XINPUT_GAMEPAD* xinput_gamepad = (XINPUT_GAMEPAD*)(&data[2]);
            memcpy(&gamepad_state, xinput_gamepad, sizeof(XINPUT_GAMEPAD));
            if (!controllers[index].caps.dpad_as_buttons)
            {
                uint8_t dpad = (data[2] & 0xf) >= 0x08 ? 0 : dpad_bindings_reverse[(data[2] & 0xf)];
                bool up = dpad & UP;
                bool left = dpad & LEFT;
                bool down = dpad & DOWN;
                bool right = dpad & RIGHT;
                gamepad_state.wButtons &= ~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
                if (up)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                if (down)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (left)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (right)
                    gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            }
            if (controllers[index].caps.device_type == gamepad && controllers[index].caps.y_axis_inverted)
            {
                gamepad_state.sThumbLY = ~gamepad_state.sThumbLY;
                gamepad_state.sThumbRY = ~gamepad_state.sThumbRY;
            }
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
                    if (report->leftThumbClick)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                    if (report->rightThumbClick)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
                    if (report->guide)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_GUIDE;
                    gamepad_state.bLeftTrigger = report->leftTrigger;
                    gamepad_state.bRightTrigger = report->rightTrigger;
                    gamepad_state.sThumbLX = ps_axis(report->leftStickX);
                    gamepad_state.sThumbLY = ps_axis_inverted(report->leftStickY);
                    gamepad_state.sThumbRX = ps_axis(report->rightStickX);
                    gamepad_state.sThumbRY = ps_axis_inverted(report->rightStickY);
                }
                else if (controllers[index].caps.device_type == gh_guitar)
                {
                    // A PS2 guitar through an adapter that copies the PS2 report onto a DS3 one (see PS2GuitarOnPS3_Data_t).
                    // D-pad left is always held, it is how the guitar identifies itself, so it isn't passed on.
                    if (report->r2)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A; // green
                    if (report->b)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B; // red (circle)
                    if (report->y)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y; // yellow (triangle)
                    if (report->a)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X; // blue (cross)
                    if (report->x || report->leftShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // orange (square, or L1 on some guitars)
                    if (report->dpadUp)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP; // strum up
                    if (report->dpadDown)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; // strum down
                    if (report->start)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (report->back || report->rightShoulder)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK; // select, or the star power pedal (R1)
                    if (report->guide)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_GUIDE;

                    // whammy is the left stick's Y axis, resting at 0x7F and going down to 0x00 when pushed all the way
                    uint8_t whammy = 0x7F - min(report->leftStickY, 0x7F);
                    gamepad_state.sThumbRX = (SHORT)(whammy * 65535 / 0x7F - 32768);
                    // tilt is digital, on L2
                    gamepad_state.sThumbRY = report->l2 ? 32767 : 0;
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
                case gamepad:
                {
                    PS3ThirdPartyGamepad_Data_t *report = (PS3ThirdPartyGamepad_Data_t *)data;
                    PS_FILL_GAMEPAD(gamepad_state, report, up, down, left, right);
                    break;
                }
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
            case gamepad:
            {
                PS4Gamepad_Data_t *report = (PS4Gamepad_Data_t *)data;
                // report 1 is the only one with input in it, keep the last state for anything else
                if (report->report_id != 0x01)
                    return;
                PS_FILL_GAMEPAD(gamepad_state, report, up, down, left, right);
                break;
            }
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
            case gamepad:
            {
                PS5Gamepad_Data_t *report = (PS5Gamepad_Data_t *)data;
                // report 1 is the only one with input in it, keep the last state for anything else
                if (report->report_id != 0x01)
                    return;
                PS_FILL_GAMEPAD(gamepad_state, report, up, down, left, right);
                break;
            }
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
        if (controllers[index].caps.console_type == generic_hid && controllers[index].generic)
        {
            generic_hid_update(controllers[index].generic, data, size, &gamepad_state);
        }
        if (controllers[index].caps.console_type == linux_sdl)
        {
            if (size >= 1 + caps.axes * 2)
            {
                int16_t axis0 = *(int16_t *)(data + 1);
                int16_t axis1 = (caps.axes > 1) ? *(int16_t *)(data + 3) : 0;
                int16_t axis2 = (caps.axes > 2) ? *(int16_t *)(data + 5) : 0;
                int16_t axis3 = (caps.axes > 3) ? *(int16_t *)(data + 7) : 0;

                bool up = false;
                bool down = false;
                bool left = false;
                bool right = false;

                int hat_offset = 1 + caps.axes * 2;
                if (caps.pov && size > hat_offset)
                {
                    uint8_t hat_val = data[hat_offset] & 0x0F;
                    up = (hat_val == 1 || hat_val == 2 || hat_val == 8);
                    right = (hat_val == 2 || hat_val == 3 || hat_val == 4);
                    down = (hat_val == 4 || hat_val == 5 || hat_val == 6);
                    left = (hat_val == 6 || hat_val == 7 || hat_val == 8);
                }

                int button_offset = hat_offset + (caps.pov ? 1 : 0);
                uint16_t btn_mask = 0;
                if (size > button_offset)
                {
                    btn_mask = data[button_offset];
                    if (size > button_offset + 1)
                    {
                        btn_mask |= (data[button_offset + 1] << 8);
                    }
                }

                switch (controllers[index].caps.device_type)
                {
                case gh_guitar:
                {
                    if (btn_mask & (1 << 0))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A; // Green
                    if (btn_mask & (1 << 1))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B; // Red
                    if (btn_mask & (1 << 2))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y; // Yellow (Triangle on PS3 GH)
                    if (btn_mask & (1 << 3))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X; // Blue (Square on PS3 GH)
                    if (btn_mask & (1 << 4))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // Orange

                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

                    if (btn_mask & (1 << 9))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (btn_mask & (1 << 8))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    gamepad_state.sThumbRX = axis2;
                    if (axis3 < 0)
                    {
                        int32_t val = -((int32_t)axis3) * 3;
                        gamepad_state.sThumbRY = (val > 32767) ? 32767 : (int16_t)val;
                    }
                    else
                    {
                        gamepad_state.sThumbRY = 0;
                    }
                    gamepad_state.sThumbLX = axis0;
                    gamepad_state.sThumbLY = axis1;
                    break;
                }
                case rb_guitar:
                {
                    if (btn_mask & (1 << 0))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A; // Green
                    if (btn_mask & (1 << 1))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B; // Red
                    if (btn_mask & (1 << 2))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X; // Blue (Triangle)
                    if (btn_mask & (1 << 3))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y; // Yellow (Square)
                    if (btn_mask & (1 << 4))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // Orange
                    if ((btn_mask & (1 << 6)) || (btn_mask & (1 << 10)) || (btn_mask & (1 << 11)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; // Solo frets

                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

                    if (btn_mask & (1 << 9))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (btn_mask & (1 << 8))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    gamepad_state.sThumbRX = axis2;
                    if (btn_mask & (1 << 5))
                        gamepad_state.sThumbRY = 32767;
                    else if (caps.axes > 3 && axis3 != 0)
                        gamepad_state.sThumbRY = axis3;
                    else
                        gamepad_state.sThumbRY = 0;
                    gamepad_state.sThumbLX = axis0;
                    gamepad_state.sThumbLY = axis1;
                    break;
                }
                case live_guitar:
                {
                    if (btn_mask & (1 << 0))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A; // Black 1
                    if (btn_mask & (1 << 1))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B; // Black 2
                    if (btn_mask & (1 << 2))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y; // Black 3
                    if (btn_mask & (1 << 3))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X; // White 1
                    if (btn_mask & (1 << 4))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // White 2
                    if (btn_mask & (1 << 5))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; // White 3
                    if ((btn_mask & (1 << 6)) || (btn_mask & (1 << 10)) || (btn_mask & (1 << 11)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

                    if (btn_mask & (1 << 9))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (btn_mask & (1 << 8))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;

                    gamepad_state.sThumbRX = axis2;
                    gamepad_state.sThumbRY = axis3;
                    break;
                }
                case gh_drum:
                case rb_drum:
                {
                    if (btn_mask & (1 << 0))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A; // Green
                    if (btn_mask & (1 << 1))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B; // Red
                    if (btn_mask & (1 << 2))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y; // Yellow
                    if (btn_mask & (1 << 3))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X; // Blue
                    if (btn_mask & (1 << 4))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // Kick
                    if ((btn_mask & (1 << 5)) || (btn_mask & (1 << 6)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    if ((btn_mask & (1 << 7)) || (btn_mask & (1 << 10)) || (btn_mask & (1 << 11)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

                    if (btn_mask & (1 << 9))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if (btn_mask & (1 << 8))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                    break;
                }
                default:
                {
                    if (btn_mask & (1 << 0))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_A;
                    if (btn_mask & (1 << 1))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_B;
                    if (btn_mask & (1 << 2))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_Y;
                    if (btn_mask & (1 << 3))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_X;
                    if (btn_mask & (1 << 4))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (btn_mask & (1 << 5))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                    if (btn_mask & (1 << 8))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_BACK;
                    if (btn_mask & (1 << 9))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_START;
                    if ((btn_mask & (1 << 10)) || (btn_mask & (1 << 11)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
                    if ((btn_mask & (1 << 11)) || (btn_mask & (1 << 12)))
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

                    if (up)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
                    if (down)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
                    if (left)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
                    if (right)
                        gamepad_state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

                    gamepad_state.sThumbLX = axis0;
                    gamepad_state.sThumbLY = ~axis1;
                    gamepad_state.sThumbRX = axis2;
                    gamepad_state.sThumbRY = (caps.axes > 3) ? ~axis3 : 0;
                    break;
                }
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

static unsigned __stdcall dinput_update_all(void *argument)
{
    while (1)
    {
        for (int index = 0; index < XUSER_MAX_COUNT; index++)
        {
            if (controllers[index].connected && controllers[index].xinput_index == -1 &&
                controllers[index].caps.console_type != xbox_one)
            {
                dinput_update(index);
            }
        }
    }
}

/* ===================== Xbox One (GIP) instruments ========================= */
// Windows doesn't expose Xbox One instruments through XInput, so read them straight from the GIP driver.
// Input only arrives while this process is focused, which is fine as we live inside the game.

static int gip_find_controller(uint64_t device_id)
{
    for (int i = 0; i < dinput.mapped; i++)
    {
        if (controllers[i].caps.console_type == xbox_one && controllers[i].gip_device_id == device_id)
            return i;
    }
    return -1;
}

static bool gip_proxied_by_xinput(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < dinput.mapped; i++)
    {
        if (controllers[i].xinput_index != -1 && controllers[i].caps.vid == vid && controllers[i].caps.pid == pid)
            return true;
    }
    return false;
}

static void gip_set_state(int index, const XINPUT_GAMEPAD *gamepad_state)
{
    if (memcmp(&controllers[index].state.Gamepad, gamepad_state, sizeof(XINPUT_GAMEPAD)))
    {
        controllers[index].state.Gamepad = *gamepad_state;
        controllers[index].state.dwPacketNumber++;
    }
}

static device_type_t gip_type_from_vidpid(uint16_t vid, uint16_t pid)
{
    switch (MAKELONG(vid, pid))
    {
    case MAKELONG(0x0738, 0x4161): // MadCatz Stratocaster
    case MAKELONG(0x0E6F, 0x0170): // PDP Jaguar
    case MAKELONG(0x0E6F, 0x0248): // PDP Riffmaster
    case MAKELONG(0x3651, 0x4161): // CRKD
        return rb_guitar;
    case MAKELONG(0x0738, 0x4262): // MadCatz drums
    case MAKELONG(0x0E6F, 0x0171): // PDP drums
        return rb_drum;
    }
    // Anything else (gamepads etc) is left to XInput
    return gamepad;
}

static device_type_t gip_type_from_descriptor(const uint8_t *data, uint32_t length)
{
    if (length < sizeof(GipDescriptorHeader_t))
        return gamepad;
    const GipDescriptorHeader_t *header = (const GipDescriptorHeader_t *)data;
    const uint8_t *base = data + offsetof(GipDescriptorHeader_t, offsetExternalCommands);
    uint32_t base_length = length - offsetof(GipDescriptorHeader_t, offsetExternalCommands);
    uint16_t offset = header->offsetInterfaceGuids;
    if (!offset || offset >= base_length)
        return gamepad;
    uint8_t count = base[offset];
    for (int i = 0; i < count && offset + 1 + (i + 1) * sizeof(GUID) <= base_length; i++)
    {
        const GUID *guid = (const GUID *)(base + offset + 1 + i * sizeof(GUID));
        if (IsEqualGUID(guid, &GIP_GUID_GUITAR_MADCATZ) || IsEqualGUID(guid, &GIP_GUID_GUITAR_PDP))
            return rb_guitar;
        if (IsEqualGUID(guid, &GIP_GUID_DRUMS_MADCATZ) || IsEqualGUID(guid, &GIP_GUID_DRUMS_PDP))
            return rb_drum;
    }
    return gamepad;
}

static void gip_add_controller(uint64_t device_id, uint16_t vid, uint16_t pid, device_type_t type)
{
    int index = gip_find_controller(device_id);
    if (index == -1)
    {
        // Reuse the slot of a disconnected GIP device so reconnecting keeps the player index
        for (int i = 0; i < dinput.mapped; i++)
        {
            if (controllers[i].caps.console_type == xbox_one && !controllers[i].connected)
            {
                index = i;
                break;
            }
        }
    }
    if (index == -1)
    {
        if (dinput.mapped == XUSER_MAX_COUNT)
        {
            TRACE("no free slot for xbox one device %04x:%04x\n", vid, pid);
            return;
        }
        index = dinput.mapped++;
    }
    TRACE("xbox one %s %04x:%04x in slot %d\n", type == rb_drum ? "drums" : "guitar", vid, pid, index);
    struct ControllerMap *controller = &controllers[index];
    memset(&controller->caps, 0, sizeof(controller->caps));
    controller->caps.vid = vid;
    controller->caps.pid = pid;
    controller->caps.pov = TRUE;
    controller->caps.device_type = type;
    controller->caps.console_type = xbox_one;
    if (type == rb_drum)
        controller->caps.subtype = XINPUT_DEVSUBTYPE_DRUM_KIT;
    else
        controller->caps.subtype = override_rb ? XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE : XINPUT_DEVSUBTYPE_GUITAR;
    controller->xinput_index = -1;
    controller->device = NULL;
    controller->gip_device_id = device_id;
    // Sequence numbers wrap from 255 to 1, so 0 never matches the first report
    controller->gip_last_sequence = 0;
    memset(&controller->state, 0, sizeof(controller->state));
    controller->connected = TRUE;
}

static void gip_disconnect(int index)
{
    XINPUT_GAMEPAD gamepad_state = {0};
    TRACE("xbox one device in slot %d disconnected\n", index);
    gip_set_state(index, &gamepad_state);
    controllers[index].connected = FALSE;
}

static void gip_map_common_buttons(uint16_t buttons, XINPUT_GAMEPAD *gamepad_state)
{
    if (buttons & GIP_BUTTON_MENU)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_START;
    if (buttons & GIP_BUTTON_VIEW)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_BACK;
    if (buttons & GIP_BUTTON_DPAD_UP)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (buttons & GIP_BUTTON_DPAD_DOWN)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (buttons & GIP_BUTTON_DPAD_LEFT)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (buttons & GIP_BUTTON_DPAD_RIGHT)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
}

static void gip_map_guitar(const uint8_t *data, uint32_t length, XINPUT_GAMEPAD *gamepad_state)
{
    if (length < sizeof(GipGuitarInput_t))
        return;
    const GipGuitarInput_t *report = (const GipGuitarInput_t *)data;
    gip_map_common_buttons(report->buttons, gamepad_state);

    // The fret flags in the button bitmask overlap with the Riffmaster joystick click, so use the fret bitmasks
    uint8_t frets = report->upperFrets | report->lowerFrets;
    if (frets & GIP_FRET_GREEN)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_A;
    if (frets & GIP_FRET_RED)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_B;
    if (frets & GIP_FRET_YELLOW)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_Y;
    if (frets & GIP_FRET_BLUE)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_X;
    if (frets & GIP_FRET_ORANGE)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (report->lowerFrets)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

    gamepad_state->sThumbRX = (report->whammy << 8) - 32767;
    gamepad_state->sThumbRY = report->tilt << 7;
    uint8_t pickup = report->pickup >> 4;
    if (pickup < sizeof(pickup_vals))
        gamepad_state->bLeftTrigger = pickup_vals[pickup];
}

static void gip_map_drums(const uint8_t *data, uint32_t length, XINPUT_GAMEPAD *gamepad_state)
{
    if (length < sizeof(GipDrumInput_t))
        return;
    const GipDrumInput_t *report = (const GipDrumInput_t *)data;
    gip_map_common_buttons(report->buttons, gamepad_state);

    // Face buttons are still used for menus. PDP kits only set X/Y for blue/yellow pads,
    // so pad colours come from the velocities instead
    if (report->buttons & GIP_BUTTON_A)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_A;
    if (report->buttons & GIP_BUTTON_B)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_B;
    if (report->buttons & GIP_BUTTON_X)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_X;
    if (report->buttons & GIP_BUTTON_Y)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_Y;
    if (report->buttons & GIP_DRUM_KICK1)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (report->buttons & GIP_DRUM_KICK2)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

    if (report->redYellowPad >> 4)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (report->redYellowPad & 0x0F)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (report->blueGreenPad >> 4)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (report->blueGreenPad & 0x0F)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_RIGHT_THUMB;
    if (report->yellowBlueCymbal >> 4)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP;
    if (report->yellowBlueCymbal & 0x0F)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_DOWN;
    if (report->greenCymbal >> 4)
        gamepad_state->wButtons |= XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_RIGHT_SHOULDER;
}

static void gip_handle_message(const uint8_t *buf, DWORD size)
{
    if (size < sizeof(GipHeader_t))
        return;
    const GipHeader_t *header = (const GipHeader_t *)buf;
    const uint8_t *data = buf + sizeof(GipHeader_t);
    uint32_t length = header->length;
    if (length > size - sizeof(GipHeader_t))
        length = size - sizeof(GipHeader_t);
    bool system = header->flags & 0x20;
    int index = gip_find_controller(header->deviceId);

    if (system && header->commandId == GIP_CMD_ARRIVAL && length >= sizeof(GipArrival_t))
    {
        const GipArrival_t *arrival = (const GipArrival_t *)data;
        TRACE("xbox one device arrived: %04x:%04x\n", arrival->vendorId, arrival->productId);
        device_type_t type = gip_type_from_vidpid(arrival->vendorId, arrival->productId);
        if (type != gamepad && !gip_proxied_by_xinput(arrival->vendorId, arrival->productId))
            gip_add_controller(header->deviceId, arrival->vendorId, arrival->productId, type);
    }
    else if (system && header->commandId == GIP_CMD_DESCRIPTOR && length >= sizeof(GipDescriptorHeader_t))
    {
        // Picks up instruments missing from the VID/PID list
        const GipDescriptorHeader_t *descriptor = (const GipDescriptorHeader_t *)data;
        if (index != -1 && controllers[index].connected)
            return;
        device_type_t type = gip_type_from_descriptor(data, length);
        if (type != gamepad && !gip_proxied_by_xinput(descriptor->vendorId, descriptor->productId))
            gip_add_controller(header->deviceId, descriptor->vendorId, descriptor->productId, type);
    }
    else if (system && header->commandId == GIP_CMD_STATUS && length >= 1)
    {
        if (index != -1 && controllers[index].connected && !(data[0] & GIP_STATUS_CONNECTED))
            gip_disconnect(index);
    }
    else if (system && header->commandId == GIP_CMD_FOCUS && length >= 1)
    {
        // Input stops while unfocused, so release everything rather than leaving buttons held
        if (data[0] == 0)
        {
            XINPUT_GAMEPAD gamepad_state = {0};
            for (int i = 0; i < dinput.mapped; i++)
            {
                if (controllers[i].caps.console_type == xbox_one && controllers[i].connected)
                    gip_set_state(i, &gamepad_state);
            }
        }
    }
    else if (!system && header->commandId == GIP_CMD_INPUT)
    {
        if (index == -1 || !controllers[index].connected)
            return;
        // Drums send every report twice
        if (header->sequence == controllers[index].gip_last_sequence)
            return;
        controllers[index].gip_last_sequence = header->sequence;
        XINPUT_GAMEPAD gamepad_state = {0};
        if (controllers[index].caps.device_type == rb_drum)
            gip_map_drums(data, length, &gamepad_state);
        else
            gip_map_guitar(data, length, &gamepad_state);
        gip_set_state(index, &gamepad_state);
    }
}

static void gip_reenumerate(HANDLE handle)
{
    // Makes the driver resend arrival and descriptor messages for devices that are already connected
    OVERLAPPED ov = {0};
    DWORD unused;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(handle, GIP_ADD_REENUMERATE_CALLER_CONTEXT, NULL, 0, NULL, 0, NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(handle, &ov, &unused, TRUE);
    if (!ok)
        TRACE("xbox one re-enumerate failed: %lu\n", GetLastError());
    CloseHandle(ov.hEvent);
}

static unsigned __stdcall gip_update_all(void *argument)
{
    static uint8_t buf[4096];
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (1)
    {
        HANDLE handle = CreateFileW(L"\\\\.\\XboxGIP", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (handle == INVALID_HANDLE_VALUE)
        {
            // Fails while no Xbox One devices are connected
            Sleep(2000);
            continue;
        }
        TRACE("opened XboxGIP\n");
        bool reenumerated = false;
        while (1)
        {
            DWORD size = 0;
            ResetEvent(ov.hEvent);
            if (!ReadFile(handle, buf, sizeof(buf), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
                break;
            // Only request re-enumeration once a read is pending, so the resent messages aren't missed
            if (!reenumerated)
            {
                gip_reenumerate(handle);
                reenumerated = true;
            }
            if (!GetOverlappedResult(handle, &ov, &size, TRUE))
                break;
            gip_handle_message(buf, size);
        }
        TRACE("lost XboxGIP: %lu\n", GetLastError());
        CloseHandle(handle);
        for (int i = 0; i < dinput.mapped; i++)
        {
            if (controllers[i].caps.console_type == xbox_one && controllers[i].connected)
                gip_disconnect(i);
        }
        Sleep(2000);
    }
}

HANDLE hThread;
unsigned threadID;
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
    if (ShowConsole() && AllocConsole())
    {

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

        if (NULL != ProcXInputGetState && NULL != ProcXInputGetCapabilitiesEx && hasEx)
        {
            TRACE("found procs ex\r\n");
            for (int i = 0; i < 4; i++)
            {
                XINPUT_CAPABILITIES_EX caps2;
                if ((ProcXInputGetCapabilitiesEx)(1, i, 0, &caps2) != ERROR_DEVICE_NOT_CONNECTED)
                {
                    TRACE("xinput vid: %04x, pid: %04x, subtype: %02x!\n", caps2.VendorId, caps2.ProductId, caps2.Capabilities.SubType);
                    TRACE("found xinput device, proxying\r\n");
                    controllers[dinput.mapped].xinput_index = i;
                    controllers[dinput.mapped].connected = TRUE;
                    dinput_fill_caps(&controllers[dinput.mapped].caps, MAKELONG(caps2.VendorId, caps2.ProductId), caps2.VersionNumber, NULL);
                    // fill_caps defaults unknown devices to gamepad, so trust the subtype XInput reported unless it's a plain gamepad
                    if (caps2.Capabilities.SubType != XINPUT_DEVSUBTYPE_GAMEPAD)
                        controllers[dinput.mapped].caps.subtype = caps2.Capabilities.SubType;
                    if (controllers[dinput.mapped].caps.console_type == raphnet_wii || controllers[dinput.mapped].caps.console_type == raphnet_ps2)
                        TRACE("raphnet adapter found via Wine's SDL mapping, enable hidraw for vendor 289b to use it as an instrument\n");
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
        else if (NULL != ProcXInputGetState && NULL != ProcXInputGetCapabilities)
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
    hThread = (HANDLE)_beginthreadex(NULL, 0, &dinput_update_all, NULL, 0,
                                     &threadID);
    // Wine has no GIP driver
    if (!IsWine())
        CloseHandle((HANDLE)_beginthreadex(NULL, 0, &gip_update_all, NULL, 0, NULL));
}

void dumb_Init(DWORD version)
{
    // Does nothing
}

void dumb_Cleanup()
{
    // Does nothing as well
}

static void FixupProxiedXInput(int index, XINPUT_STATE *state)
{
    if (!state)
        return;

    // Check if this is a PlayStation guitar (PS3, PS4, PS5)
    bool is_ps_guitar = (controllers[index].caps.device_type == gh_guitar || controllers[index].caps.device_type == rb_guitar) &&
                        (controllers[index].caps.console_type == ps3 ||
                         controllers[index].caps.console_type == ps4 ||
                         controllers[index].caps.console_type == ps5);

    if (is_ps_guitar)
    {
        // 1. Swap Yellow and Blue frets for RB PS3, PS4, and PS5 guitars:
        // Wine/SDL maps Triangle (North) to XINPUT_GAMEPAD_Y and Square (West) to XINPUT_GAMEPAD_X.
        // PS3 Guitar Hero guitars already have Triangle as Yellow and Square as Blue in their hardware reports,
        // so Wine's mapping is already correct for them.
        // Other PlayStation guitars (Rock Band PS3, PS4, PS5) have Square as Yellow and Triangle as Blue,
        // so they need X and Y swapped.
        bool is_ps3_gh = (controllers[index].caps.device_type == gh_guitar &&
                          controllers[index].caps.console_type == ps3);
        if (!is_ps3_gh)
        {
            WORD buttons = state->Gamepad.wButtons;
            bool x = (buttons & XINPUT_GAMEPAD_X) != 0;
            bool y = (buttons & XINPUT_GAMEPAD_Y) != 0;
            buttons &= ~(XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y);
            if (x) buttons |= XINPUT_GAMEPAD_Y;
            if (y) buttons |= XINPUT_GAMEPAD_X;
            state->Gamepad.wButtons = buttons;
        }

        // 2. Tilt handling:
        // - PS3 Rock Band guitars have a digital tilt switch wired to R1 (RIGHT_SHOULDER)
        if (controllers[index].caps.console_type == ps3 && controllers[index].caps.device_type == rb_guitar)
        {
            if (state->Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
            {
                state->Gamepad.sThumbRY = 32767;
                state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_RIGHT_SHOULDER;
            }
            else
            {
                state->Gamepad.sThumbRY = 0;
            }
        }
        // - PS4/PS5 guitars report analog tilt on ABS_RZ (bRightTrigger: 0..255)
        else if (controllers[index].caps.console_type == ps4 || controllers[index].caps.console_type == ps5)
        {
            state->Gamepad.sThumbRY = ((int32_t)state->Gamepad.bRightTrigger * 32767) / 255;
        }
        // - PS3 Guitar Hero guitars report analog tilt on ABS_RY (sThumbRY), deflecting negatively when tilted up
        else if (is_ps3_gh)
        {
            if (state->Gamepad.sThumbRY < 0)
            {
                int32_t val = -((int32_t)state->Gamepad.sThumbRY) * 3;
                state->Gamepad.sThumbRY = (val > 32767) ? 32767 : (int16_t)val;
            }
            else
            {
                state->Gamepad.sThumbRY = 0;
            }
        }

        // 3. Whammy for PS4/PS5 guitars:
        // hid-sony reports whammy on ABS_Z (bLeftTrigger).
        if ((controllers[index].caps.console_type == ps4 || controllers[index].caps.console_type == ps5) &&
            state->Gamepad.bLeftTrigger > 0)
        {
            state->Gamepad.sThumbRX = ((int32_t)state->Gamepad.bLeftTrigger * 32767) / 255;
        }

        // 4. Solo frets on PS3 Rock Band guitars (L2 / bLeftTrigger):
        if (controllers[index].caps.console_type == ps3 &&
            controllers[index].caps.device_type == rb_guitar &&
            state->Gamepad.bLeftTrigger > 128)
        {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }
    }
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
        int ret;
        if (ProcXInputGetStateEx)
            ret = (ProcXInputGetStateEx)(controllers[index].xinput_index, state);
        else if (ProcXInputGetState)
            ret = (ProcXInputGetState)(controllers[index].xinput_index, state);
        else
            ret = ERROR_DEVICE_NOT_CONNECTED;

        if (ret == ERROR_DEVICE_NOT_CONNECTED)
        {
            controllers[index].connected = false;
            return ret;
        }
        if (ret == ERROR_SUCCESS)
        {
            FixupProxiedXInput(index, state);
        }
        return ret;
    }

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
        int ret;
        if (ProcXInputGetState)
            ret = (ProcXInputGetState)(controllers[index].xinput_index, state);
        else if (ProcXInputGetStateEx)
            ret = (ProcXInputGetStateEx)(controllers[index].xinput_index, state);
        else
            ret = ERROR_DEVICE_NOT_CONNECTED;

        if (ret == ERROR_DEVICE_NOT_CONNECTED)
        {
            controllers[index].connected = false;
            return ret;
        }
        if (ret == ERROR_SUCCESS)
        {
            FixupProxiedXInput(index, state);
            state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;
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
