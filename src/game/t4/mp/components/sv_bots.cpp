//
// Bot Warfare T4 port — engine module.
//
// ===========================================================================
// r320-A — first real test of the r317 surgical guard at the bot-spawn step
// ===========================================================================
//
// HISTORY
//   r314  deleted Path C; called the real engine SV_AddTestClient() directly.
//   r316  proved that call hangs on T4 Xenia: FLUSH-A prints, FLUSH-B never.
//   r317  added the NET_CompareBaseAdr_impl detour ("surgical guard") — but
//         froze in PREGAME (Awaiting challenge), before bot-spawn was reached,
//         so the guard itself was NEVER actually exercised at the spawn step.
//   r318  gutted hook bodies — still froze in pregame. Hook bodies cleared.
//   r319  commented out ALL FOUR Install() calls. Result: pregame LOADED fine
//         (so the pregame freeze was the detour MECHANISM, not our hook code),
//         BUT pressing D-pad Up still froze — because r319 left
//         GScr_AddTestClient calling the REAL SV_AddTestClient() with the
//         guard DISARMED. That r319 spawn-hang reproduced r316 by construction
//         and proved nothing new.
//
// r320-A — THE TEST THAT WAS NEVER RUN
//   Re-enable ONLY NET_CompareBaseAdr_impl_Detour.Install(). The other three
//   detours stay off (they are irrelevant to bot spawn, and r318 already
//   cleared their bodies). This is the first genuine test of whether r317's
//   surgical fix actually makes the real SV_AddTestClient() complete.
//
//   PLUS a bug fix in the hook. The original r317 hook tested only
//   (*a == 0 && *b == 0) — NA_BOT vs NA_BOT. But the slot that corrupts is the
//   HOST at slot 0, and on Xenia (XLive-offline) the host netadr is degenerate
//   too: type 0 (NA_BOT) OR type 2 (NA_LOOPBACK). NET_CompareBaseAdr_impl falls
//   through to the port-compare for BOTH type 0 and type 2 (see file header of
//   symbols_bw_ext.h). The original test missed the bot-vs-host case entirely
//   whenever the host was loopback. r320-A widens the guard to fire for any
//   degenerate-vs-degenerate pair (type 0 or 2 on both sides). This is most
//   likely why r317 could never have worked even in principle.
//
// THE DECISION TREE FOR THIS BUILD
//   A1 + A2 + B all print, ent non-null  -> guard works; r317 design correct;
//                                           the bug was the missing type==2.
//                                           Next: re-enable the other 3 detours.
//   A1 prints, A2 never                  -> still hanging INSIDE SV_AddTestClient
//                                           even with the guard -> go to r320-B
//                                           (hand-port / Path C resurrected).
//   A2 prints, B weird / ent bad         -> hang moved downstream -> new info.
//
// Verified engine addresses (TU7 default_mp.xex):
//   SV_AddTestClient        0x82281F08   (returns gentity_s*, no args)
//   NET_CompareBaseAdr      0x82278BD0   (wrapper)
//   NET_CompareBaseAdr_impl 0x82278B20   (inner — detour target)
//   SV_UserinfoChanged      0x82280690
//   SV_BotUserMove          0x82286D68
//   SV_ClientThink          0x82280F38
//   G_SelectWeaponIndex     0x8225D6D8
//

#include "pch.h"
#include "sv_bots.h"

#include <cmath>
#include <cstring>

#pragma warning(disable: 4505)  // unreferenced local fn

namespace t4
{
namespace mp
{

using namespace t4::mp::bw;

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

// ---------------------------------------------------------------------------
// SYSTEM REPORT — engine-global sanity dump
// ---------------------------------------------------------------------------

static void BW_DumpClientSlot(const char *cl, int slot)
{
    const int   state = *reinterpret_cast<const int *>(cl);
    const char *name  = cl + BW_CLIENT_NAME_OFF;

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
    DbgPrint("sv_bots: ===== BW SYSTEM REPORT (r320-A) =====\n");

    const unsigned int hdrAddr  = BW_ADDR_SVSHEADER;
    const unsigned int clientsP = *reinterpret_cast<const unsigned int *>(hdrAddr + 0x0);
    const int          svTime   = BW_svs_time();

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

    const unsigned int gentBase    = BW_ADDR_G_ENTITIES;
    const int          gent0Number = *reinterpret_cast<const int *>(gentBase + 0x0);
    DbgPrint("sv_bots: g_entities @0x%08X stride=0x%X  ent[0].s.number=%d\n",
             gentBase, BW_GENTITY_STRIDE, gent0Number);

    DbgPrint("sv_bots: maxclients (constant) = %d\n", MAX_CLIENTS_BW);
    DbgPrint("sv_bots: ===== END REPORT =====\n");
}

// ===========================================================================
// r320-A — NET_CompareBaseAdr_impl detour (the surgical guard)
// ===========================================================================
// While a bot-add is in progress, force any comparison between two DEGENERATE
// netadrs (type 0 NA_BOT or type 2 NA_LOOPBACK) to report "not equal", so the
// SV_AddTestClient post-SV_DirectConnect scan does NOT false-match the host at
// slot 0 (which on Xenia-offline also has a degenerate, port-0 netadr).
//
// r317 bug fixed here: the original test was (*a == 0 && *b == 0) only — it
// matched NA_BOT vs NA_BOT but MISSED bot-vs-host when the host is loopback
// (type 2). NET_CompareBaseAdr_impl falls through to the port-compare for both
// type 0 and type 2, so both must be treated as degenerate.
//
// Return convention (verified from the wrapper disassembly at 0x82278BD0):
//   _impl returns 0       -> wrapper returns 1  ("equal")
//   _impl returns nonzero -> wrapper returns 0  ("not equal")
// So the hook returns 1 (nonzero) to force "not equal".
//
// The hook is INERT unless s_botAddInProgress is set; it is set ONLY around
// the SV_AddTestClient() call, so no unrelated caller is ever affected.

static Detour       NET_CompareBaseAdr_impl_Detour;
static volatile int s_botAddInProgress = 0;

static long long NET_CompareBaseAdr_impl_Hook(unsigned int *a,
                                              unsigned int *b,
                                              unsigned __int64 p3,
                                              unsigned __int64 p4,
                                              unsigned __int64 p5,
                                              unsigned __int64 p6,
                                              unsigned __int64 p7)
{
    // a[0] / b[0] are the netadr_t.type ints. NA_BOT == 0, NA_LOOPBACK == 2.
    if (s_botAddInProgress && a != nullptr && b != nullptr)
    {
        const unsigned int ta = *a;
        const unsigned int tb = *b;
        const bool aDegenerate = (ta == 0u || ta == 2u);
        const bool bDegenerate = (tb == 0u || tb == 2u);
        if (aDegenerate && bDegenerate)
        {
            // Force "not equal" so the post-scan skips this slot instead of
            // false-matching the host.
            return 1;
        }
    }

    return NET_CompareBaseAdr_impl_Detour
        .GetOriginal<NET_CompareBaseAdr_impl_t>()(a, b, p3, p4, p5, p6, p7);
}

// ===========================================================================
// SV_BotUserMove detour — the AI input driver
// ===========================================================================
// 1:1 port of the IW3 reference SV_BotUserMove_Stub. T4 difference: the IW3
// reference gates the input block on g_clients[clientNum].sess.archiveTime==0
// (killcam suppression). structs_bw_ext.h does not expose sess.archiveTime,
// so the gate is omitted — T4 bots are not input-frozen during killcam.
//
// r320-A: detour constructed but NOT installed (irrelevant to bot spawn).

static Detour SV_BotUserMove_Detour;

static void SV_BotUserMove_Stub(clientBW_t *cl)
{
    if (!cl->gentity)
        return;

    const int clientNum = static_cast<int>(
        (reinterpret_cast<char *>(cl) - reinterpret_cast<char *>(BW_svs_clients()))
        / static_cast<int>(BW_CLIENT_STRIDE));
    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
        return;

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

// ===========================================================================
// G_SelectWeaponIndex detour — track per-client weapon
// ===========================================================================
// r320-A: detour constructed but NOT installed. Body still gutted to a pure
// pass-through (r318 diagnostic state). Real body kept in comment for restore.
//
//   ORIGINAL r317 BODY:
//     if (clientNum >= 0 && clientNum < MAX_CLIENTS_BW)
//         g_botai[clientNum].weapon = static_cast<unsigned char>(iWeaponIndex);

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    // r318/r320-A: pure pass-through — no g_botai write.
    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ===========================================================================
// SV_UserinfoChanged detour — custom bot names
// ===========================================================================
// r320-A: detour constructed but NOT installed. Body still gutted to a pure
// pass-through (r318 diagnostic state). Real body kept in comment for restore.
// Note: SV_UserinfoChanged's userinfo buffer is at client+0x6DC (verified).
//
//   ORIGINAL r317 BODY:
//     if (s_pendingBotName[0] &&
//         cl->header.netchan.remoteAddress.type == NA_BOT &&
//         cl->header.state == CS_CONNECTED)
//     {
//         Info_SetValueForKey(cl->userinfo, "name", s_pendingBotName);
//     }

static char   s_pendingBotName[32] = {0};
static Detour SV_UserinfoChanged_Detour;

static void SV_UserinfoChanged_Hook(clientBW_t *cl)
{
    // r318/r320-A: pure pass-through — no name patch.
    SV_UserinfoChanged_Detour.GetOriginal<SV_UserinfoChanged_t>()(cl);
}

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
// GSC global function — addtestclient(<name>)
// ---------------------------------------------------------------------------
// Stash an optional custom name, ARM the NET_CompareBaseAdr_impl guard, call
// the REAL engine SV_AddTestClient(), disarm, clear the name.
//
// r320-A FLUSH markers:
//   FLUSH-A1 : guard armed, about to enter SV_AddTestClient
//   FLUSH-A2 : SV_AddTestClient RETURNED (this is the line r316/r319 never hit)
//   FLUSH-B  : guard disarmed
// If A1 prints and A2 does not -> still hanging inside SV_AddTestClient even
// with the guard installed -> r320-B (hand-port) is required.

static void GScr_AddTestClient()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) == 1)
    {
        const char *string = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);

        char name[32];
        int  i = 0, j = 0;
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

        std::strncpy(s_pendingBotName, name, sizeof(s_pendingBotName) - 1);
        s_pendingBotName[sizeof(s_pendingBotName) - 1] = '\0';
    }

    DbgPrint("sv_bots: [ADDTESTCLIENT] name='%s'\n",
             s_pendingBotName[0] ? s_pendingBotName : "(default)");

    BW_SystemReport();

    // Arm the NET_CompareBaseAdr_impl detour ONLY for the duration of the
    // SV_AddTestClient() call. Outside this window the hook is fully inert.
    s_botAddInProgress = 1;
    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A1 guard armed=%d, entering SV_AddTestClient "
             "(r320-A guard INSTALLED)\n", s_botAddInProgress);

    gentity_s *ent = SV_AddTestClient();

    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A2 SV_AddTestClient RETURNED (ent=0x%08X)\n",
             reinterpret_cast<unsigned int>(ent));

    s_botAddInProgress = 0;
    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-B guard disarmed\n");

    s_pendingBotName[0] = '\0';

    if (ent)
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] success, entnum=%d\n", ent->s.number);
        Scr_AddEntityNum(ent->s.number, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] SV_AddTestClient returned NULL\n");
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
    // r321 TEST: addtestclient hijack DISABLED -- falls through to the native
    // WaW engine builtin. GScr_AddTestClient kept defined but unreachable.
    {"kick",          reinterpret_cast<BuiltinFunction>(GScr_Kick)},
    {nullptr, nullptr},
};

// r321: keep GScr_AddTestClient referenced so /WX (C4505) does not flag it
// while it is out of the dispatch table. volatile prevents the optimizer
// dropping it. Never called.
static const volatile BuiltinFunction s_keep_GScr_AddTestClient_alive =
    reinterpret_cast<BuiltinFunction>(GScr_AddTestClient);

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
// r320-A: ONLY NET_CompareBaseAdr_impl_Detour.Install() is enabled. The other
// three detours are constructed but NOT installed — they are irrelevant to the
// bot-spawn path and r318 already cleared their bodies. This isolates one
// question: with the surgical guard ACTIVE, does the real SV_AddTestClient()
// complete?
//
// To restore the other three later: uncomment their .Install() lines.

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: T4 BW module init (r322 — ZERO detours installed)\n");

    CleanBotArray();
    s_pendingBotName[0] = '\0';
    s_botAddInProgress  = 0;

    NET_CompareBaseAdr_impl_Detour =
        Detour(NET_CompareBaseAdr_impl, NET_CompareBaseAdr_impl_Hook);
    // NET_CompareBaseAdr_impl_Detour.Install();  // r322: DISABLED — pregame "Awaiting challenge" freeze (r319 finding: detour mechanism breaks pregame)
    DbgPrint("sv_bots: NET_CompareBaseAdr_impl detour NOT installed (r322)\n");

    G_SelectWeaponIndex_Detour = Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    // G_SelectWeaponIndex_Detour.Install();    // r320-A: NOT installed
    DbgPrint("sv_bots: G_SelectWeaponIndex detour NOT installed (r320-A)\n");

    SV_BotUserMove_Detour = Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    // SV_BotUserMove_Detour.Install();         // r320-A: NOT installed
    DbgPrint("sv_bots: SV_BotUserMove detour NOT installed (r320-A)\n");

    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    // SV_UserinfoChanged_Detour.Install();     // r320-A: NOT installed
    DbgPrint("sv_bots: SV_UserinfoChanged detour NOT installed (r320-A)\n");

    DbgPrint("sv_bots: r322 — module loaded, ZERO detours active\n");
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: T4 BW module shutdown\n");

    NET_CompareBaseAdr_impl_Detour.Remove();
    G_SelectWeaponIndex_Detour.Remove();
    SV_BotUserMove_Detour.Remove();
    SV_UserinfoChanged_Detour.Remove();

    CleanBotArray();
}

} // namespace mp
} // namespace t4
