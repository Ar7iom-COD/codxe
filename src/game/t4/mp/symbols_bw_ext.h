#pragma once
//
// Bot Warfare T4 port — function symbol table.
//
// ============================================================================
// AUDIT WARNING: STOCK codxe T4 symbols.h IS LARGELY INCORRECT
// ============================================================================
// During this port, every Scr_* and Dvar_* address in the stock codxe-main
// `src/game/t4/mp/symbols.h` was verified against TU7 default_mp.xex via
// Ghidra and found to NOT land at function boundaries. The codxe-shipped T4
// symbol table targets a different T4 build than the binary we have.
//
// This header (and the BW C++ port that includes it) avoids the stock
// symbols by declaring its own corrected pointers for every Scr_*, Dvar_*,
// va, and SV_* function it touches. Names use a `_BW` suffix where they
// would otherwise collide with stock declarations.
//
// All addresses below are Ghidra-verified against TU7 default_mp.xex:
//   - Either labeled by name in the Ghidra symbol table, or
//   - Manually confirmed by decompile pattern match (Scr_AddInt /
//     Scr_AddUndefined, where Ghidra labels them only as FUN_xxx).
//
// Audited & corrected vs stock codxe T4 symbols.h:
//   Scr_GetInt              stock 0x8234AFD0  →  real 0x82341C20
//   Scr_GetFloat            stock 0x8234B250  →  real 0x82341EA0
//   Scr_GetString           stock 0x8234B550  →  real 0x823421A0
//   Scr_GetVector           stock 0x8234B790  →  real 0x823423E0
//   Scr_GetEntity           stock 0x82254018  →  use Scr_GetEntityNum 0x82342770
//   Scr_GetNumParam         stock 0x82345650  →  inlined (read DAT_85bb3fb4)
//   Scr_Error               stock 0x8234BE08  →  real 0x8233CAC0
//   Scr_ParamError          stock 0x82345E70  →  real 0x8233C9B8
//   Scr_ObjectError         stock 0x82345EF0  →  real 0x8233CB40
//   Scr_AddInt              stock 0x82345668  →  real 0x8233C4D0
//   Scr_AddUndefined       (not in stock)     →  real 0x8233C458
//   Scr_AddString          (not in stock)     →  real 0x8233C5F8
//   Scr_AddEntityNum       (not in stock)     →  real 0x8233C2B8
//   va                      stock 0x822C38D8  →  real 0x822BE508
//   SV_ClientThink          stock 0x82284D50  →  real 0x82280F38
//
// Hand-hunted (not in stock codxe T4 symbols.h):
//   SV_AddTestClient        0x82281F08
//   SV_BotUserMove          0x82286D68
//   SV_UserinfoChanged      0x82280690
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

// Low-level client drop. Used by GSC kick() to force a bot off the server.
// Verified address from Ghidra session: function string "Going to CS_ZOMBIE
// from %i for %s\n". Signature matches Quake3/CoD pattern.
typedef void (*SV_DropClient_t)(clientBW_t *cl, const char *reason, bool tellThem);
static SV_DropClient_t SV_DropClient =
    reinterpret_cast<SV_DropClient_t>(0x8227FDE0);

// Real SV_ClientThink. Stock codxe T4 SV_ClientThink (0x82284D50) is wrong.
// Suffixed `_BW` to avoid linker collision with the stock declaration.
typedef void (*SV_ClientThink_BW_t)(clientBW_t *cl, usercmd_s *cmd);
static SV_ClientThink_BW_t SV_ClientThink_BW =
    reinterpret_cast<SV_ClientThink_BW_t>(0x82280F38);

// ---- Server: bot driver -------------------------------------------------

typedef void (*SV_BotUserMove_t)(clientBW_t *cl);
static SV_BotUserMove_t SV_BotUserMove =
    reinterpret_cast<SV_BotUserMove_t>(0x82286D68);

// NOTE: SV_CalcPings (0x822863D8) is a stub thunk to an empty function on
// T4 X360 — not called from any per-frame loop. We removed the detour
// from sv_bots.cpp because it would accomplish nothing. The 1-bar bot
// scoreboard indicator is XLive QoS driven and cannot be changed from
// server code. Symbol intentionally NOT exposed.

// ---- Server: misc -------------------------------------------------------

typedef int (*SV_IsTestClient_t)(int clientNum);
static SV_IsTestClient_t SV_IsTestClient =
    reinterpret_cast<SV_IsTestClient_t>(0x8221D1E0);

// ---- Userinfo parsing ---------------------------------------------------

typedef const char *(*Info_ValueForKey_t)(const char *info, const char *key);
static Info_ValueForKey_t Info_ValueForKey =
    reinterpret_cast<Info_ValueForKey_t>(0x822BE640);

typedef void (*Info_SetValueForKey_t)(char *info, const char *key, const char *value);
static Info_SetValueForKey_t Info_SetValueForKey =
    reinterpret_cast<Info_SetValueForKey_t>(0x822BEC10);

// ---- Misc utilities -----------------------------------------------------
// va is heavily used by Scr_*Error paths. Stock codxe T4 va (0x822C38D8)
// is wrong. Real T4 X360 TU7 address verified via Ghidra label.

typedef char *(*va_BW_t)(const char *format, ...);
static va_BW_t va_BW = reinterpret_cast<va_BW_t>(0x822BE508);

// ---- Script (GSC) bridge — corrected addresses --------------------------
//
// Pattern conventions (verified by decompile):
//   Scr_Get*:   (unsigned int index, scriptInstance_t inst)
//   Scr_Add*:   (value, scriptInstance_t inst)  except Scr_AddUndefined(inst)
//   Scr_*Error: (param-dependent, scriptInstance_t inst)
//
// PowerPC ABI: r3=arg1, r4=arg2, ...
// SCRIPTINSTANCE_SERVER == 0 always for our paths.

typedef void (*Scr_Notify_t)(gentity_s *ent, unsigned __int16 stringValue, unsigned int paramcount);
static Scr_Notify_t Scr_Notify =
    reinterpret_cast<Scr_Notify_t>(0x82251460);

typedef int (*Scr_AllocString_t)(const char *s);
static Scr_AllocString_t Scr_AllocString =
    reinterpret_cast<Scr_AllocString_t>(0x823323F0);

// --- Scr_Add* (push onto active GSC stack) ---

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

// --- Scr_Get* (read from GSC stack at index) ---

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

// Returns the entity number directly (signature differs from IW3 codxe's
// Scr_GetEntity which returns a gentity_s*). On T4 use Scr_GetEntityNum
// then resolve via &g_entities[entnum].
typedef int (*Scr_GetEntityNum_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetEntityNum_t Scr_GetEntityNum =
    reinterpret_cast<Scr_GetEntityNum_t>(0x82342770);

// --- Scr_*Error (longjmp out — these don't return) ---

typedef void (*Scr_Error_BW_t)(const char *error, scriptInstance_t inst);
static Scr_Error_BW_t Scr_Error_BW =
    reinterpret_cast<Scr_Error_BW_t>(0x8233CAC0);

typedef void (*Scr_ObjectError_BW_t)(const char *error, scriptInstance_t inst);
static Scr_ObjectError_BW_t Scr_ObjectError_BW =
    reinterpret_cast<Scr_ObjectError_BW_t>(0x8233CB40);

typedef void (*Scr_ParamError_BW_t)(unsigned int index, const char *error, scriptInstance_t inst);
static Scr_ParamError_BW_t Scr_ParamError_BW =
    reinterpret_cast<Scr_ParamError_BW_t>(0x8233C9B8);

// --- Scr_GetNumParam: INLINED ---
// No real function — the engine reads this directly at every call site.
// Param count for the active instance lives at &DAT_85bb3fb4 + inst*0x4320.

static inline unsigned int Scr_GetNumParam_BW(scriptInstance_t inst)
{
    return *reinterpret_cast<volatile unsigned int *>(
        0x85BB3FB4 + static_cast<size_t>(inst) * 0x4320);
}

// ---- Gameplay -----------------------------------------------------------

typedef void (*G_SelectWeaponIndex_t)(int clientNum, int iWeaponIndex);
static G_SelectWeaponIndex_t G_SelectWeaponIndex =
    reinterpret_cast<G_SelectWeaponIndex_t>(0x8225D6D8);

} // namespace bw
} // namespace mp
} // namespace t4
