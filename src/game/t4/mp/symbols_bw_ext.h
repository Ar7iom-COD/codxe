#pragma once
//
// Bot Warfare T4 port — function symbol table (TU7).
//
// ============================================================================
// TU7 ADDRESS TABLE — corrected r335
// ============================================================================
// The running binary is World at War default_mp.xex **TU7 / 0.0.7.8**: Xenia
// loads the disc image (base 0.0.0.8) and applies default_mp.xexp to produce
// TU7 in memory. Every address below targets the TU7 image.
//
// History of the bug this header fixes:
//   The previous symbols_bw_ext.h was built from **base 0.0.0.8** Ghidra
//   addresses. Its "audit" compared codxe-core's symbols.h against the base
//   image, saw mismatches, and concluded codxe was wrong. That was backwards:
//   stock codxe symbols.h is correct *for TU7*, and BW was the one calling
//   base addresses on a TU7 binary. That mismatch was the bot-spawn freeze.
//
// How each address below was obtained:
//   [codxe]  taken verbatim from stock codxe src/game/t4/mp/symbols.h, which
//            is the trusted TU7 reference (codxe-core runs correctly on TU7).
//   [sig]    signature-matched: the base-image function prologue/body was
//            masked for branch displacements and D-form data immediates
//            (lis/addi/ori — TU7 relocates these) and matched uniquely
//            against a raw TU7 .text dump (BW_DumpRegions, r334 log).
//
// base->TU7 function order is NOT preserved and the delta is not uniform;
// every [sig] address was matched on its own full body, never by position.
//
//   Symbol                  TU7 address   source
//   ----------------------  -----------   ----------------------------------
//   SV_AddTestClient        0x82285D20    [sig]
//   SV_BotUserMove          0x8228AB98    [sig]
//   SV_UserinfoChanged      0x822844A0    [sig]
//   SV_DropClient           0x82283BF0    [sig]
//   SV_ClientThink          0x82284D50    [codxe]
//   SV_IsTestClient         0x8221F6D0    [sig]  (base 0x8221D1E0)
//   NET_CompareBaseAdr_impl 0x8227C940    [sig]
//   Info_ValueForKey        0x822C3A10    [sig]
//   Info_SetValueForKey     0x822C3FE0    [sig]
//   Scr_Notify              0x82254180    [sig]
//   Scr_AllocString         0x8233B7A0    [sig]
//   Scr_AddInt              0x82345668    [codxe] (= integer-push primitive)
//   Scr_AddEntityNum        0x82345668    [sig]   (entnum is a tag-6 int;
//                                                  same primitive as AddInt)
//   Scr_AddString           0x82345A40    [sig]   (base 0x8233C5F8)
//   Scr_AddUndefined        0x82345808    [sig]   (base 0x8233C458)
//   Scr_GetInt              0x8234AFD0    [codxe]
//   Scr_GetFloat            0x8234B250    [codxe]
//   Scr_GetString           0x8234B550    [codxe]
//   Scr_GetVector           0x8234B790    [codxe]
//   Scr_GetEntityNum        0x8234BB20    [sig]   (base 0x82342770)
//   Scr_Error               0x8234BE08    [codxe]
//   Scr_ObjectError         0x82345EF0    [codxe]
//   Scr_ParamError          0x82345E70    [codxe]
//   Scr_GetNumParam         0x82345650    [codxe] (real function on TU7)
//   va                      0x822C38D8    [codxe]
//   G_SelectWeaponIndex     0x82260C88    [sig]
//
// NOTE on Scr_AddInt / Scr_AddEntityNum sharing 0x82345668: on the T4 script
// VM an entity number is pushed as a plain VAR_INTEGER (type tag 6). stock
// codxe names 0x82345668 "Scr_AddInt"; the base-image Ghidra DB names its
// copy "Scr_AddEntityNum". A full-body signature match confirmed they are
// the same integer-push primitive. Both names are kept below, both resolve
// to 0x82345668 — this is intentional, not a copy-paste error.
//
// NOTE on Scr_GetNumParam: stock codxe exposes it as a real function at
// 0x82345650. The previous header claimed it was inlined and read a DAT_
// global directly — that was a base-image artifact. Use the function.
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
    reinterpret_cast<SV_AddTestClient_t>(0x82285D20);

typedef void (*SV_UserinfoChanged_t)(clientBW_t *cl);
static SV_UserinfoChanged_t SV_UserinfoChanged =
    reinterpret_cast<SV_UserinfoChanged_t>(0x822844A0);

typedef void (*SV_DropClient_t)(clientBW_t *cl, const char *reason, bool tellThem);
static SV_DropClient_t SV_DropClient =
    reinterpret_cast<SV_DropClient_t>(0x82283BF0);

// Real TU7 SV_ClientThink. Suffixed `_BW` to avoid a linker collision with
// the stock codxe declaration of the same name (same address — kept for
// call-site clarity in the BW port).
typedef void (*SV_ClientThink_BW_t)(clientBW_t *cl, usercmd_s *cmd);
static SV_ClientThink_BW_t SV_ClientThink_BW =
    reinterpret_cast<SV_ClientThink_BW_t>(0x82284D50);

// ---- Server: bot driver -------------------------------------------------

typedef void (*SV_BotUserMove_t)(clientBW_t *cl);
static SV_BotUserMove_t SV_BotUserMove =
    reinterpret_cast<SV_BotUserMove_t>(0x8228AB98);

// NOTE: SV_CalcPings is intentionally NOT exposed — see sv_bots.cpp header.
// The 1-bar bot scoreboard indicator is XLive QoS driven and cannot be
// changed from server code.

// ---- Server: misc -------------------------------------------------------

// SV_IsTestClient(int clientNum): leaf function. Indexes the gclient_s
// array (stride 0x3C6C) and returns the isTestClient int at gclient + 0x39BC.
typedef int (*SV_IsTestClient_t)(int clientNum);
static SV_IsTestClient_t SV_IsTestClient =
    reinterpret_cast<SV_IsTestClient_t>(0x8221F6D0);

// ---- Networking ---------------------------------------------------------

typedef int (*NET_CompareBaseAdr_impl_t)(const netadr_t *a, const netadr_t *b);
static NET_CompareBaseAdr_impl_t NET_CompareBaseAdr_impl =
    reinterpret_cast<NET_CompareBaseAdr_impl_t>(0x8227C940);

// ---- Userinfo parsing ---------------------------------------------------

typedef const char *(*Info_ValueForKey_t)(const char *info, const char *key);
static Info_ValueForKey_t Info_ValueForKey =
    reinterpret_cast<Info_ValueForKey_t>(0x822C3A10);

typedef void (*Info_SetValueForKey_t)(char *info, const char *key, const char *value);
static Info_SetValueForKey_t Info_SetValueForKey =
    reinterpret_cast<Info_SetValueForKey_t>(0x822C3FE0);

// ---- Misc utilities -----------------------------------------------------

typedef char *(*va_BW_t)(const char *format, ...);
static va_BW_t va_BW = reinterpret_cast<va_BW_t>(0x822C38D8);

// ---- Script (GSC) bridge ------------------------------------------------
//
// Pattern conventions (verified by decompile):
//   Scr_Get*:   (unsigned int index, scriptInstance_t inst)
//   Scr_Add*:   (value, scriptInstance_t inst)  except Scr_AddUndefined(inst)
//   Scr_*Error: (param-dependent, scriptInstance_t inst)
//
// PowerPC ABI: r3=arg1, r4=arg2, ...
// SCRIPTINSTANCE_SERVER == 0 for all BW paths.

typedef void (*Scr_Notify_t)(gentity_s *ent, unsigned __int16 stringValue, unsigned int paramcount);
static Scr_Notify_t Scr_Notify =
    reinterpret_cast<Scr_Notify_t>(0x82254180);

typedef int (*Scr_AllocString_t)(const char *s);
static Scr_AllocString_t Scr_AllocString =
    reinterpret_cast<Scr_AllocString_t>(0x8233B7A0);

// --- Scr_Add* (push onto active GSC stack) ---

// Integer-push primitive. Pushes a VAR_INTEGER (type tag 6).
typedef void (*Scr_AddInt_BW_t)(int value, scriptInstance_t inst);
static Scr_AddInt_BW_t Scr_AddInt_BW =
    reinterpret_cast<Scr_AddInt_BW_t>(0x82345668);

// Entity-number push. On T4 an entnum is a plain VAR_INTEGER, so this is the
// same primitive as Scr_AddInt — same address by design (see header note).
typedef void (*Scr_AddEntityNum_t)(int entnum, scriptInstance_t inst);
static Scr_AddEntityNum_t Scr_AddEntityNum =
    reinterpret_cast<Scr_AddEntityNum_t>(0x82345668);

typedef void (*Scr_AddString_t)(const char *str, scriptInstance_t inst);
static Scr_AddString_t Scr_AddString =
    reinterpret_cast<Scr_AddString_t>(0x82345A40);

typedef void (*Scr_AddUndefined_BW_t)(scriptInstance_t inst);
static Scr_AddUndefined_BW_t Scr_AddUndefined_BW =
    reinterpret_cast<Scr_AddUndefined_BW_t>(0x82345808);

// Float-push primitive. Pushes a VAR_FLOAT (type tag 5).
//   Discovered via Ghidra signature match against tu7_default_mp.xex:
//   identical body shape to Scr_AddInt (0x82345668) — fmr f31,f1 saves the
//   float arg, then `li r9, 0x5` tags VAR_FLOAT, then `stfs f31, 0x0(r5)`
//   writes 4 bytes. Sibling slot in the Scr_Add* family.
typedef void (*Scr_AddFloat_BW_t)(float value, scriptInstance_t inst);
static Scr_AddFloat_BW_t Scr_AddFloat_BW =
    reinterpret_cast<Scr_AddFloat_BW_t>(0x823456F0);

// Vector-push primitive. Pushes a VAR_VECTOR (type tag 4). Caller passes a
// pointer to 3 floats; the engine interns/refcounts the vector and stores
// the resulting handle in the slot.
//   Discovered via Ghidra signature match against tu7_default_mp.xex: body
//   at 0x82345B70 uses `li r9, 0x4` (VAR_VECTOR), passes the vector pointer
//   to helper 0x8233CD18 (vector intern/dedupe), then stores returned
//   handle. The entry-point (mfspr/bl __savegprlr prologue) is at
//   0x82345B68 — calling the body directly at 0x82345B70 corrupts r30/r31
//   because __savegprlr never ran to save them, and the matching
//   __restgprlr at exit pops garbage.
typedef void (*Scr_AddVector_BW_t)(const float *vec, scriptInstance_t inst);
static Scr_AddVector_BW_t Scr_AddVector_BW =
    reinterpret_cast<Scr_AddVector_BW_t>(0x82345B68);

// Array primitives. Scr_MakeArray opens a fresh empty array on the GSC
// stack; subsequent Scr_AddArray takes the last pushed value and appends it
// as the next array element.
typedef void (*Scr_MakeArray_BW_t)(scriptInstance_t inst);
static Scr_MakeArray_BW_t Scr_MakeArray_BW =
    reinterpret_cast<Scr_MakeArray_BW_t>(0x82345BF8);

typedef void (*Scr_AddArray_BW_t)(scriptInstance_t inst);
static Scr_AddArray_BW_t Scr_AddArray_BW =
    reinterpret_cast<Scr_AddArray_BW_t>(0x82345C80);

// --- Scr_Get* (read from GSC stack at index) ---

typedef int (*Scr_GetInt_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetInt_BW_t Scr_GetInt_BW =
    reinterpret_cast<Scr_GetInt_BW_t>(0x8234AFD0);

typedef float (*Scr_GetFloat_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetFloat_BW_t Scr_GetFloat_BW =
    reinterpret_cast<Scr_GetFloat_BW_t>(0x8234B250);

typedef const char *(*Scr_GetString_BW_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetString_BW_t Scr_GetString_BW =
    reinterpret_cast<Scr_GetString_BW_t>(0x8234B550);

typedef void (*Scr_GetVector_BW_t)(unsigned int index, float *out, scriptInstance_t inst);
static Scr_GetVector_BW_t Scr_GetVector_BW =
    reinterpret_cast<Scr_GetVector_BW_t>(0x8234B790);

// Returns the entity number directly (T4 has no Scr_GetEntity returning a
// gentity_s* like IW3 codxe). Resolve via &g_entities[entnum].
typedef int (*Scr_GetEntityNum_t)(unsigned int index, scriptInstance_t inst);
static Scr_GetEntityNum_t Scr_GetEntityNum =
    reinterpret_cast<Scr_GetEntityNum_t>(0x8234BB20);

// --- Scr_*Error (longjmp out — these don't return) ---

typedef void (*Scr_Error_BW_t)(const char *error, scriptInstance_t inst);
static Scr_Error_BW_t Scr_Error_BW =
    reinterpret_cast<Scr_Error_BW_t>(0x8234BE08);

typedef void (*Scr_ObjectError_BW_t)(const char *error, scriptInstance_t inst);
static Scr_ObjectError_BW_t Scr_ObjectError_BW =
    reinterpret_cast<Scr_ObjectError_BW_t>(0x82345EF0);

typedef void (*Scr_ParamError_BW_t)(unsigned int index, const char *error, scriptInstance_t inst);
static Scr_ParamError_BW_t Scr_ParamError_BW =
    reinterpret_cast<Scr_ParamError_BW_t>(0x82345E70);

// --- Scr_GetNumParam: real function on TU7 (0x82345650) ---

typedef unsigned int (*Scr_GetNumParam_BW_fn_t)(scriptInstance_t inst);
static Scr_GetNumParam_BW_fn_t Scr_GetNumParam_BW_fn =
    reinterpret_cast<Scr_GetNumParam_BW_fn_t>(0x82345650);

static inline unsigned int Scr_GetNumParam_BW(scriptInstance_t inst)
{
    return Scr_GetNumParam_BW_fn(inst);
}

// ---- Gameplay -----------------------------------------------------------

typedef void (*G_SelectWeaponIndex_t)(int clientNum, int iWeaponIndex);
static G_SelectWeaponIndex_t G_SelectWeaponIndex =
    reinterpret_cast<G_SelectWeaponIndex_t>(0x82260C88);

} // namespace bw
} // namespace mp
} // namespace t4
