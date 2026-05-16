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
//   Scr_GetInt       0x8234AFD0 → 0x82341C20
//   Scr_GetFloat     0x8234B250 → 0x82341EA0
//   Scr_GetString    0x8234B550 → 0x823421A0
//   Scr_GetVector    0x8234B790 → 0x823423E0
//   Scr_GetEntity    0x82254018 → use Scr_GetEntityNum 0x82342770
//   Scr_GetNumParam  0x82345650 → inlined (read DAT_85bb3fb4)
//   Scr_Error        0x8234BE08 → 0x8233CAC0
//   Scr_ParamError   0x82345E70 → 0x8233C9B8
//   Scr_ObjectError  0x82345EF0 → 0x8233CB40
//   Scr_AddInt       0x82345668 → 0x8233C4D0
//   Scr_AddUndefined (n/a)      → 0x8233C458
//   Scr_AddString    (n/a)      → 0x8233C5F8
//   Scr_AddEntityNum (n/a)      → 0x8233C2B8
//   va               0x822C38D8 → 0x822BE508
//   SV_ClientThink   0x82284D50 → 0x82280F38
//
// ============================================================================
// r295 — PATH C (verified)
// ============================================================================
// r294 proved vanilla SV_AddTestClient (0x82281F08) hangs on Xenia even with
// all our detours disabled. Confirmed cause: its post-SV_DirectConnect scan
// loop uses NET_CompareBaseAdr to locate the bot's slot; two NA_BOT (zeroed)
// netadrs compare "equal", so the scan matches the host (slot 0) first and
// writes isTestClient / SV_SendClientGameState / SV_ClientEnterWorld onto the
// active human host → engine corruption → freeze.
//
// Path C bypasses vanilla SV_AddTestClient. We replicate its verified body
// (steps 2-9 + 11 below) but REPLACE the broken post-scan (step 10) with our
// own slot-find that explicitly skips slot 0 (host). Connect string carries
// \invited\1 so SV_DirectConnect takes its safe "invited" CS_FREE scan path.
//
// SV_AddTestClient body, decompiler-verified (Ghidra, TU7 default_mp.xex):
//   2. rand()/format → xuid string
//   3. rand()/format → xnaddr string
//   4. build connect string (fmt @ 0x82040A30, no \invited)
//   5. FUN_8226ce38(connectbuf)                          ← netbuf push
//   6. memset(netadr, 0, 12)
//   7. qport = DAT_82f4b9d0++
//   8. SV_DirectConnect(netadr[0:8], netadr[8:12]|qport<<32, 0xC,
//                       xuidPtr, xnaddrPtr, qport, qport+1, maxclients)
//   9. FUN_8226ce58()                                    ← netbuf pop
//  10. post-scan via NET_CompareBaseAdr  ← BUG, we skip this
//  11. cl->isTestClient = 1;
//      SV_SendClientGameState(cl);
//      memset(buf, 0, 0x2c);
//      SV_ClientEnterWorld(cl, buf);
//
// All addresses below are Ghidra-verified against TU7 default_mp.xex:
//   SV_AddTestClient        0x82281F08   (NOT called — kept for reference)
//   SV_DirectConnect        0x822815B0   (8-arg, signature verified)
//   FUN_8226CE38 (push)     0x8226CE38   (verified: netbuf write)
//   FUN_8226CE58 (pop)      0x8226CE58   (verified: netbuf stack restore)
//   SV_SendClientGameState  0x82280080   (verified: param_1 = client_t*)
//   SV_ClientEnterWorld     0x82280598   (verified: param_1=client_t*,
//                                         param_2=ptr to 0x2c-byte buffer)
//   SV_BotUserMove          0x82286D68
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

// ---- Path C: SV_DirectConnect + netbuf push/pop -------------------------
//
// SV_DirectConnect — 8 args, ABI verified from SV_AddTestClient decompile.
// The netadr_t (12 bytes) is passed split: bytes 0-7 in r3, bytes 8-11 in
// the LOW half of r4 (qport occupies the HIGH half of r4). All-zero netadr
// → type field (first 4 bytes) = 0 = NA_BOT.
//
//   r3  = netadr bytes 0..7         (all zero for a bot)
//   r4  = (qport << 32) | netadr bytes 8..11  (bytes 8..11 zero)
//   r5  = 0xC                       (sizeof netadr_t = 12)
//   r6  = xuid string pointer
//   r7  = xnaddr string pointer
//   r8  = qport
//   r9  = qport + 1
//   r10 = sv_maxclients

typedef __int64 (*SV_DirectConnect_BW_t)(unsigned __int64 netadr_lo,
                                          unsigned __int64 netadr_hi_qport,
                                          unsigned __int64 netadr_len,
                                          unsigned __int64 xuid_ptr,
                                          unsigned __int64 xnaddr_ptr,
                                          unsigned __int64 qport,
                                          unsigned __int64 qport_plus1,
                                          unsigned __int64 maxclients);
static SV_DirectConnect_BW_t SV_DirectConnect_BW =
    reinterpret_cast<SV_DirectConnect_BW_t>(0x822815B0);

// Netbuf push: append a NUL-terminated OOB packet buffer into the global
// netbuf parser state. SV_DirectConnect reads userinfo from this state.
typedef void (*Netmsg_Push_t)(char *packet_buf);
static Netmsg_Push_t Netmsg_Push =
    reinterpret_cast<Netmsg_Push_t>(0x8226CE38);

// Netbuf pop: restore the prior netbuf parser state. Must follow every push.
typedef void (*Netmsg_Pop_t)();
static Netmsg_Pop_t Netmsg_Pop =
    reinterpret_cast<Netmsg_Pop_t>(0x8226CE58);

// SV_SendClientGameState — only param_1 (client_t*) is meaningful; the rest
// are leftover register values in the vanilla call. Transitions the client
// CS_CONNECTED → CS_CLIENTLOADING and sends the gamestate snapshot.
typedef void (*SV_SendClientGameState_BW_t)(clientBW_t *cl,
                                            unsigned __int64 a2,
                                            unsigned __int64 a3,
                                            unsigned __int64 a4,
                                            unsigned __int64 a5);
static SV_SendClientGameState_BW_t SV_SendClientGameState_BW =
    reinterpret_cast<SV_SendClientGameState_BW_t>(0x82280080);

// SV_ClientEnterWorld — param_1 = client_t*, param_2 = pointer to a 0x2c
// (44) byte buffer copied into client_t + 0x20ef4. Transitions
// CS_CLIENTLOADING → CS_ACTIVE, links the gentity, calls GSC ClientBegin.
typedef void (*SV_ClientEnterWorld_BW_t)(clientBW_t *cl,
                                         void *cmd44,
                                         unsigned __int64 a3,
                                         unsigned __int64 a4,
                                         unsigned __int64 a5,
                                         unsigned __int64 a6,
                                         unsigned __int64 a7,
                                         unsigned __int64 a8);
static SV_ClientEnterWorld_BW_t SV_ClientEnterWorld_BW =
    reinterpret_cast<SV_ClientEnterWorld_BW_t>(0x82280598);

// Real SV_ClientThink. Stock codxe T4 SV_ClientThink (0x82284D50) is wrong.
typedef void (*SV_ClientThink_BW_t)(clientBW_t *cl, usercmd_s *cmd);
static SV_ClientThink_BW_t SV_ClientThink_BW =
    reinterpret_cast<SV_ClientThink_BW_t>(0x82280F38);

// ---- Server: bot driver -------------------------------------------------

typedef void (*SV_BotUserMove_t)(clientBW_t *cl);
static SV_BotUserMove_t SV_BotUserMove =
    reinterpret_cast<SV_BotUserMove_t>(0x82286D68);

// NOTE: SV_CalcPings (0x822863D8) is a stub thunk on T4 X360 — not detoured.

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

// ---- Engine globals (Path C slot-finding) -------------------------------
//
// Verified from SV_AddTestClient + SV_ClientEnterWorld + SV_SendClientGameState
// decompiles (TU7 default_mp.xex). Path C uses these DIRECTLY rather than
// trusting stock codxe T4's svsHeader, which has been wrong about many
// offsets this session.
//
// Each is the ADDRESS OF a global, not the global's value. The engine code
// does e.g. `piVar18 = DAT_830afc90;` — a load — so 0x830AFC90 holds a
// pointer; the clients array base is *(void**)0x830AFC90.
//
//   DAT_830afc90  → svs.clients      base pointer   (client_t array)
//   DAT_82ff7c08  → sv                struct ptr; sv_maxclients at +0xC
//   DAT_82ff7a08  → g_entities        base pointer  (gentity_s array)
//   DAT_82ff7a0c  → sizeof(gentity_s) stride        (int)
//
// client_t stride = 0xB762C (verified: SV_AddTestClient `piVar18 += 0x2dd8b`
// int-steps = 0x2dd8b * 4 = 0xB762C; and EnterWorld `/ 0xb762c`).

// Raw addresses of the engine globals — kept as named constants so the
// SYSTEM REPORT diagnostic can probe BOTH interpretations (base vs pointer)
// without re-typing magic numbers.
static const unsigned int BW_ADDR_SVS_CLIENTS = 0x830AFC90;  // svs.clients
static const unsigned int BW_ADDR_SV_STRUCT   = 0x82FF7C08;  // sv struct
static const unsigned int BW_ADDR_G_ENTITIES  = 0x82FF7A08;  // g_entities base
static const unsigned int BW_ADDR_G_ENT_SIZE  = 0x82FF7A0C;  // sizeof(gentity_s)

// Verified client_t stride (SV_AddTestClient `piVar18 += 0x2dd8b` → ×4).
static const unsigned int BW_CLIENT_STRIDE = 0xB762C;

// Verified client_t.name offset (SV_SendClientGameState/EnterWorld DPrintf
// use `param_1 + 0x21328`).
static const unsigned int BW_CLIENT_NAME_OFF = 0x21328;

// svs.clients — the client_t array base.
//
// CURRENT INTERPRETATION: 0x830AFC90 HOLDS A POINTER to the array (one
// dereference). This is unverified — the SYSTEM REPORT in sv_bots.cpp dumps
// both this and the no-deref interpretation so a single run settles it.
static inline clientBW_t *BW_svs_clients()
{
    return *reinterpret_cast<clientBW_t *const *>(BW_ADDR_SVS_CLIENTS);
}

// sv_maxclients.
//
// T4 MP MAX_CLIENTS is a verified constant: 18 (confirmed in-game — the
// Private Match lobby shows "1/18 Players"). The previous engine-memory
// read (*(0x82FF7C08) treated as a pointer, then +0xC) returned 0 because
// 0x82FF7C08 is almost certainly the sv struct BASE, not a pointer to it,
// and sv's first field reads 0. Rather than guess the indirection, use the
// constant: Path C's slot loops are bounded by MAX_CLIENTS_BW and check
// per-slot state, so the constant is safe and correct.
static inline int BW_sv_maxclients()
{
    return MAX_CLIENTS_BW;  // = 18
}

// g_entities[entnum] — base + entnum*stride.
//
// CURRENT INTERPRETATION: 0x82FF7A08 / 0x82FF7A0C HOLD the base and stride
// values (one dereference each). Also dumped by the SYSTEM REPORT.
static inline gentity_s *BW_g_entity(int entnum)
{
    const unsigned int base   = *reinterpret_cast<const unsigned int *>(BW_ADDR_G_ENTITIES);
    const unsigned int stride = *reinterpret_cast<const unsigned int *>(BW_ADDR_G_ENT_SIZE);
    return reinterpret_cast<gentity_s *>(base + static_cast<unsigned int>(entnum) * stride);
}

// qport counter the engine itself uses (DAT_82f4b9d0, a ushort). Path C
// keeps its OWN counter instead, to avoid racing the engine, but the
// address is recorded here for reference.
//   DAT_82f4b9d0  → engine qport counter (ushort)

} // namespace bw
} // namespace mp
} // namespace t4
