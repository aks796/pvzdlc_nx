/* hle_misc.c: the small DLLs: winmm (timers), ole32/oleaut32 (COM init,
 * BSTRs), shell32, shfolder, and networking that politely fails (the game's
 * update check and old leaderboards have no server to talk to). */
#include <stdio.h>
#include <string.h>

#include "rt.h"

uint32_t rt_tick_ms(void);

/* ---- winmm ---- */
HLE(timeGetTime) { RET(0, rt_tick_ms()); }
HLE(timeBeginPeriod) { RET(1, 0); }
HLE(timeEndPeriod) { RET(1, 0); }
HLE(PlaySoundA) { RET(3, 1); }
#define MMSYSERR_NODRIVER 6
HLE(mixerOpen) { RET(5, MMSYSERR_NODRIVER); }
HLE(mixerClose) { RET(1, 0); }
HLE(mixerGetDevCapsW) { RET(3, MMSYSERR_NODRIVER); }
HLE(mixerGetLineInfoW) { RET(3, MMSYSERR_NODRIVER); }
HLE(mixerGetLineControlsW) { RET(3, MMSYSERR_NODRIVER); }
HLE(mixerGetControlDetailsW) { RET(3, MMSYSERR_NODRIVER); }
HLE(mixerSetControlDetails) { RET(3, MMSYSERR_NODRIVER); }

/* ---- ole32 / oleaut32 ---- */
HLE(CoInitialize) { RET(1, 0); }
HLE(CoCreateInstance)
{
    if (ARG(4)) g_wr32(ARG(4), 0);
    rt_log("[ole32] CoCreateInstance refused\n");
    RET(5, 0x80040154u); /* REGDB_E_CLASSNOTREG */
}
/* BSTR: length prefix (bytes) + UTF-16 + NUL; the pointer is past the prefix */
static uint32_t bstr_alloc(uint32_t src, uint32_t chars)
{
    uint32_t p = rt_heap_alloc(4 + 2 * chars + 2, 1);
    g_wr32(p, 2 * chars);
    if (src) memcpy(G2H(p + 4), G2H(src), 2 * chars);
    return p + 4;
}
HLE(SysAllocString) { RET(1, ARG(0) ? bstr_alloc(ARG(0), (uint32_t)rt_wlen(ARG(0))) : 0); }
HLE(SysFreeString)
{
    if (ARG(0)) rt_heap_free(ARG(0) - 4);
    RET(1, 0);
}
HLE(VariantInit)
{
    memset(G2H(ARG(0)), 0, 16);
    RET(1, 0);
}
HLE(VariantClear)
{
    memset(G2H(ARG(0)), 0, 16);
    RET(1, 0);
}
HLE(VariantChangeType) { RET(4, 0x80020005u); } /* DISP_E_TYPEMISMATCH */
HLE(GetErrorInfo)
{
    if (ARG(1)) g_wr32(ARG(1), 0);
    RET(2, 1); /* S_FALSE: no error info */
}
HLE(SetErrorInfo) { RET(2, 0); }
HLE(CreateErrorInfo)
{
    if (ARG(0)) g_wr32(ARG(0), 0);
    RET(1, 0x8007000eu);
}

/* ---- shell32 / shfolder ---- */
HLE(ShellExecuteA)
{
    char s[512] = "";
    if (ARG(2)) rt_a2u(ARG(2), s, sizeof s);
    rt_log("[shell32] ShellExecute(%s) ignored\n", s);
    RET(6, 33);
}
static const char *folder_for(uint32_t csidl)
{
    switch (csidl & 0xff) {
    case 0x05: return "C:\\Users\\Player\\Documents";
    case 0x1a: return "C:\\Users\\Player\\AppData\\Roaming";
    case 0x1c: return "C:\\Users\\Player\\AppData\\Local";
    case 0x23: return "C:\\ProgramData";
    default: return "C:\\PvZDLC";
    }
}
HLE(SHGetFolderPathA)
{
    rt_u2a(folder_for(ARG(1)), ARG(4), 260);
    RET(5, 0);
}
HLE(SHGetFolderPathW)
{
    rt_u2w(folder_for(ARG(1)), ARG(4), 260);
    RET(5, 0);
}

/* ---- networking: always offline ---- */
HLE(WSAStartup)
{
    memset(G2H(ARG(1)), 0, 400);
    g_wr16(ARG(1), 0x0202);
    g_wr16(ARG(1) + 2, 0x0202);
    RET(2, 0);
}
HLE(WSACleanup) { RET(0, 0); }
HLE(WSAGetLastError) { RET(0, 10051); } /* WSAENETUNREACH */
HLE(socket) { RET(3, 0xffffffffu); }
HLE(closesocket) { RET(1, 0); }
HLE(connect) { RET(3, 0xffffffffu); }
HLE(gethostbyname) { RET(1, 0); }
HLE(htons) { RET(1, (uint16_t)((ARG(0) >> 8) | (ARG(0) << 8))); }
HLE(inet_addr) { RET(1, 0xffffffffu); }
HLE(ioctlsocket) { RET(3, 0xffffffffu); }
HLE(recv) { RET(4, 0xffffffffu); }
HLE(send) { RET(4, 0xffffffffu); }
HLE(ws_select) { RET(5, 0xffffffffu); }
HLE(__WSAFDIsSet) { RET(2, 0); }
HLE(WinHttpOpen)
{
    rt_set_last_error(12029); /* ERROR_WINHTTP_CANNOT_CONNECT */
    RET(5, 0);
}
HLE(WinHttpCloseHandle) { RET(1, 1); }
HLE(WinHttpConnect) { RET(4, 0); }
HLE(WinHttpOpenRequest) { RET(7, 0); }
HLE(WinHttpSendRequest) { RET(7, 0); }
HLE(WinHttpReceiveResponse) { RET(2, 0); }
HLE(WinHttpQueryDataAvailable) { RET(2, 0); }
HLE(WinHttpReadData) { RET(4, 0); }

/* ---- version.dll: no version resources ---- */
HLE(GetFileVersionInfoSizeA)
{
    if (ARG(1)) g_wr32(ARG(1), 0);
    rt_set_last_error(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    RET(2, 0);
}
HLE(GetFileVersionInfoA) { RET(4, 0); }
HLE(VerQueryValueA) { RET(4, 0); }

const HleEntry hle_misc[] = {
    {"version.dll", "GetFileVersionInfoSizeA", GetFileVersionInfoSizeA},
    {"version.dll", "GetFileVersionInfoA", GetFileVersionInfoA}, {"version.dll", "VerQueryValueA", VerQueryValueA},
    {"winmm.dll", "timeGetTime", timeGetTime}, {"winmm.dll", "timeBeginPeriod", timeBeginPeriod},
    {"winmm.dll", "timeEndPeriod", timeEndPeriod}, {"winmm.dll", "PlaySoundA", PlaySoundA},
    {"winmm.dll", "mixerOpen", mixerOpen}, {"winmm.dll", "mixerClose", mixerClose},
    {"winmm.dll", "mixerGetDevCapsW", mixerGetDevCapsW}, {"winmm.dll", "mixerGetLineInfoW", mixerGetLineInfoW},
    {"winmm.dll", "mixerGetLineControlsW", mixerGetLineControlsW},
    {"winmm.dll", "mixerGetControlDetailsW", mixerGetControlDetailsW},
    {"winmm.dll", "mixerSetControlDetails", mixerSetControlDetails},
    {"ole32.dll", "CoInitialize", CoInitialize}, {"ole32.dll", "CoCreateInstance", CoCreateInstance},
    {"oleaut32.dll", "SysAllocString", SysAllocString}, {"oleaut32.dll", "SysFreeString", SysFreeString},
    {"oleaut32.dll", "VariantInit", VariantInit}, {"oleaut32.dll", "VariantClear", VariantClear},
    {"oleaut32.dll", "VariantChangeType", VariantChangeType}, {"oleaut32.dll", "GetErrorInfo", GetErrorInfo},
    {"oleaut32.dll", "SetErrorInfo", SetErrorInfo}, {"oleaut32.dll", "CreateErrorInfo", CreateErrorInfo},
    {"shell32.dll", "ShellExecuteA", ShellExecuteA}, {"shfolder.dll", "SHGetFolderPathA", SHGetFolderPathA},
    {"shfolder.dll", "SHGetFolderPathW", SHGetFolderPathW}, {"shell32.dll", "SHGetFolderPathA", SHGetFolderPathA},
    {"shell32.dll", "SHGetFolderPathW", SHGetFolderPathW},
    {"ws2_32.dll", "WSAStartup", WSAStartup}, {"ws2_32.dll", "WSACleanup", WSACleanup},
    {"ws2_32.dll", "WSAGetLastError", WSAGetLastError}, {"ws2_32.dll", "socket", socket},
    {"ws2_32.dll", "closesocket", closesocket}, {"ws2_32.dll", "connect", connect},
    {"ws2_32.dll", "gethostbyname", gethostbyname}, {"ws2_32.dll", "htons", htons},
    {"ws2_32.dll", "inet_addr", inet_addr}, {"ws2_32.dll", "ioctlsocket", ioctlsocket}, {"ws2_32.dll", "recv", recv},
    {"ws2_32.dll", "send", send}, {"ws2_32.dll", "select", ws_select}, {"ws2_32.dll", "__WSAFDIsSet", __WSAFDIsSet},
    {"winhttp.dll", "WinHttpOpen", WinHttpOpen}, {"winhttp.dll", "WinHttpCloseHandle", WinHttpCloseHandle},
    {"winhttp.dll", "WinHttpConnect", WinHttpConnect}, {"winhttp.dll", "WinHttpOpenRequest", WinHttpOpenRequest},
    {"winhttp.dll", "WinHttpSendRequest", WinHttpSendRequest},
    {"winhttp.dll", "WinHttpReceiveResponse", WinHttpReceiveResponse},
    {"winhttp.dll", "WinHttpQueryDataAvailable", WinHttpQueryDataAvailable},
    {"winhttp.dll", "WinHttpReadData", WinHttpReadData},
    {NULL, NULL, NULL}};
