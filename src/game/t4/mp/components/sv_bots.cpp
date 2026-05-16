//
// Bot Warfare T4 port — engine module.
//
// Ports the codxe IW3 sv_bots.cpp to T4 X360 (TU7). Adds the BW C++ surface
// that the GSC layer (scripts/mp/bots*.gsc + maps/mp/bots/_bot*.gsc) drives:
//
//   GSC functions
//     addtestclient(<name>)        — spawn a bot (returns the bot entity)
//     kick(<clientNum>[,<reason>]) — drop a client
//
//   GSC entity methods (on a bot entity)
//     <bot> botMoveTo(<vec3>)      — drive forward/strafe toward a world point
//     <bot> botAction("+fire")     — set/clear a button bit
//     <bot> botMirror(<player>)    — copy another client's lastUsercmd 1:1
//     <bot> botStop()              — clear all bot input state
//
// ===========================================================================
// r311 — netbuf bisect + re-entrancy guard
// ===========================================================================
//
// r307..r310 crashed identically: pressing ADD BOTS produced 65,525 repeats of
// "[PATH-C] connect string built" / "[PATH-C] FLUSH-1 before Netmsg_Push" and
// then Xenia's "Overflowed stackpoints!". FLUSH-2 never printed even once.
//
// Full decompile of the netbuf chain (FUN_8226ce38 / FUN_8226ce58 /
// Function_8226CCB8) proved:
//   - Netmsg_Push / Netmsg_Pop are a BALANCED push/pop stack over a 0x200-byte
//     arena. Depth counter @0x82E1CE40, running offset @0x82DCCE38.
//   - Function_8226CCB8 increments the depth counter with NO upper bound. It
//     is safe ONLY because the engine always pairs every push with a pop.
//   - Our Path C pushes but the loop never reaches the matching pop, so depth
//     runs away, the 0x200 arena overruns, 0x200-offset underflows, and the
//     resulting wild size corrupts memory -> bad return -> recursion -> crash.
//
// r311 does two things:
//   1. BW_USE_NETMSG bisect toggle (default 0) — gate BOTH Netmsg_Push and
//      Netmsg_Pop together so the pair stays balanced. With 0, the netbuf is
//      skipped entirely; if Path C now reaches FLUSH-3+, the netbuf pair was
//      the crash. A new diagnostic prints the netbuf depth/offset BEFORE the
//      first push so we can see if it was already dirty.
//   2. A re-entrancy guard on BW_AddBotPathC: any future bad call target that
//      recurses into the spawn path is logged once and rejected, instead of
//      driving the guest stack into the ground.
//
// r294 proved, with all our detours disabled, that vanilla SV_AddTestClient
// (0x82281F08) hangs on Xenia by itself. Root cause confirmed by full
// decompile: after SV_DirectConnect places the bot, SV_AddTestClient runs a
// post-scan loop using NET_CompareBaseAdr to re-locate the bot's slot. Two
// NA_BOT (all-zero) netadrs compare "equal", so on Xenia (XLive-offline, host
// netadr also zero-ish) the scan matches the HOST (slot 0) first, then writes
// isTestClient=1 + SV_SendClientGameState + SV_ClientEnterWorld onto the live
// human host → engine corruption → freeze.
//
// Path C bypasses SV_AddTestClient entirely. GScr_AddTestClient below is a
// hand-port of SV_AddTestClient's verified body, with ONE change: the broken
// post-scan (step 10) is replaced by BW_FindNewBotSlot(), which diffs slot
// occupancy before/after SV_DirectConnect and explicitly never returns slot 0.
//
// SV_AddTestClient body — decompiler-verified, TU7 default_mp.xex:
//   1. pre-scan svs.clients for first CS_FREE slot; abort if none
//   2. rand()/format → xuid string         (we use our own LCG + hex format)
//   3. rand()/format → xnaddr string
//   4. build connect string                (we build our own, WITH \invited\1)
//   5. Netmsg_Push(connectbuf)             [0x8226CE38]
//   6. memset(netadr, 0, 12)               (type field = 0 = NA_BOT)
//   7. qport = counter++                   (our own counter, not engine's)
//   8. SV_DirectConnect(netadr0_7, netadr8_11|qport<<32, 0xC,
//                       xuidPtr, xnaddrPtr, qport, qport+1, maxclients)
//                                          [0x822815B0]
//   9. Netmsg_Pop()                        [0x8226CE58]
//  10. >>> SKIP vanilla NET_CompareBaseAdr post-scan — REPLACED <<<
//  11. cl->isTestClient = 1                (write +0xB561C)
//      SV_SendClientGameState(cl)          [0x82280080]
//      memset(buf44, 0, 0x2c)
//      SV_ClientEnterWorld(cl, buf44)      [0x82280598]
//      return &g_entities[slot]
//
// The r292 Path C attempt failed only because BW_BuildConnectPacket returned
// _snprintf's value (a bogus stack pointer on this toolchain) instead of a
// byte count, so the build was treated as failed and Path C never executed.
// r295 eliminates _snprintf entirely — the connect string is assembled by a
// tiny manual appender (BW_StrAppend) that tracks length itself.
//
// Every engine call below is traced directly to the Ghidra decompile. No
// unverified assumptions remain. Heavy FLUSH-N DbgPrint markers bracket each
// engine call so that, if anything still hangs, the last printed marker names
// the exact culprit.
//
// All three AI-driver detours remain OFF in r295 (CODXE_DIAG_* = 0). They are
// re-enabled, with proper client-state guards, only in r296 AFTER Path C is
// confirmed to spawn cleanly.
//

#include "pch.h"
#include "sv_bots.h"

#include <cmath>
#include <cstring>

#pragma warning(disable: 4505)  // unreferenced local fn (hook bodies)

namespace t4
{
namespace mp
{

using namespace t4::mp::bw;

// ===========================================================================
// r311 — netbuf bisect toggle
// ===========================================================================
// 0 = skip BOTH Netmsg_Push and Netmsg_Pop (prove the netbuf pair is the
//     crash; if Path C reaches FLUSH-3+ with this off, it was).
// 1 = use the engine netbuf (original behaviour).
// VS2010: static const, NOT constexpr.
// ===========================================================================
static const int BW_USE_NETMSG = 0;

// r311 — engine netbuf globals (verified from the FUN_8226ce38 decompile:
// Netmsg_Push passes &DAT_82e1ce40 as the depth-counter pointer).
static const unsigned int BW_NETBUF_DEPTH_ADDR  = 0x82E1CE40u;
static const unsigned int BW_NETBUF_OFFSET_ADDR = 0x82DCCE38u;

// ---------------------------------------------------------------------------
// Per-client AI input state
// ---------------------------------------------------------------------------

struct BotMovementInfo_t
{
    int           buttons;
    unsigned char weapon;
    bool          is_mirroring_client;
    int           mirror_client_num;
    float         moveTo[2];
    int           doMove;
};

static BotMovementInfo_t g_botai[MAX_CLIENTS_BW];

static void CleanBotArray()
{
    ZeroMemory(g_botai, sizeof(g_botai));
}

// ---------------------------------------------------------------------------
// Button mapping
// ---------------------------------------------------------------------------

struct BotAction_t
{
    const char *action;
    int         key;
};

static const BotAction_t BotActions[] = {
    {"gostand",    KEY_GOSTAND},
    {"gocrouch",   KEY_CROUCH},
    {"goprone",    KEY_PRONE},
    {"fire",       KEY_FIRE},
    {"melee",      KEY_MELEE},
    {"frag",       KEY_FRAG},
    {"smoke",      KEY_SMOKE},
    {"reload",     KEY_RELOAD},
    {"sprint",     KEY_SPRINT},
    {"leanleft",   KEY_LEANLEFT},
    {"leanright",  KEY_LEANRIGHT},
    {"ads",        KEY_ADSMODE | KEY_ADS},
    {"holdbreath", KEY_HOLDBREATH},
    {"activate",   KEY_USE},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Path C uses the verified engine global directly — see symbols_bw_ext.h.
static inline clientBW_t *BW_GetClient(int clientNum)
{
    return reinterpret_cast<clientBW_t *>(
        reinterpret_cast<char *>(BW_svs_clients()) +
        static_cast<unsigned int>(clientNum) * BW_CLIENT_STRIDE);
}

static gentity_s *BW_RequirePlayerEntity(scr_entref_t entref)
{
    if (entref.classnum != 0)
        Scr_ObjectError_BW("not an entity", SCRIPTINSTANCE_SERVER);

    if (entref.entnum >= MAX_CLIENTS_BW)
        Scr_ObjectError_BW("entity is not a player", SCRIPTINSTANCE_SERVER);

    gentity_s *ent = &g_entities[entref.entnum];
    if (!ent->client)
        Scr_ObjectError_BW(va_BW("entity %i is not a player", entref.entnum),
                           SCRIPTINSTANCE_SERVER);

    return ent;
}

// ===========================================================================
// PATH C — bot spawn
// ===========================================================================

// --- tiny LCG so we never touch the engine's rand state -------------------
static unsigned int g_botRandState = 0x1A2B3C4Du;

static unsigned int BW_BotRand()
{
    // Numerical Recipes LCG constants — good enough for fake xuid/xnaddr.
    g_botRandState = g_botRandState * 1664525u + 1013904223u;
    return g_botRandState;
}

// --- our own qport counter ------------------------------------------------
static unsigned short g_botQport = 0x4000;

// --- manual string appender (NO _snprintf — that was the r292 bug) --------
//
// Appends src to dst at *pos, NUL-terminates, advances *pos. Never overruns
// cap. Returns false if it had to truncate.
static bool BW_StrAppend(char *dst, int cap, int *pos, const char *src)
{
    int p = *pos;
    while (*src && p < cap - 1)
        dst[p++] = *src++;
    dst[p] = '\0';
    *pos = p;
    return (*src == '\0');
}

// Append an 8-digit lowercase hex value.
static bool BW_StrAppendHex8(char *dst, int cap, int *pos, unsigned int v)
{
    static const char hexdig[] = "0123456789abcdef";
    char tmp[9];
    for (int i = 7; i >= 0; --i)
    {
        tmp[i] = hexdig[v & 0xF];
        v >>= 4;
    }
    tmp[8] = '\0';
    return BW_StrAppend(dst, cap, pos, tmp);
}

// Append a decimal int.
static bool BW_StrAppendInt(char *dst, int cap, int *pos, int v)
{
    char tmp[16];
    int  n = 0;
    if (v < 0) { tmp[n++] = '-'; v = -v; }
    char digs[12];
    int  d = 0;
    do { digs[d++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v);
    while (d > 0) tmp[n++] = digs[--d];
    tmp[n] = '\0';
    return BW_StrAppend(dst, cap, pos, tmp);
}

// --- slot-finding ---------------------------------------------------------
//
// Replaces SV_AddTestClient's broken NET_CompareBaseAdr post-scan. We snapshot
// which slots are occupied (state != CS_FREE) BEFORE SV_DirectConnect, then
// after, find the slot that newly became occupied. Slot 0 (host) is never
// returned. CS_FREE == 0 (verified: SV_AddTestClient pre-scan tests `*piVar18
// == 0`).

static bool s_slotOccupiedBefore[MAX_CLIENTS_BW];

static void BW_SnapshotSlots(int maxclients)
{
    for (int i = 0; i < MAX_CLIENTS_BW; ++i)
        s_slotOccupiedBefore[i] = false;

    for (int i = 0; i < maxclients && i < MAX_CLIENTS_BW; ++i)
    {
        clientBW_t *cl = BW_GetClient(i);
        // state is the first int of client_t.
        const int state = *reinterpret_cast<const int *>(cl);
        s_slotOccupiedBefore[i] = (state != 0);
    }
}

// Returns the newly-occupied slot (1..maxclients-1), or -1 if none / only
// slot 0 changed.
static int BW_FindNewBotSlot(int maxclients)
{
    for (int i = 1; i < maxclients && i < MAX_CLIENTS_BW; ++i)  // i starts at 1: skip host
    {
        clientBW_t *cl = BW_GetClient(i);
        const int state = *reinterpret_cast<const int *>(cl);
        if (state != 0 && !s_slotOccupiedBefore[i])
            return i;
    }
    return -1;
}

// Pre-scan for a free slot (mirrors SV_AddTestClient step 1). Returns true if
// at least one slot in 1..maxclients-1 is CS_FREE.
static bool BW_HasFreeSlot(int maxclients)
{
    for (int i = 1; i < maxclients && i < MAX_CLIENTS_BW; ++i)
    {
        clientBW_t *cl = BW_GetClient(i);
        const int state = *reinterpret_cast<const int *>(cl);
        if (state == 0)
            return true;
    }
    return false;
}

// --- SYSTEM REPORT --------------------------------------------------------
//
// imagedump-style diagnostic. Dumps the engine-global state to the Xenia
// log in one shot so each build is self-verifying.
//
// r309: the engine globals are now the codxe-verified T4 addresses
// (svsHeader 0x84F85100, g_entities 0x82BAD1B0). This report confirms they
// are live: with a match running it should show YOUR name at slot 0 and a
// sane server time. If slot 0 still reads empty, the server was not up when
// addtestclient() fired.

static void BW_DumpClientSlot(const char *cl, int slot)
{
    // state = first int of client_t (clientHeader_t.state); name at +0x21328.
    const int   state = *reinterpret_cast<const int *>(cl);
    const char *name  = cl + BW_CLIENT_NAME_OFF;

    // Copy name defensively — it may be garbage / unterminated.
    char namebuf[36];
    int  n = 0;
    for (; n < 32; ++n)
    {
        const char ch = name[n];
        if (ch == '\0') break;
        namebuf[n] = (static_cast<unsigned char>(ch) >= 0x20 &&
                      static_cast<unsigned char>(ch) < 0x7F) ? ch : '?';
    }
    namebuf[n] = '\0';

    DbgPrint("sv_bots:   slot %d: state=%d name=\"%s\"\n", slot, state, namebuf);
}

static void BW_SystemReport()
{
    DbgPrint("sv_bots: ===== BW SYSTEM REPORT (r311) =====\n");

    // --- svsHeader / client array ----------------------------------------
    // svsHeader (0x84F85100) -> serverStaticHeader_t { client_t* clients;
    // int time; }. .clients is the client_t array base.
    const unsigned int hdrAddr   = BW_ADDR_SVSHEADER;
    const unsigned int clientsP  = *reinterpret_cast<const unsigned int *>(hdrAddr + 0x0);
    const int          svTime    = BW_svs_time();

    DbgPrint("sv_bots: svsHeader @0x%08X  .clients=0x%08X  .time=%d\n",
             hdrAddr, clientsP, svTime);

    if (clientsP != 0)
    {
        const char *base = reinterpret_cast<const char *>(clientsP);
        BW_DumpClientSlot(base + 0 * BW_CLIENT_STRIDE, 0);
        BW_DumpClientSlot(base + 1 * BW_CLIENT_STRIDE, 1);
    }
    else
    {
        DbgPrint("sv_bots:   svsHeader.clients is 0 — server not up?\n");
    }

    // --- g_entities ------------------------------------------------------
    // Flat array at a fixed base; print slot 0's entity number as a sanity
    // check (should be 0 for a live g_entities[0]).
    const unsigned int gentBase = BW_ADDR_G_ENTITIES;
    const int gent0Number = *reinterpret_cast<const int *>(gentBase + 0x0);
    DbgPrint("sv_bots: g_entities @0x%08X stride=0x%X  ent[0].s.number=%d\n",
             gentBase, BW_GENTITY_STRIDE, gent0Number);

    DbgPrint("sv_bots: maxclients (constant) = %d\n", MAX_CLIENTS_BW);

    // r311 — netbuf state, so we can see if the push/pop arena is already
    // dirty (depth should be 0 before the first Netmsg_Push).
    DbgPrint("sv_bots: netbuf depth=%d offset=%d  (BW_USE_NETMSG=%d)\n",
             *reinterpret_cast<const int *>(BW_NETBUF_DEPTH_ADDR),
             *reinterpret_cast<const int *>(BW_NETBUF_OFFSET_ADDR),
             BW_USE_NETMSG);

    DbgPrint("sv_bots: ===== END REPORT =====\n");
}

// --- the spawn itself -----------------------------------------------------
//
// Returns the bot's gentity on success, nullptr on failure. Mirrors the
// verified SV_AddTestClient body; see the file header for the step map.
//
// r311: the actual body is BW_AddBotPathC_Impl. BW_AddBotPathC is a thin
// re-entrancy guard wrapper — if anything recurses into the spawn path
// (a bad call target returning into our frame, as r307..r310 did), it is
// logged once and rejected instead of driving the guest stack to overflow.

static gentity_s *BW_AddBotPathC_Impl(const char * /*requestedName*/)
{
    // Full system diagnostic FIRST — prints regardless of what follows.
    BW_SystemReport();

    const int maxclients = BW_sv_maxclients();
    DbgPrint("sv_bots: [PATH-C] begin, maxclients=%d\n", maxclients);

    if (maxclients <= 1)
    {
        DbgPrint("sv_bots: [PATH-C] FAIL: maxclients<=1 (server not up?)\n");
        return nullptr;
    }

    // Step 1 — pre-scan for a free slot.
    if (!BW_HasFreeSlot(maxclients))
    {
        DbgPrint("sv_bots: [PATH-C] FAIL: no free slot\n");
        return nullptr;
    }

    // Snapshot occupancy for the post-DirectConnect diff.
    BW_SnapshotSlots(maxclients);

    // Steps 2-3 — fake xuid + xnaddr hex strings.
    char xuidStr[24];
    {
        int p = 0;
        xuidStr[0] = '\0';
        BW_StrAppendHex8(xuidStr, sizeof(xuidStr), &p, BW_BotRand());
        BW_StrAppendHex8(xuidStr, sizeof(xuidStr), &p, BW_BotRand());
    }

    char xnaddrStr[40];
    {
        int p = 0;
        xnaddrStr[0] = '\0';
        BW_StrAppendHex8(xnaddrStr, sizeof(xnaddrStr), &p, BW_BotRand());
        BW_StrAppendHex8(xnaddrStr, sizeof(xnaddrStr), &p, BW_BotRand());
        BW_StrAppendHex8(xnaddrStr, sizeof(xnaddrStr), &p, BW_BotRand());
        BW_StrAppendHex8(xnaddrStr, sizeof(xnaddrStr), &p, BW_BotRand());
    }

    // Step 4 — build the connect string. WITH \invited\1 so SV_DirectConnect
    // takes its safe "invited" CS_FREE-scan path. Fields mirror the engine's
    // format string (verified at 0x82040A30) plus \invited.
    //
    // The leading "connect " keyword + the userinfo string. SV_DirectConnect
    // parses this out of the netbuf.
    char connectBuf[1024];
    {
        int p = 0;
        connectBuf[0] = '\0';
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "connect \"");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\cg_predictItems\\1");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\cl_anonymous\\0");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\color\\4");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\head\\default");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\model\\multi");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\snaps\\20");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\rate\\5000");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\name\\BWBot");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\protocol\\");
        BW_StrAppendInt(connectBuf, sizeof(connectBuf), &p, 92);   // 0x5c
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\qport\\");
        BW_StrAppendInt(connectBuf, sizeof(connectBuf), &p, static_cast<int>(g_botQport));
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\xuid\\");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, xuidStr);
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\xnaddr\\");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, xnaddrStr);
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\natType\\2");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\\invited\\1");
        BW_StrAppend(connectBuf, sizeof(connectBuf), &p, "\"");
        DbgPrint("sv_bots: [PATH-C] connect string built, len=%d\n", p);
    }

    // Step 5 — push the connect string into the netbuf.
    //
    // r311 BISECT: gated by BW_USE_NETMSG. Netmsg_Push / Netmsg_Pop are a
    // balanced push/pop stack — they MUST be gated together by the SAME flag
    // so the pair stays balanced (skipping only one corrupts the depth
    // counter the other direction).
    DbgPrint("sv_bots: [PATH-C] netbuf depth=%d offset=%d\n",
             *reinterpret_cast<const int *>(BW_NETBUF_DEPTH_ADDR),
             *reinterpret_cast<const int *>(BW_NETBUF_OFFSET_ADDR));
    DbgPrint("sv_bots: [PATH-C] FLUSH-1 before Netmsg_Push\n");
    if (BW_USE_NETMSG)
        Netmsg_Push(connectBuf);
    else
        DbgPrint("sv_bots: [PATH-C] Netmsg_Push SKIPPED (bisect)\n");
    DbgPrint("sv_bots: [PATH-C] FLUSH-2 after Netmsg_Push\n");

    // Step 6-7 — netadr is all-zero (NA_BOT). qport from our own counter.
    const unsigned short qport = g_botQport++;

    // Step 8 — SV_DirectConnect. Exact 8-arg ABI from the verified decompile:
    //   r3  = netadr bytes 0..7   = 0
    //   r4  = netadr bytes 8..11 (0) | qport<<32
    //   r5  = 0xC
    //   r6  = xuid string ptr
    //   r7  = xnaddr string ptr
    //   r8  = qport
    //   r9  = qport+1
    //   r10 = maxclients
    DbgPrint("sv_bots: [PATH-C] FLUSH-3 before SV_DirectConnect (qport=0x%x)\n", qport);
    SV_DirectConnect_BW(
        0ULL,                                                       // r3
        (static_cast<unsigned __int64>(qport) << 32),             // r4
        0xCULL,                                                     // r5
        static_cast<unsigned __int64>(
            reinterpret_cast<unsigned int>(xuidStr)),               // r6
        static_cast<unsigned __int64>(
            reinterpret_cast<unsigned int>(xnaddrStr)),             // r7
        static_cast<unsigned __int64>(qport),                     // r8
        static_cast<unsigned __int64>(qport) + 1,                 // r9
        static_cast<unsigned __int64>(maxclients));               // r10
    DbgPrint("sv_bots: [PATH-C] FLUSH-4 after SV_DirectConnect\n");

    // Step 9 — pop the netbuf (balances the push).
    //
    // r311 BISECT: gated by the SAME BW_USE_NETMSG flag as the push above.
    DbgPrint("sv_bots: [PATH-C] FLUSH-5 before Netmsg_Pop\n");
    if (BW_USE_NETMSG)
        Netmsg_Pop();
    else
        DbgPrint("sv_bots: [PATH-C] Netmsg_Pop SKIPPED (bisect)\n");
    DbgPrint("sv_bots: [PATH-C] FLUSH-6 after Netmsg_Pop\n");

    // Step 10 — REPLACED. Find the bot's slot ourselves, skipping slot 0.
    const int slot = BW_FindNewBotSlot(maxclients);
    if (slot < 0)
    {
        DbgPrint("sv_bots: [PATH-C] FAIL: no new slot after SV_DirectConnect\n");
        return nullptr;
    }
    DbgPrint("sv_bots: [PATH-C] bot landed in slot %d\n", slot);

    clientBW_t *cl = BW_GetClient(slot);

    // Step 11 — finalize: isTestClient, gamestate, enter world.
    cl->isTestClient = 1;

    DbgPrint("sv_bots: [PATH-C] FLUSH-7 before SV_SendClientGameState\n");
    SV_SendClientGameState_BW(cl, 0, 0, 0, 0);
    DbgPrint("sv_bots: [PATH-C] FLUSH-8 after SV_SendClientGameState\n");

    unsigned char buf44[0x2c];
    std::memset(buf44, 0, sizeof(buf44));

    DbgPrint("sv_bots: [PATH-C] FLUSH-9 before SV_ClientEnterWorld\n");
    SV_ClientEnterWorld_BW(cl, buf44, 0, 0, 0, 0, 0, 0);
    DbgPrint("sv_bots: [PATH-C] FLUSH-10 after SV_ClientEnterWorld\n");

    gentity_s *ent = BW_g_entity(slot);
    DbgPrint("sv_bots: [PATH-C] SUCCESS: bot slot %d, entnum=%d\n", slot, ent->s.number);
    return ent;
}

// r311 — re-entrancy guard wrapper. If a bad engine call target ever returns
// into the spawn path again, this rejects the recursive call with a single
// log line instead of letting the guest stack overflow.
static gentity_s *BW_AddBotPathC(const char *requestedName)
{
    static volatile int s_inPathC = 0;

    if (s_inPathC)
    {
        DbgPrint("sv_bots: [PATH-C] RE-ENTRY BLOCKED — spawn path recursed "
                 "(bad call target?)\n");
        return nullptr;
    }

    s_inPathC = 1;
    gentity_s *result = BW_AddBotPathC_Impl(requestedName);
    s_inPathC = 0;
    return result;
}

// ---------------------------------------------------------------------------
// G_SelectWeaponIndex detour — track per-client weapon (driver, OFF in r295)
// ---------------------------------------------------------------------------

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    if (clientNum >= 0 && clientNum < MAX_CLIENTS_BW)
        g_botai[clientNum].weapon = static_cast<unsigned char>(iWeaponIndex);

    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ---------------------------------------------------------------------------
// SV_BotUserMove detour — core bot driver (OFF in r295, re-enabled r296)
// ---------------------------------------------------------------------------

static Detour SV_BotUserMove_Detour;

static void SV_BotUserMove_Stub(clientBW_t *cl)
{
    if (!cl->gentity)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }

    const int clientNum = static_cast<int>(
        (reinterpret_cast<char *>(cl) - reinterpret_cast<char *>(BW_svs_clients()))
        / static_cast<int>(BW_CLIENT_STRIDE));
    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }

    if (cl->header.netchan.remoteAddress.type != NA_BOT)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }
    if (cl->isTestClient == 0)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }

    usercmd_s cmd;
    std::memset(&cmd, 0, sizeof(cmd));

    cmd.serverTime = BW_svs_time();
    cmd.weapon     = g_botai[clientNum].weapon;
    cmd.buttons    = static_cast<button_mask>(g_botai[clientNum].buttons);

    if (g_botai[clientNum].doMove)
    {
        gentity_s *ent = cl->gentity;

        float move_pos[2];
        move_pos[0] = g_botai[clientNum].moveTo[0] - ent->r.currentOrigin[0];
        move_pos[1] = g_botai[clientNum].moveTo[1] - ent->r.currentOrigin[1];

        const float distance = std::sqrt(move_pos[0] * move_pos[0] + move_pos[1] * move_pos[1]);
        g_botai[clientNum].doMove = (distance > 7.0f) ? 1 : 0;

        const float yaw_rad = -ent->r.currentAngles[1] * (3.14159265358979323846f / 180.0f);
        const float s = std::sin(yaw_rad);
        const float c = std::cos(yaw_rad);
        const float rx = move_pos[0] * c - move_pos[1] * s;
        const float ry = move_pos[0] * s + move_pos[1] * c;
        move_pos[0] = rx;
        move_pos[1] = ry;

        const float absX = move_pos[0] < 0.0f ? -move_pos[0] : move_pos[0];
        const float absY = move_pos[1] < 0.0f ? -move_pos[1] : move_pos[1];
        const float maxabs = absX > absY ? absX : absY;
        if (maxabs > 0.0f)
        {
            move_pos[0] = move_pos[0] * (127.0f / maxabs);
            move_pos[1] = move_pos[1] * (127.0f / maxabs);
        }

        move_pos[0] =  std::floor(move_pos[0]);
        move_pos[1] = -std::floor(move_pos[1]);

        cmd.forwardmove = static_cast<char>(static_cast<int>(move_pos[0]) & 0xFF);
        cmd.rightmove   = static_cast<char>(static_cast<int>(move_pos[1]) & 0xFF);

        if (!g_botai[clientNum].doMove)
        {
            static const auto scr_const_movedone = Scr_AllocString("movedone");
            Scr_Notify(ent, static_cast<unsigned __int16>(scr_const_movedone), 0);
        }
    }

    if (g_botai[clientNum].is_mirroring_client)
    {
        const int mirror_num = g_botai[clientNum].mirror_client_num;
        if (mirror_num >= 0 && mirror_num < MAX_CLIENTS_BW)
        {
            const usercmd_s &last = BW_GetClient(mirror_num)->lastUsercmd;
            cmd.buttons     = last.buttons;
            cmd.angles[0]   = last.angles[0];
            cmd.angles[1]   = last.angles[1];
            cmd.forwardmove = last.forwardmove;
            cmd.rightmove   = last.rightmove;
        }
    }

    cl->header.deltaMessage = cl->header.netchan.outgoingSequence - 1;
    SV_ClientThink_BW(cl, &cmd);
}

// ---------------------------------------------------------------------------
// SV_UserinfoChanged detour — custom bot names (OFF in r295)
// ---------------------------------------------------------------------------

static char    s_pendingBotName[32] = {0};
static Detour  SV_UserinfoChanged_Detour;

static void SV_UserinfoChanged_Hook(clientBW_t *cl)
{
    if (s_pendingBotName[0] &&
        cl->header.netchan.remoteAddress.type == NA_BOT &&
        cl->header.state == CS_CONNECTED)
    {
        Info_SetValueForKey(cl->userinfo, "name", s_pendingBotName);
    }

    SV_UserinfoChanged_Detour.GetOriginal<SV_UserinfoChanged_t>()(cl);
}

// ===========================================================================
// r295 DIAGNOSTIC TOGGLES
// ===========================================================================
// All driver detours OFF. Path C is a plain GSC builtin — it needs no
// detours. The detours are for AI input driving and are re-enabled, with
// client-state guards, in r296 once Path C confirms a clean spawn.
// ===========================================================================
#define CODXE_DIAG_ENABLE_WEAPON_HOOK         0
#define CODXE_DIAG_ENABLE_USERINFO_HOOK       0
#define CODXE_DIAG_ENABLE_BOTUSERMOVE         0

// ---------------------------------------------------------------------------
// GSC entity methods
// ---------------------------------------------------------------------------

static void Scr_BotMoveTo(scr_entref_t entref)
{
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: <bot> botMoveTo(<vec3 position>);", SCRIPTINSTANCE_SERVER);

    float moveTo[3] = {0};
    Scr_GetVector_BW(0, moveTo, SCRIPTINSTANCE_SERVER);

    g_botai[entref.entnum].moveTo[0] = moveTo[0];
    g_botai[entref.entnum].moveTo[1] = moveTo[1];
    g_botai[entref.entnum].doMove    = 1;
}

static void Scr_BotAction(scr_entref_t entref)
{
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: <bot> botAction(<action>);", SCRIPTINSTANCE_SERVER);

    const char *action = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    if (!action || (action[0] != '+' && action[0] != '-'))
        Scr_ParamError_BW(0, "Sign for bot action must be '+' or '-'.", SCRIPTINSTANCE_SERVER);

    for (size_t i = 0; i < ARRAYSIZE(BotActions); ++i)
    {
        if (_stricmp(&action[1], BotActions[i].action) == 0)
        {
            if (action[0] == '+')
                g_botai[entref.entnum].buttons |=  BotActions[i].key;
            else
                g_botai[entref.entnum].buttons &= ~BotActions[i].key;
            return;
        }
    }

    char buffer[1024];
    buffer[0] = '\0';
    for (size_t i = 0; i < ARRAYSIZE(BotActions); ++i)
    {
        std::strncat(buffer, " ",                sizeof(buffer) - std::strlen(buffer) - 1);
        std::strncat(buffer, BotActions[i].action, sizeof(buffer) - std::strlen(buffer) - 1);
    }
    Scr_ParamError_BW(0, va_BW("Unknown bot action. Must be one of:%s.", buffer),
                      SCRIPTINSTANCE_SERVER);
}

static void Scr_BotStop(scr_entref_t entref)
{
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <bot> botStop();", SCRIPTINSTANCE_SERVER);

    g_botai[entref.entnum].buttons             = 0;
    g_botai[entref.entnum].is_mirroring_client = false;
    g_botai[entref.entnum].doMove              = 0;
}

static void Scr_BotMirror(scr_entref_t entref)
{
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: <bot> botMirror(<client>);", SCRIPTINSTANCE_SERVER);

    const int targetEntNum = Scr_GetEntityNum(0, SCRIPTINSTANCE_SERVER);

    if (targetEntNum < 0 || targetEntNum >= MAX_CLIENTS_BW)
        Scr_Error_BW("not a player", SCRIPTINSTANCE_SERVER);

    gentity_s *target = &g_entities[targetEntNum];
    if (!target->client)
        Scr_Error_BW("not a player", SCRIPTINSTANCE_SERVER);

    if (entref.entnum == targetEntNum)
        Scr_Error_BW("botMirror: a bot cannot mirror itself.", SCRIPTINSTANCE_SERVER);

    g_botai[entref.entnum].is_mirroring_client = true;
    g_botai[entref.entnum].mirror_client_num   = targetEntNum;
}

// ---------------------------------------------------------------------------
// GSC global function — addtestclient(<name>)  → Path C
// ---------------------------------------------------------------------------

static void GScr_AddTestClient()
{
    char name[32] = "Bot";

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) == 1)
    {
        const char *string = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);

        int i = 0, j = 0;
        if (string)
        {
            for (; string[i] && j < static_cast<int>(sizeof(name)) - 1; ++i)
            {
                if (static_cast<unsigned char>(string[i]) >= 0x20)
                    name[j++] = string[i];
            }
        }
        name[j] = '\0';

        if (j < 1)
            Scr_Error_BW("AddTestClient(): name must be at least 1 character long",
                         SCRIPTINSTANCE_SERVER);
    }

    DbgPrint("sv_bots: [ADDTESTCLIENT] name='%s' — using Path C\n", name);

    gentity_s *ent = BW_AddBotPathC(name);

    if (ent)
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] success, entnum=%d\n", ent->s.number);
        Scr_AddEntityNum(ent->s.number, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] Path C returned NULL\n");
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
    }
}

static void GScr_Kick()
{
    const int nparam = Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER);
    if (nparam < 1 || nparam > 2)
        Scr_Error_BW("Usage: kick(<clientNum>) or kick(<clientNum>, <reason>)",
                     SCRIPTINSTANCE_SERVER);
    const int clientNum = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
        Scr_ParamError_BW(0, va_BW("kick: clientNum %i out of range", clientNum),
                          SCRIPTINSTANCE_SERVER);
    const char *reason = "EXE_PLAYERKICKED";
    if (nparam == 2)
    {
        const char *r = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
        if (r && *r) reason = r;
    }
    clientBW_t *cl = BW_GetClient(clientNum);
    if (cl && cl->header.state >= CS_CONNECTED)
        SV_DropClient(cl, reason, true);
}

static void PlayerCmd_GetEntityNumber(scr_entref_t entref)
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <entity> getentitynumber()", SCRIPTINSTANCE_SERVER);
    Scr_AddInt_BW(static_cast<int>(entref.entnum), SCRIPTINSTANCE_SERVER);
}

static void PlayerCmd_GetGuid(scr_entref_t entref)
{
    if (entref.classnum != 0)
        Scr_ObjectError_BW("not a player entity", SCRIPTINSTANCE_SERVER);
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <player> getguid()", SCRIPTINSTANCE_SERVER);
    if (entref.entnum < 0 || entref.entnum >= MAX_CLIENTS_BW)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }
    clientBW_t *cl = BW_GetClient(entref.entnum);
    if (!cl)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }
    if (cl->header.netchan.remoteAddress.type == NA_BOT)
    {
        Scr_AddInt_BW(entref.entnum, SCRIPTINSTANCE_SERVER);
        return;
    }
    const char *xuidStr = Info_ValueForKey(cl->userinfo, "xuid");
    if (!xuidStr || !*xuidStr)
    {
        Scr_AddInt_BW(entref.entnum, SCRIPTINSTANCE_SERVER);
        return;
    }
    Scr_AddString(xuidStr, SCRIPTINSTANCE_SERVER);
}

// ---------------------------------------------------------------------------
// Exported lookup tables
// ---------------------------------------------------------------------------

static struct
{
    const char     *name;
    BuiltinFunction handler;
} sv_bots_functions[] = {
    {"addtestclient", reinterpret_cast<BuiltinFunction>(GScr_AddTestClient)},
    {"kick",          reinterpret_cast<BuiltinFunction>(GScr_Kick)},
    {nullptr, nullptr},
};

static struct
{
    const char   *name;
    BuiltinMethod handler;
} sv_bots_methods[] = {
    {"botmoveto",         Scr_BotMoveTo},
    {"botaction",         Scr_BotAction},
    {"botmirror",         Scr_BotMirror},
    {"botstop",           Scr_BotStop},
    {"getentitynumber",   PlayerCmd_GetEntityNumber},
    {"getguid",           PlayerCmd_GetGuid},
    {nullptr, nullptr},
};

extern "C" BuiltinFunction BW_LookupFunction(const char *name)
{
    if (!name)
        return nullptr;
    for (const auto *f = sv_bots_functions; f->name != nullptr; ++f)
    {
        if (_stricmp(name, f->name) == 0)
            return f->handler;
    }
    return nullptr;
}

extern "C" BuiltinMethod BW_LookupMethod(const char *name)
{
    if (!name)
        return nullptr;
    for (const auto *m = sv_bots_methods; m->name != nullptr; ++m)
    {
        if (_stricmp(name, m->name) == 0)
            return m->handler;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: T4 BW module init (r311 — netbuf bisect + reentry guard)\n");
    DbgPrint("sv_bots: [DIAG] weapon=%d userinfo=%d botmove=%d netmsg=%d\n",
             CODXE_DIAG_ENABLE_WEAPON_HOOK,
             CODXE_DIAG_ENABLE_USERINFO_HOOK,
             CODXE_DIAG_ENABLE_BOTUSERMOVE,
             BW_USE_NETMSG);

    CleanBotArray();
    s_pendingBotName[0] = '\0';

#if CODXE_DIAG_ENABLE_WEAPON_HOOK
    G_SelectWeaponIndex_Detour = Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    G_SelectWeaponIndex_Detour.Install();
    DbgPrint("sv_bots: G_SelectWeaponIndex detour INSTALLED\n");
#else
    DbgPrint("sv_bots: G_SelectWeaponIndex detour SKIPPED (r295)\n");
#endif

#if CODXE_DIAG_ENABLE_BOTUSERMOVE
    SV_BotUserMove_Detour = Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    SV_BotUserMove_Detour.Install();
    DbgPrint("sv_bots: SV_BotUserMove detour INSTALLED\n");
#else
    DbgPrint("sv_bots: SV_BotUserMove detour SKIPPED (r295)\n");
#endif

#if CODXE_DIAG_ENABLE_USERINFO_HOOK
    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    SV_UserinfoChanged_Detour.Install();
    DbgPrint("sv_bots: SV_UserinfoChanged detour INSTALLED\n");
#else
    DbgPrint("sv_bots: SV_UserinfoChanged detour SKIPPED (r295)\n");
#endif
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: T4 BW module shutdown\n");

#if CODXE_DIAG_ENABLE_WEAPON_HOOK
    G_SelectWeaponIndex_Detour.Remove();
#endif
#if CODXE_DIAG_ENABLE_BOTUSERMOVE
    SV_BotUserMove_Detour.Remove();
#endif
#if CODXE_DIAG_ENABLE_USERINFO_HOOK
    SV_UserinfoChanged_Detour.Remove();
#endif

    CleanBotArray();
}

} // namespace mp
} // namespace t4
