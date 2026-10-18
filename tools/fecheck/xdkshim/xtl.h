/* xtl.h stand-in for fecheck builds (VS2022 x86, no XDK).
   Win32 types plus Xbox-only declarations whose shape is public (documented
   API signatures and structure sizes). Never retail code or data. Anything
   whose value we do not know for certain (Direct3D render-state numbers,
   XOnline structures) is deliberately left out: a source that needs it stays
   "unbuildable" rather than being built wrong. */
#pragma once
#ifndef FECHECK_XTL_H
#define FECHECK_XTL_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <float.h>   /* decomp sources use FLT_MAX after only <xtl.h> */
#include "d3d8.h"

#ifdef __cplusplus
extern "C" {
#endif
/* memory */
LPVOID WINAPI XPhysicalAlloc(SIZE_T dwSize, ULONG_PTR ulPhysicalAddress, ULONG_PTR ulAlignment, DWORD flProtect);
void WINAPI XPhysicalFree(LPVOID lpAddress);
void WINAPI XPhysicalProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect);
SIZE_T WINAPI XPhysicalSize(LPVOID lpAddress);
/* system */
DWORD WINAPI XGetLanguage(void);
DWORD WINAPI XGetGameRegion(void);
DWORD WINAPI XGetVideoStandard(void);
DWORD WINAPI XGetVideoFlags(void);
DWORD WINAPI XGetAVPack(void);
#define XC_GAME_REGION_NA             0x00000001
#define XC_GAME_REGION_JAPAN          0x00000002
#define XC_GAME_REGION_RESTOFWORLD    0x00000004
#define XC_GAME_REGION_MANUFACTURING  0x80000000
/* content signatures */
#define XCALCSIG_SIGNATURE_SIZE 20
typedef struct _XCALCSIG_SIGNATURE { BYTE Signature[XCALCSIG_SIGNATURE_SIZE]; } XCALCSIG_SIGNATURE, *PXCALCSIG_SIGNATURE;
HANDLE WINAPI XCalculateSignatureBegin(DWORD dwFlags);
DWORD WINAPI XCalculateSignatureUpdate(HANDLE hCalcSig, const BYTE *pbData, ULONG cbData);
DWORD WINAPI XCalculateSignatureEnd(HANDLE hCalcSig, PXCALCSIG_SIGNATURE pSignature);
/* saved games */
DWORD WINAPI XCreateSaveGame(LPCSTR lpRootPathName, LPCWSTR lpSaveGameName, DWORD dwCreationDisposition, DWORD dwCreateFlags, LPSTR lpPathBuffer, UINT uSize);
DWORD WINAPI XDeleteSaveGame(LPCSTR lpRootPathName, LPCWSTR lpSaveGameName);
/* XNet (public shapes) */
typedef struct { BYTE ab[8]; } XNKID;
typedef struct { BYTE ab[16]; } XNKEY;
typedef struct {
    IN_ADDR ina;
    IN_ADDR inaOnline;
    WORD wPortOnline;
    BYTE abEnet[6];
    BYTE abOnline[20];
} XNADDR;
#pragma pack(push, 4)
typedef struct _XUID { ULONGLONG qwUserID; DWORD dwUserFlags; } XUID;
#pragma pack(pop)
INT WINAPI XNetRandom(BYTE *pb, UINT cb);
INT WINAPI XNetRegisterKey(const XNKID *pxnkid, const XNKEY *pxnkey);
INT WINAPI XNetUnregisterKey(const XNKID *pxnkid);
DWORD WINAPI XNetGetTitleXnAddr(XNADDR *pxna);
DWORD WINAPI XNetGetEthernetLinkStatus(void);
INT WINAPI XNetCleanup(void);

/* ---- additions, from the decomp's usage and the public API shape.
   Values marked "unverified" have no decomp evidence. The XNet flag values
   are the publicly documented ones (the same names and values are in the
   public Xbox 360 XNet documentation). ---- */
#ifndef IPPROTO_VDP
#define IPPROTO_VDP 254                     /* public Xbox VDP protocol number */
#endif
typedef struct { BYTE cfgSizeOfStruct; BYTE cfgFlags; BYTE cfgPrivatePoolSizeInPages; BYTE cfgEnetReceiveQueueLength;
    BYTE cfgIpFragMaxSimultaneous; BYTE cfgIpFragMaxPacketDiv256; BYTE cfgSockMaxSockets;
    BYTE cfgSockDefaultRecvBufsizeInK; BYTE cfgSockDefaultSendBufsizeInK; BYTE cfgKeyRegMax; BYTE cfgSecRegMax;
    BYTE cfgQosDataLimitDiv4; } XNetStartupParams;
typedef struct { IN_ADDR ina; DWORD dwPort; } TSADDR;   /* layout unverified */
typedef struct { BYTE bFlags; BYTE bReserved; WORD cProbesXmit; WORD cProbesRecv; WORD cbData; BYTE *pbData;
    WORD wRttMinInMsecs; WORD wRttMedInMsecs; DWORD dwUpBitsPerSec; DWORD dwDnBitsPerSec; } XNQOSINFO;
typedef struct { UINT cxnqos; UINT cxnqosPending; XNQOSINFO axnqosinfo[1]; } XNQOS;
#define XNET_XNQOSINFO_COMPLETE          0x01
#define XNET_XNQOSINFO_TARGET_CONTACTED  0x02
#define XNET_XNQOSINFO_TARGET_DISABLED   0x04
#define XNET_XNQOSINFO_DATA_RECEIVED     0x08
#define XNET_XNQOSINFO_PARTIAL_COMPLETE  0x10
#define XNET_QOS_LISTEN_ENABLE           0x01
#define XNET_QOS_LISTEN_DISABLE          0x02
#define XNET_QOS_LISTEN_SET_DATA         0x04
#define XNET_QOS_LISTEN_SET_BITSPERSEC   0x08
#define XNET_QOS_LISTEN_RELEASE          0x10
#define XNET_GET_XNADDR_PENDING          0x00
#define XNET_GET_XNADDR_NONE             0x01
#define XNET_GET_XNADDR_ETHERNET         0x02
#define XNET_GET_XNADDR_STATIC           0x04
#define XNET_GET_XNADDR_DHCP             0x08
#define XNET_GET_XNADDR_PPPOE            0x10
#define XNET_GET_XNADDR_GATEWAY          0x20
#define XNET_GET_XNADDR_DNS              0x40
#define XNET_GET_XNADDR_ONLINE           0x80
#define XNET_ETHERNET_LINK_ACTIVE        0x01
#define XNET_ETHERNET_LINK_100MBPS       0x02
#define XNET_ETHERNET_LINK_10MBPS        0x04
#define XNET_ETHERNET_LINK_FULL_DUPLEX   0x08
#define XNET_ETHERNET_LINK_HALF_DUPLEX   0x10
#define XNET_XNKID_MASK                  0xF0
#define XNET_XNKID_SYSTEM_LINK           0x00
#define XNET_XNKID_ONLINE_PEER           0x80
#define XNET_XNKID_ONLINE_SERVER         0xC0
#define XNET_CONNECT_STATUS_IDLE         0x00
#define XNET_CONNECT_STATUS_PENDING      0x01
#define XNET_CONNECT_STATUS_CONNECTED    0x02
#define XNET_CONNECT_STATUS_LOST         0x03
INT WINAPI XNetStartup(const XNetStartupParams *pxnsp);
INT WINAPI XNetCreateKey(XNKID *pxnkid, XNKEY *pxnkey);
INT WINAPI XNetXnAddrToInAddr(const XNADDR *pxna, const XNKID *pxnkid, IN_ADDR *pina);
INT WINAPI XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR *pxna, XNKID *pxnkid);
INT WINAPI XNetTsAddrToInAddr(const TSADDR *ptsa, DWORD dwServiceId, const XNKID *pxnkid, IN_ADDR *pina);
INT WINAPI XNetConnect(const IN_ADDR ina);
DWORD WINAPI XNetGetConnectStatus(const IN_ADDR ina);
INT WINAPI XNetQosListen(const XNKID *pxnkid, const BYTE *pb, UINT cb, DWORD dwBitsPerSec, DWORD dwFlags);
INT WINAPI XNetQosLookup(UINT cxna, const XNADDR *apxna[], const XNKID *apxnkid[], const XNKEY *apxnkey[],
                         UINT cina, const IN_ADDR aina[], const DWORD adwServiceId[], UINT cProbes,
                         DWORD dwBitsPerSec, DWORD dwFlags, WSAEVENT hEvent, XNQOS **ppxnqos);
INT WINAPI XNetQosServiceLookup(DWORD dwFlags, WSAEVENT hEvent, XNQOS **ppxnqos);
INT WINAPI XNetQosRelease(XNQOS *pxnqos);

/* content signatures */
#define XCALCSIG_FLAG_NON_ROAMABLE 0x00000001   /* unverified */

/* memory allocator (XMemAlloc attributes; bitfield layout unverified) */
typedef struct _XALLOC_ATTRIBUTES {
    DWORD dwObjectType : 13;
    DWORD dwHeapTracksAttributes : 1;
    DWORD dwMustSucceed : 1;
    DWORD dwFixedSize : 1;
    DWORD dwAllocatorId : 8;
    DWORD dwAlignment : 4;
    DWORD dwMemoryProtect : 2;
    DWORD dwZeroInitialize : 1;
    DWORD dwStackAlignment : 1;
} XALLOC_ATTRIBUTES, *PXALLOC_ATTRIBUTES;
LPVOID WINAPI XMemAllocDefault(SIZE_T dwSize, DWORD dwAllocAttributes);
void WINAPI XMemFreeDefault(PVOID pAddress, DWORD dwAllocAttributes);
void WINAPI XSaveFloatingPointStateForDpc(void);
void WINAPI XRestoreFloatingPointStateForDpc(void);
DWORD WINAPI XGetDiskClusterSize(LPCSTR lpRootPathName);
BOOL WINAPI XMountSecondaryUtilityDrive(void);
PVOID WINAPI XLoadSection(LPCSTR pSectionName);
BOOL WINAPI XFreeSection(LPCSTR pSectionName);

/* saved games and content */
#define MAX_GAMENAME 128
#define MAX_CONTENT_DISPLAY_NAME 128   /* unverified */
typedef struct _XGAME_FIND_DATA {     /* layout unverified */
    WIN32_FIND_DATAA wfd;
    CHAR szSaveGameDirectory[MAX_PATH];
    WCHAR szSaveGameName[MAX_GAMENAME];
} XGAME_FIND_DATA, *PXGAME_FIND_DATA;
typedef struct _XCONTENT_FIND_DATA {  /* layout unverified */
    WIN32_FIND_DATAA wfd;
    DWORD dwFlags;
    CHAR szContentDirectory[MAX_PATH];
    WCHAR szDisplayName[MAX_CONTENT_DISPLAY_NAME];
} XCONTENT_FIND_DATA, *PXCONTENT_FIND_DATA;
HANDLE WINAPI XFindFirstSaveGame(LPCSTR lpRootPathName, PXGAME_FIND_DATA pFindGameData);
BOOL WINAPI XFindNextSaveGame(HANDLE hFindGame, PXGAME_FIND_DATA pFindGameData);
HANDLE WINAPI XFindFirstContent(LPCSTR lpRootPathName, DWORD dwFlagFilter, PXCONTENT_FIND_DATA pFindContentData);
BOOL WINAPI XFindNextContent(HANDLE hFindContent, PXCONTENT_FIND_DATA pFindContentData);
BOOL WINAPI XFindClose(HANDLE hFind);

/* launching */
#define MAX_LAUNCH_DATA_SIZE 3072      /* public (3 KB launch data) */
#define LDT_TITLE 0                     /* unverified */
#define LDT_FROM_DASHBOARD 2            /* unverified */
#define LDT_FROM_DEBUGGER_CMDLINE 3     /* unverified */
#define LDT_FROM_UPDATE 4               /* unverified */
#define XLD_LAUNCH_DASHBOARD_MAIN_MENU 0                  /* unverified */
#define XLD_LAUNCH_DASHBOARD_ERROR 1                      /* unverified */
#define XLD_LAUNCH_DASHBOARD_MEMORY 2                     /* unverified */
#define XLD_LAUNCH_DASHBOARD_SETTINGS 3                   /* unverified */
#define XLD_LAUNCH_DASHBOARD_MUSIC 4                      /* unverified */
#define XLD_LAUNCH_DASHBOARD_NETWORK_CONFIGURATION 6      /* unverified */
#define XLD_LAUNCH_DASHBOARD_NEW_ACCOUNT_SIGNUP 7         /* unverified */
#define XLD_LAUNCH_DASHBOARD_MESSAGE_SERVER_INFO 8        /* unverified */
#define XLD_LAUNCH_DASHBOARD_POLICY_DOCUMENT 9            /* unverified */
#define XLD_LAUNCH_DASHBOARD_ONLINE_MENU 10               /* unverified */
#define XLD_LAUNCH_DASHBOARD_ACCOUNT_MANAGEMENT 11        /* unverified */
typedef struct _LD_LAUNCH_DASHBOARD {   /* layout unverified */
    DWORD dwReason;
    DWORD dwContext;
    DWORD dwParameter1;
    DWORD dwParameter2;
    BYTE Reserved[MAX_LAUNCH_DATA_SIZE - 16];
} LD_LAUNCH_DASHBOARD, *PLD_LAUNCH_DASHBOARD;
typedef union _LAUNCH_DATA {   /* layout unverified */
    BYTE Data[MAX_LAUNCH_DATA_SIZE];
    LD_LAUNCH_DASHBOARD dashboard;
} LAUNCH_DATA, *PLAUNCH_DATA;
DWORD WINAPI XGetLaunchInfo(PDWORD pdwLaunchDataType, PLAUNCH_DATA pLaunchData);
DWORD WINAPI XLaunchNewImage(LPCSTR lpTitlePath, PLAUNCH_DATA pLaunchData);

/* devices and input (public layouts) */
typedef struct _XDEVICE_PREALLOC_TYPE { struct _XPP_DEVICE_TYPE *DeviceType; DWORD dwPreallocCount; } XDEVICE_PREALLOC_TYPE, *PXDEVICE_PREALLOC_TYPE;
typedef struct _XPP_DEVICE_TYPE { ULONG CurrentConnected; ULONG ChangeConnected; ULONG PreviousConnected; } XPP_DEVICE_TYPE, *PXPP_DEVICE_TYPE;
extern XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
extern XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
extern XPP_DEVICE_TYPE XDEVICE_TYPE_VOICE_MICROPHONE_TABLE;
extern XPP_DEVICE_TYPE XDEVICE_TYPE_VOICE_HEADPHONE_TABLE;
#define XDEVICE_TYPE_GAMEPAD (&XDEVICE_TYPE_GAMEPAD_TABLE)
#define XDEVICE_TYPE_MEMORY_UNIT (&XDEVICE_TYPE_MEMORY_UNIT_TABLE)
#define XDEVICE_TYPE_VOICE_MICROPHONE (&XDEVICE_TYPE_VOICE_MICROPHONE_TABLE)
#define XDEVICE_TYPE_VOICE_HEADPHONE (&XDEVICE_TYPE_VOICE_HEADPHONE_TABLE)
#define XDEVICE_NO_SLOT 0
#define XDEVICE_TOP_SLOT 0
#define XDEVICE_BOTTOM_SLOT 1
#define XINPUT_DEVSUBTYPE_GC_GAMEPAD 0x01        /* unverified */
#define XINPUT_DEVSUBTYPE_GC_GAMEPAD_ALT 0x02    /* unverified */
typedef struct _XINPUT_GAMEPAD { WORD wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX; SHORT sThumbLY; SHORT sThumbRX; SHORT sThumbRY; } XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;
typedef struct _XINPUT_STATE { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE, *PXINPUT_STATE;
#pragma pack(push, 1)
typedef struct _XINPUT_RUMBLE { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; } XINPUT_RUMBLE, *PXINPUT_RUMBLE;
typedef struct _XINPUT_FEEDBACK_HEADER { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58]; } XINPUT_FEEDBACK_HEADER, *PXINPUT_FEEDBACK_HEADER;
typedef struct _XINPUT_FEEDBACK { XINPUT_FEEDBACK_HEADER Header; union { XINPUT_RUMBLE Rumble; }; } XINPUT_FEEDBACK, *PXINPUT_FEEDBACK;
typedef struct _XINPUT_CAPABILITIES { BYTE SubType; WORD Reserved; union { XINPUT_GAMEPAD Gamepad; } In; union { XINPUT_RUMBLE Rumble; } Out; } XINPUT_CAPABILITIES, *PXINPUT_CAPABILITIES;
#pragma pack(pop)
typedef struct _XINPUT_POLLING_PARAMETERS { BYTE fAutoPoll : 1; BYTE fInterruptOut : 1; BYTE ReservedMBZ1 : 6; BYTE bInputInterval; BYTE bOutputInterval; BYTE ReservedMBZ2; } XINPUT_POLLING_PARAMETERS, *PXINPUT_POLLING_PARAMETERS;
void WINAPI XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE PreallocTypes);
DWORD WINAPI XGetDevices(PXPP_DEVICE_TYPE DeviceType);
BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE DeviceType, PDWORD pdwInsertions, PDWORD pdwRemovals);
HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE XppDeviceType, DWORD dwPort, DWORD dwSlot, PXINPUT_POLLING_PARAMETERS pPollingParameters);
void WINAPI XInputClose(HANDLE hDevice);
DWORD WINAPI XInputGetCapabilities(HANDLE hDevice, PXINPUT_CAPABILITIES pCapabilities);
DWORD WINAPI XInputGetState(HANDLE hDevice, PXINPUT_STATE pState);
DWORD WINAPI XInputSetState(HANDLE hDevice, PXINPUT_FEEDBACK pFeedback);
#ifdef __cplusplus
}
#endif
#include "dsound.h"
#endif
