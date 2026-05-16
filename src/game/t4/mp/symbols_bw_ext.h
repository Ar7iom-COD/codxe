#pragma once
//
// Bot Warfare T4 port — function symbol table.
//
// ============================================================================
// AUDIT WARNING: STOCK codxe T4 symbols.h IS LARGELY INCORRECT
// ============================================================================
// Every Scr_*/Dvar_* address in stock codxe-main src/game/t4/mp/symbols.h was
// verified against TU7 default_mp.xex via Ghidra and found wrong. This header
// declares corrected pointers with a `_BW` suffix where names would collide.
//
// Audited & corrected vs stock codxe T4 symbols.h:
//   Scr_GetInt       0x8234AFD0 -> 0x82341C20
//   Scr_GetFloat     0x8234B250 -> 0x82341EA0
//   Scr_GetString    0x8234B550 -> 0x823421A0
//   Scr_GetVector    0x8234B790 -> 0x823423E0
//   Scr_GetEntity    0x82254018 -> use Scr_GetEntityNum 0x82342770
//   Scr_GetNumParam  0x82345650 -> inlined (read DAT_85bb3fb4)
//   Scr_Error        0x8234BE08 -> 0x8233CAC0
//   Scr_ParamError   0x82345E70 -> 0x8233C9B8
//   Scr_ObjectError  0x82345EF0 -> 0x8233CB40
//   Scr_AddInt       0x82345668 -> 0x8233C4D0
//   Scr_AddUndefined (n/a)      -> 0x8233C458
//   Scr_AddString    (n/a)      -> 0x8233C5F8
//   Scr_AddEntityNum (n/a)      -> 0x8233C2B8
//   va               0x822C38D8 -> 0x822BE508
//   SV_ClientThink   0x82284D50 -> 0x82280F38
//
// ============================================================================
// r317 — bot spawn via real SV_AddTestClient + NET_CompareBaseAdr post-scan fix
// ============================================================================
// r314 deleted Path C and called the real engine SV_AddTestClient() directly
// (the codxe IW3 reference architecture). r316 proved that call hangs on T4
// Xenia. The NET_CompareBaseAdr decompile pinned the cause: SV_AddTestClient
// memsets the bot netadr to all-zero (port 0); the Xenia host netadr is also
// zero; NET_CompareBaseAdr_impl, for type==0 (NA_BOT), skips the IP compare
// and falls through to compare .port -> equal ports -> the post-scan matches
// the host at slot 0 -> hang. r317 detours NET_CompareBaseAdr_impl to report
// "not equal" for NA_BOT-vs-NA_BOT while a bot-add is in progress.
//
// All addresses below are Ghidra-verified against TU7 default_mp.xex:
//   SV_AddTestClient        0x82281F08   (returns gentity_s*, no args)
//   NET_CompareBaseAdr      0x82278BD0   (wrapper)
//   NET_CompareBaseAdr_impl 0x82278B20   (inner comparator — r317 detour target)
//   SV_DirectConnect        0x822815B0
//   FUN_8226CE38 (push)     0x8226CE38
//   FUN_8226CE58 (pop)      0x8226CE58
//   SV_SendClientGameState  0x82280080
//   SV_ClientEnterWorld     0x82280598
//   SV_BotUserMove          0x82286D68   (runtime-confirmed working)
//   SV_UserinfoChanged      0x82280690
//   SV_DropClient           0x8227FDE0
//   SV_IsTestClient         0x8221D1E0
//   Info_ValueForKey        0x822BE640
//   Info_SetValueForKey     0x822BEC10
//   Scr_Notify              0x82251460
//   Scr_AllocString         0x823323F0
//   G_SelectWeaponIndex     0x8225D6D8
// ============================================================================

#include "structs_bw_ext.h"

namespace t4
{
namespace mp
{
namespace bw
{

// ---- Server: client lifecycle -------------------------------------------

typedef gentity_s *(*SV_AddTestClient_t)();
static SV_AddTestClient_t SV_AddTestClient =
    reinterpret_cast<SV_AddTestClient_t>(0x82281F08);

typedef void (*SV_UserinfoChanged_t)(clientBW_t *cl);
static SV_UserinfoChanged_t SV_UserinfoChanged =
    reinterpret_cast<SV_UserinfoChanged_t>(0x82280690);

typedef void (*SV_DropClient_t)(clientBW_t *cl, const char *reason, bool tellThem);
static SV_DropClient_t SV_DropClient =
    reinterpret_cast<SV_DropClient_t>(0x8227FDE0);

// ---- Server: bot driver -------------------------------------------------

typedef void (*SV_BotUserMove_t)(clientBW_t *cl);
static SV_BotUserMove_t SV_BotUserMove =
    reinterpret_cast<SV_BotUserMove_t>(0x82286D68);

// NET_CompareBaseAdr_impl — the inner address comparator.
// 0x82278B20, __stdcall, longlong(uint* a, uint* b, undefined8 x5).
// Verified: the NET_CompareBaseAdr wrapper (0x82278BD0) spills the two
// by-value netadrs to stack and passes POINTERS to this _impl. For type==0
// (NA_BOT) and type==2 (loopback) _impl skips the IP compare and falls
// through to compare the .port field; it returns (portA - portB), where 0
// means "equal". SV_AddTestClient memsets the bot netadr to all-zero (port
// 0); the Xenia host netadr is also zero -> equal ports -> the
// SV_AddTestClient post-scan false-matches the host at slot 0 -> hang.
// r317 detours this to report "not equal" for NA_BOT-vs-NA_BOT while a
// bot-add is in progress.
typedef long long (*NET_CompareBaseAdr_impl_t)(unsigned int *a,
                                               unsigned int *b,
                                               unsigned __int64 p3,
                                               unsigned __int64 p4,
                                               unsigned __int64 p5,
                                               unsigned __int64 p6,
                                               unsigned __int64 p7);
static NET_CompareBaseAdr_impl_t NET_CompareBaseAdr_impl =
    reinterpret_cast<NET_CompareBaseAdr_impl_t>(0x82278B20);

// NOTE: SV_CalcPings (0x822863D8) is a stub thunk on T4 X360 — not detoured.

// ---- Server: misc -------------------------------------------------------

typedef int (*SV_IsTestClient_t)(int clientNum);
static SV_IsTestClient_t SV_IsTestClient =
    reinterpret_cast<SV_IsTestClient_t>(0x8221D1E0);

typedef void (*SV_ClientThink_BW_t)(clientBW_t *cl, usercmd_s *cmd);
static SV_ClientThink_BW_t SV_ClientThink_BW =
    reinterpret_cast<SV_ClientThink_BW_t>(0x82280F38);

// ---- Userinfo parsing ---------------------------------------------------

typedef const char *(*Info_ValueForKey_t)(const char *info, const char *key);
static Info_ValueForKey_t Info_ValueForKey =
    reinterpret_cast<Info_ValueForKey_t>(0x822BE640);

typedef void (*Info_SetValueForKey_t)(char *info, const char *key, const char *value);
static Info_SetValueForKey_t Info_SetValueForKey =
    reinterpret_cast<Info_SetValueForKey_t>(0x822BEC10);

// ---- Misc utilities -----------------------------------------------------

typedef char *(*va_BW_t)(const char *format, ...);
static va_BW_t va_BW = reinterpret_cast<va_BW_t>(0x822BE508);

// ---- Script (GSC) bridge — corrected addresses --------------------------

typedef void (*Scr_Notify_t)(gentity_s *ent, unsigned __int16 stringValue, unsigned int paramcount);
static Scr_Notify_t Scr_Notify =
    reinterpret_cast<Scr_Notify_t>(0x82251460);

typedef int (*Scr_AllocString_t)(const char *s);
static Scr_AllocString_t Scr_AllocString =
    reinterpret_cast<Scr_AllocString_t>(0x823323F0);

typedef void (*Scr_AddInt_BW_t)(int value, scriptInstance_t inst);
static Scr_AddInt_BW_t Scr_AddInt_BW =
    reinterpret_cast<Scr_AddInt_BW_t>(0x8233C4D0);

typedef void (*Scr_AddUndefined_BW_t)(scriptInstance_t inst);
static Scr_AddUndefined_BW_t Scr_AddUndefined_BW =
    reinterpret_cast<Scr_AddUndefined_BW_t>(0x8233C458);

typedef void (*Scr_AddString_t)(const char *str, scriptInstance_t inst);
static Scr_AddString_t Scr_AddString =
    reinterpret_cast<Scr_AddString_t>(0x8233C5F8);

typedef void (*Scr_AddEntityNum_t)(int entnum, scriptInstance_t inst);
static Scr_AddEntityNum_t Scr_AddEntityNum =
    reinterpret_cast<Scr_AddEntityNum_t>(0x8233C2B8);

typedef int (*Scr_GetInt_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetInt_BW_t Scr_GetInt_BW =
    reinterpret_cast<Scr_GetInt_BW_t>(0x82341C20);

typedef float (*Scr_GetFloat_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetFloat_BW_t Scr_GetFloat_BW =
    reinterpret_cast<Scr_GetFloat_BW_t>(0x82341EA0);

typedef const char *(*Scr_GetString_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetString_BW_t Scr_GetString_BW =
    reinterpret_cast<Scr_GetString_BW_t>(0x823421A0);

typedef void (*Scr_GetVector_BW_t)(unsigned int index, float *out, scriptInstance_t inst);
static Scr_GetVector_BW_t Scr_GetVector_BW =
    reinterpret_cast<Scr_GetVector_BW_t>(0x823423E0);

typedef int (*Scr_GetEntityNum_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetEntityNum_t Scr_GetEntityNum =
    reinterpret_cast<Scr_GetEntityNum_t>(0x82342770);

typedef void (*Scr_Error_BW_t)(const char *error, scriptInstance_t inst);
static Scr_Error_BW_t Scr_Error_BW =
    reinterpret_cast<Scr_Error_BW_t>(0x8233CAC0);

typedef void (*Scr_ObjectError_BW_t)(const char *error, scriptInstance_t inst);
static Scr_ObjectError_BW_t Scr_ObjectError_BW =
    reinterpret_cast<Scr_ObjectError_BW_t>(0x8233CB40);

typedef void (*Scr_ParamError_BW_t)(unsigned int index, const char *error, scriptInstance_t inst);
static Scr_ParamError_BW_t Scr_ParamError_BW =
    reinterpret_cast<Scr_ParamError_BW_t>(0x8233C9B8);

// Scr_GetNumParam: INLINED — engine reads it directly at each call site.
static inline unsigned int Scr_GetNumParam_BW(scriptInstance_t inst)
{
    return *reinterpret_cast<volatile unsigned int *>(
        0x85BB3FB4 + static_cast<size_t>(inst) * 0x4320);
}

// ---- Gameplay -----------------------------------------------------------

typedef void (*G_SelectWeaponIndex_t)(int clientNum, int iWeaponIndex);
static G_SelectWeaponIndex_t G_SelectWeaponIndex =
    reinterpret_cast<G_SelectWeaponIndex_t>(0x8225D6D8);

// ---- Engine globals -----------------------------------------------------
//
//   svsHeader  = 0x84F85100   serverStaticHeader_t*
//                  .clients @ +0x0  -> client_t*  (client array base)
//                  .time    @ +0x4  -> int        (server time)
//   g_entities = 0x82BAD1B0   gentity_s*  (flat array, stride 0x330)
//
// client_t : sizeof = 0xB762C ; .name @ 0x21328 ; .gentity @ 0x21324
// gentity_s : sizeof = 0x330

static const unsigned int BW_ADDR_SVSHEADER  = 0x84F85100;  // serverStaticHeader_t*
static const unsigned int BW_ADDR_G_ENTITIES = 0x82BAD1B0;  // gentity_s array base

static const unsigned int BW_CLIENT_STRIDE   = 0xB762C;     // sizeof(client_t)
static const unsigned int BW_CLIENT_NAME_OFF = 0x21328;     // client_t.name
static const unsigned int BW_GENTITY_STRIDE  = 0x330;       // sizeof(gentity_s)

// svs.clients — the client_t array base. svsHeader is the header struct
// address; .clients is its first member (offset 0), itself a client_t*.
static inline clientBW_t *BW_svs_clients()
{
    const unsigned int hdr = BW_ADDR_SVSHEADER;
    return *reinterpret_cast<clientBW_t *const *>(hdr + 0x0);
}

// Server time — serverStaticHeader_t.time @ +0x4.
static inline int BW_svs_time()
{
    return *reinterpret_cast<const int *>(BW_ADDR_SVSHEADER + 0x4);
}

// sv_maxclients — T4 MP MAX_CLIENTS is a verified constant: 18.
static inline int BW_sv_maxclients()
{
    return MAX_CLIENTS_BW;  // = 18
}

// g_entities[entnum] — flat array at a known base, fixed stride.
static inline gentity_s *BW_g_entity(int entnum)
{
    return reinterpret_cast<gentity_s *>(
        BW_ADDR_G_ENTITIES + static_cast<unsigned int>(entnum) * BW_GENTITY_STRIDE);
}

} // namespace bw
} // namespace mp
} // namespace t4
