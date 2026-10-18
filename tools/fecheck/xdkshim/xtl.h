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
#ifdef __cplusplus
}
#endif
#endif
