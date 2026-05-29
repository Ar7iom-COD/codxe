#pragma once
//
// Bot Warfare T4 port — structural extensions.
//
// This header layers on top of the existing codxe T4 structs.h with the types
// BW needs but that are missing from the stock plugin: netchan_t, netadr_t,
// the extended clientHeader_t with embedded netchan, a fully-spelled-out
// client_t (as clientBW_t to avoid colliding with the stock declaration),
// clientSnapshot_t for the frame ring, and the surrounding enums.
//
// All offsets verified against TU7 default_mp.xex via Ghidra MCP.
//
// ===========================================================================
// LAYERING RULE — do not violate.
// ===========================================================================
// This file is the BOTTOM of the BW include stack. It includes ONLY structs.h.
// The dependency direction is strictly one-way:
//
//     sv_bots.cpp -> sv_bots.h -> symbols_bw_ext.h -> structs_bw_ext.h -> structs.h
//
// structs_bw_ext.h must NEVER #include sv_bots.h, symbols_bw_ext.h, pch.h, or
// any component header. Doing so creates a circular include
// (sv_bots.h -> symbols_bw_ext.h -> structs_bw_ext.h -> sv_bots.h) which fails
// to compile under pch.cpp with C1083 'sv_bots.h' not found, because at that
// point the only include dir on the compiler line is .../src and sv_bots.h
// lives under .../src/game/t4/mp/components/. A previous revision pasted the
// whole sv_bots.cpp body into this file — that is what caused the r314 build
// failure. Keep this file struct-definitions ONLY.
// ===========================================================================
//
// NOTE on the stock codxe T4 `client_t`: its static_assert claims
//   offsetof(client_t, gentity) == 0x213F4
// which is INCORRECT. Verified via SV_DirectConnect decompile, the gentity
// pointer is stored at puVar3[0x84c9] == +0x21324, i.e. immediately after
// SV_DirectConnect computes &g_entities[clientNum] and assigns it. The
// +0x213F4 slot is actually the first of three sv.time-tracking ints set
// by SV_DirectConnect:
//   +0x213F4 = lastPacketTime
//   +0x213F8 = lastConnectTime
//   +0x213FC = nextSnapshotTime
// We do NOT modify the existing stock definition (its static_asserts would
// break the build); we define a parallel byte-compatible view.
//
// ===========================================================================
// r319 — STRUCT FIX (netchan layout + svs.clients base) — 2026-05-30
// ===========================================================================
// Two structural bugs were identified via Ghidra after r317 reached a stable
// match-completion baseline:
//
// BUG 1: netchan_t layout (this file)
//   The previous netchan_t had 0x1C bytes of padding between incomingSequence
//   (+0x0C) and remoteAddress (+0x2C). That made remoteAddress.type land at
//   cl + 0x3C. Verified WRONG via:
//     a) FUN_8228AD80 (SV_BotFrame) decompile: `piVar3[8] == 0` is the bot
//        check, where piVar3 is the cl pointer as int*. piVar3[8] is cl + 0x20.
//        So the engine reads netchan.remoteAddress.type at cl + 0x20.
//     b) SV_AddTestClient_Real (FUN_82285D28) calls NetCompareBaseAdr with
//        `*(undefined8 *)(piVar6 + 8)` and `piVar6[10]` — passing the
//        netadr_t at cl + 0x20 to the comparator.
//   The 0x1C padding came from an IW3 client_t layout where netchan really
//   does have intermediate fields. On T4 the netchan is denser: remoteAddress
//   sits at netchan + 0x10 (cl + 0x20).
//
//   FIX: remove the 0x1C padding. remoteAddress lands at netchan + 0x10.
//   This also re-anchors all the static_asserts for clientBW_t fields after
//   the netchan; everything downstream was already correct because struct
//   padding past userinfo (+0x06DC) is fixed by explicit _pad arrays.
//
// BUG 2: svs.clients base address (handled in sv_bots.cpp, not here)
//   Previously BW_GetClient computed clientBW_t* as
//     &svsHeader->clients[clientNum]
//   where svsHeader was 0x84F85100 (from stock codxe symbols.h). Ghidra
//   shows the engine's own functions (SV_BotFrame, SV_AddTestClient_Real,
//   SV_GetUsercmd) all use DAT_830c0c90 as the svs.clients base. These two
//   bases are NOT related; 0x84F85100 was always wrong for svs.clients.
//   sv_bots.cpp now uses an explicit constant kSvsClientsBase = 0x830C0C90.
// ===========================================================================

#include "structs.h"

namespace t4
{
namespace mp
{
namespace bw
{

// ---- Sanity --------------------------------------------------------------

static const size_t kClientTSize     = 0xB762C;
static const int    MAX_CLIENTS_BW   = 18;
static const int    PACKET_BACKUP_BW = 32;
static const int    T4_PROTOCOL      = 0x5C;

// Real svs.clients base address on TU7 default_mp.xex. Used via BW_GetClient
// in sv_bots.cpp.
//
// CRITICAL: 0x830C0C90 is NOT the svs.clients base itself — it is the address
// of a pointer variable that holds the real base. The engine code reads it
// as `*(clientBW_t**)0x830C0C90` to get the actual base pointer. Verified
// via Ghidra decompile of SV_GetUsercmd (FUN_82286458):
//   `(ulonglong)param_1 * 0xb762c + (ulonglong)DAT_830c0c90 + 0x20ef4`
// The `(ulonglong)DAT_830c0c90` cast confirms DAT_830c0c90 is a pointer
// variable; if it were the literal base address Ghidra wouldn't cast it.
// SV_BotFrame's `piVar3 = DAT_830c0c90` is also a pointer-variable read.
//
// sv_bots.cpp resolves the real base via inline pointer-deref at each call
// site so the dereference happens at runtime — the pointer is set by
// sv_initgame before our stubs ever fire. We use a macro instead of an
// inline function because the helper would need to forward-reference the
// clientBW_t type that hasn't been declared yet at this point in the file.
static const unsigned int kSvsClientsBasePtr = 0x830C0C90;

// Resolves to the real svs.clients[0] pointer at runtime. Cast to clientBW_t*
// at call site (clientBW_t is declared further down in this header).
#define BW_SVS_CLIENTS_BASE() (*reinterpret_cast<clientBW_t **>(kSvsClientsBasePtr))

// ---- Enums ---------------------------------------------------------------

enum clientConnectState_t : __int32
{
    CS_FREE          = 0x0,
    CS_ZOMBIE        = 0x1,
    CS_CONNECTED     = 0x2,
    CS_CLIENTLOADING = 0x3,
    CS_ACTIVE        = 0x4,
};

enum netadrtype_t : __int32
{
    NA_BOT       = 0x0,
    NA_BAD       = 0x1,
    NA_LOOPBACK  = 0x2,
    NA_BROADCAST = 0x3,
    NA_IP        = 0x4,
};

enum netsrc_t : __int32
{
    NS_CLIENT = 0x0,
    NS_SERVER = 0x4,
};

// ---- Network address -----------------------------------------------------

struct __declspec(align(4)) netadr_t
{
    netadrtype_t   type;
    unsigned __int8 ip[4];
    unsigned __int16 port;
    unsigned __int16 _pad0;
};

// ---- Netchan -------------------------------------------------------------
// Embedded at clientHeader_t + 0x10 (i.e. client_t + 0x10).
//
// r319 FIX: remoteAddress sits immediately after incomingSequence with NO
// padding between. Verified via Ghidra SV_BotFrame: cl[8] (= cl+0x20) is
// remoteAddress.type. The previous layout's 0x1C-byte padding came from
// an IW3 client_t structure and never matched T4.

struct netchan_t
{
    int       outgoingSequence;  // +0x00 (cl + 0x10)
    netsrc_t  sock;              // +0x04 (cl + 0x14)
    int       dropped;           // +0x08 (cl + 0x18)
    int       incomingSequence;  // +0x0C (cl + 0x1C)
    netadr_t  remoteAddress;     // +0x10 (cl + 0x20)  [FIX: was +0x2C]
    // [opaque trailing buffers + profiling stream — total netchan size
    //  is unknown but doesn't matter for BW; we only touch the fields
    //  above and the rest is absorbed by clientBW_t._pad_after_header]
};

static_assert(offsetof(netchan_t, outgoingSequence) == 0x00, "");
static_assert(offsetof(netchan_t, dropped)          == 0x08, "");
static_assert(offsetof(netchan_t, remoteAddress)    == 0x10, "");

// ---- Extended client header ----------------------------------------------

struct clientHeaderExt_t
{
    clientConnectState_t state;  // +0x00
    int       sendAsActive;      // +0x04
    int       deltaMessage;      // +0x08
    int       rateDelayed;       // +0x0C
    netchan_t netchan;           // +0x10
};

static_assert(offsetof(clientHeaderExt_t, state)        == 0x00, "");
static_assert(offsetof(clientHeaderExt_t, deltaMessage) == 0x08, "");
static_assert(offsetof(clientHeaderExt_t, netchan)      == 0x10, "");

// ---- Client snapshot frame ----------------------------------------------

struct clientSnapshot_t
{
    unsigned char ps_blob[0x39AC]; // opaque
    int num_entities;              // +0x39AC
    int num_clients;               // +0x39B0
    int first_entity;              // +0x39B4
    int first_client;              // +0x39B8
    int messageSent;               // +0x39BC  INERT on T4 X360
    int messageAcked;              // +0x39C0  INERT on T4 X360
    int messageSize;               // +0x39C4  INERT on T4 X360
    int _pad_39C8;                 // +0x39C8
    int serverTime;                // +0x39CC
};

static_assert(sizeof(clientSnapshot_t) == 0x39D0, "frame stride");
static_assert(offsetof(clientSnapshot_t, num_entities) == 0x39AC, "");

// ---- BW view of client_t -------------------------------------------------

struct clientBW_t
{
    clientHeaderExt_t header;       // +0x00000

    char _pad_after_header[0x006DC - sizeof(clientHeaderExt_t)];

    char userinfo[1024];            // +0x006DC

    char _pad_AE0_to_20EDC[0x20EDC - (0x006DC + 1024)];

    int  reliableSequence;          // +0x20EDC
    int  reliableAcknowledge_alt;   // +0x20EE0
    int  _pad_20EE4;                // +0x20EE4
    int  reliableAcknowledge;       // +0x20EE8
    int  gamestateMessageNum;       // +0x20EEC
    int  protocol;                  // +0x20EF0

    usercmd_s lastUsercmd;          // +0x20EF4

    char _pad_20F20_to_21324[0x21324 - (0x20EF4 + sizeof(usercmd_s))];

    gentity_s *gentity;             // +0x21324
    char name[32];                  // +0x21328
    char clanAbbrev[5];             // +0x21348

    char _pad_2134D_to_213F4[0x213F4 - (0x21348 + 5)];

    int  lastPacketTime;            // +0x213F4
    int  lastConnectTime;           // +0x213F8
    int  nextSnapshotTime;          // +0x213FC

    int  _pad_21400;                // +0x21400

    clientSnapshot_t frames[PACKET_BACKUP_BW]; // +0x21404

    char _pad_after_frames[0x94E08 - (0x21404 + sizeof(clientSnapshot_t) * PACKET_BACKUP_BW)];

    int  snapshotMsec;              // +0x94E08
    int  rate;                      // +0x94E0C

    char _pad_94E10_to_B561C[0xB561C - (0x94E0C + 4)];

    int  isTestClient;              // +0xB561C
    int  messageAcknowledge;        // +0xB5620
    int  natType;                   // +0xB5624
    int  ping;                      // +0xB5628

    char _pad_tail[kClientTSize - (0xB5628 + 4)];
};

static_assert(sizeof(clientBW_t)                                       == kClientTSize, "client_t size drift");
static_assert(offsetof(clientBW_t, header.state)                       == 0x00000, "");
static_assert(offsetof(clientBW_t, header.deltaMessage)                == 0x00008, "");
static_assert(offsetof(clientBW_t, header.netchan.outgoingSequence)    == 0x00010, "");
// r319: remoteAddress is now at cl + 0x20 (header.netchan + 0x10)
static_assert(offsetof(clientBW_t, header.netchan.remoteAddress)       == 0x00020, "");
static_assert(offsetof(clientBW_t, userinfo)                           == 0x006DC, "");
static_assert(offsetof(clientBW_t, lastUsercmd)                        == 0x20EF4, "");
static_assert(offsetof(clientBW_t, gentity)                            == 0x21324, "");
static_assert(offsetof(clientBW_t, name)                               == 0x21328, "");
static_assert(offsetof(clientBW_t, lastPacketTime)                     == 0x213F4, "");
static_assert(offsetof(clientBW_t, lastConnectTime)                    == 0x213F8, "");
static_assert(offsetof(clientBW_t, nextSnapshotTime)                   == 0x213FC, "");
static_assert(offsetof(clientBW_t, frames)                             == 0x21404, "");
static_assert(offsetof(clientBW_t, snapshotMsec)                       == 0x94E08, "");
static_assert(offsetof(clientBW_t, isTestClient)                       == 0xB561C, "");
static_assert(offsetof(clientBW_t, ping)                               == 0xB5628, "");

// ---- gclient_s.isTestClient mirror --------------------------------------

static const ptrdiff_t kGClientIsTestClientOffset = 0x39BC;

inline bool IsTestClient_Fast(const gclient_s *clients, int clientNum)
{
    auto base = reinterpret_cast<const char *>(clients);
    return *reinterpret_cast<const int *>(
               base + clientNum * sizeof(gclient_s) + kGClientIsTestClientOffset) != 0;
}

} // namespace bw
} // namespace mp
} // namespace t4
