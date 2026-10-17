#pragma once
#include <stdint.h>

// Xbox One (GIP) devices read directly from the \\.\XboxGIP driver interface
// https://gist.github.com/TheNathannator/bcebc77e653f71e77634144940871596
// https://github.com/TheNathannator/PlasticBand/tree/main/Docs/Instruments

#define GIP_ADD_REENUMERATE_CALLER_CONTEXT 0x40001CD0

#define GIP_CMD_ARRIVAL 0x02
#define GIP_CMD_STATUS 0x03
#define GIP_CMD_DESCRIPTOR 0x04
#define GIP_CMD_INPUT 0x20
#define GIP_CMD_FOCUS 0xE0

typedef struct {
    uint64_t deviceId;
    uint8_t commandId;
    uint8_t flags;
    uint8_t sequence;
    uint8_t unknown1;
    uint32_t length;
    uint32_t unknown2;
} __attribute__((packed)) GipHeader_t;

typedef struct {
    uint64_t deviceId;
    uint16_t vendorId;
    uint16_t productId;
    uint16_t firmwareMajor;
    uint16_t firmwareMinor;
    uint16_t firmwareBuild;
    uint16_t firmwareRevision;
} __attribute__((packed)) GipArrival_t;

#define GIP_STATUS_CONNECTED 0x80

typedef struct {
    uint16_t vendorId;
    uint16_t productId;
    uint8_t unknown[14];
    uint16_t length;
    // Offsets are relative to offsetExternalCommands
    uint16_t offsetExternalCommands;
    uint16_t offsetFirmwareVersions;
    uint16_t offsetAudioFormats;
    uint16_t offsetInputCommands;
    uint16_t offsetOutputCommands;
    uint16_t offsetClassNames;
    uint16_t offsetInterfaceGuids;
    uint16_t offsetHidDescriptor;
} __attribute__((packed)) GipDescriptorHeader_t;

// Shared by guitars and drums
#define GIP_BUTTON_MENU 0x0004
#define GIP_BUTTON_VIEW 0x0008
#define GIP_BUTTON_A 0x0010
#define GIP_BUTTON_B 0x0020
#define GIP_BUTTON_X 0x0040
#define GIP_BUTTON_Y 0x0080
#define GIP_BUTTON_DPAD_UP 0x0100
#define GIP_BUTTON_DPAD_DOWN 0x0200
#define GIP_BUTTON_DPAD_LEFT 0x0400
#define GIP_BUTTON_DPAD_RIGHT 0x0800

#define GIP_DRUM_KICK1 0x1000
#define GIP_DRUM_KICK2 0x2000

#define GIP_FRET_GREEN 0x01
#define GIP_FRET_RED 0x02
#define GIP_FRET_YELLOW 0x04
#define GIP_FRET_BLUE 0x08
#define GIP_FRET_ORANGE 0x10

typedef struct {
    uint16_t buttons;
    uint8_t tilt;
    uint8_t whammy;
    uint8_t pickup; // notch in the top 4 bits, 0-4
    uint8_t upperFrets;
    uint8_t lowerFrets;
    uint8_t autocalLight;
    uint16_t autocalAudio;
} __attribute__((packed)) GipGuitarInput_t;

typedef struct {
    uint16_t buttons;
    // 4 bit velocities, 0 when not hit
    uint8_t redYellowPad;  // red: top, yellow: bottom
    uint8_t blueGreenPad;  // blue: top, green: bottom
    uint8_t yellowBlueCymbal; // yellow: top, blue: bottom
    uint8_t greenCymbal;   // green: top
} __attribute__((packed)) GipDrumInput_t;

// Interface GUIDs that identify instruments in device descriptors
// {0D2AE438-7F7D-4933-8693-30FC55018E77} MadCatz Stratocaster / CRKD
static const GUID GIP_GUID_GUITAR_MADCATZ = {0x0D2AE438, 0x7F7D, 0x4933, {0x86, 0x93, 0x30, 0xFC, 0x55, 0x01, 0x8E, 0x77}};
// {1A266AF6-3A46-45E3-B9B6-0F2C0B2C1EBE} PDP Jaguar / Riffmaster
static const GUID GIP_GUID_GUITAR_PDP = {0x1A266AF6, 0x3A46, 0x45E3, {0xB9, 0xB6, 0x0F, 0x2C, 0x0B, 0x2C, 0x1E, 0xBE}};
// {06182893-CCE0-4B85-9271-0A10DBAB7E07} MadCatz drums
static const GUID GIP_GUID_DRUMS_MADCATZ = {0x06182893, 0xCCE0, 0x4B85, {0x92, 0x71, 0x0A, 0x10, 0xDB, 0xAB, 0x7E, 0x07}};
// {A503F9B0-955E-47C4-A2ED-B1336FA7703E} PDP drums
static const GUID GIP_GUID_DRUMS_PDP = {0xA503F9B0, 0x955E, 0x47C4, {0xA2, 0xED, 0xB1, 0x33, 0x6F, 0xA7, 0x70, 0x3E}};

_Static_assert(sizeof(GipHeader_t) == 20, "GIP header size");
_Static_assert(sizeof(GipArrival_t) == 20, "GIP arrival size");
_Static_assert(sizeof(GipDescriptorHeader_t) == 36, "GIP descriptor header size");
_Static_assert(sizeof(GipGuitarInput_t) == 10, "GIP guitar report size");
_Static_assert(sizeof(GipDrumInput_t) == 6, "GIP drum report size");
