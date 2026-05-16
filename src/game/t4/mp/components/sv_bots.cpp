//
// Bot Warfare T4 port — engine module.
//
// ===========================================================================
// r317 — surgical fix for the SV_AddTestClient post-scan hang
// ===========================================================================
//
// r314 deleted Path C and called the real engine SV_AddTestClient() directly,
// the codxe-IW3-reference architecture. The r316 log proved that call HANGS on
// T4 Xenia: [ADDTESTCLIENT] FLUSH-A prints, FLUSH-B never does — control enters
// SV_AddTestClient (0x82281F08) and never returns.
//
// The cause is now decompiler-confirmed, and it is NOT what r294 assumed.
// r294 said "two all-zero NA_BOT netadrs compare equal". The NET_CompareBaseAdr
// decompile shows that is over-stated:
//
//   NET_CompareBaseAdr_impl (0x82278B20):
//     if (typeA == typeB) {
//         if (typeA != 2 && typeA != 0) { ...compare IP bytes (type 4)... }
//         // type 0 (NA_BOT) and type 2 (loopback) FALL THROUGH to:
//         portA = *(ushort*)(a + 2);   // netadr_t.port at offset 8
//         portB = *(ushort*)(b + 2);
//     }
//     return portA - portB;            // 0 == "equal"
//
// So two NA_BOT addrs compare equal ONLY IF their .port fields match. The real
// bug: SV_AddTestClient does memset(netadr, 0, 12) for the bot — port 0 — and
// the Xenia host (XLive-offline) netadr is also all-zero — port 0. Equal port
// => "equal" => SV_AddTestClient's post-SV_DirectConnect scan matches the HOST
// at slot 0, writes isTestClient / SV_SendClientGameState / SV_ClientEnterWorld
// onto the live host => corruption => freeze. (Verified: SV_AddTestClient at
// 0x8228205C calls NET_CompareBaseAdr; the wrapper 0x82278BD0 calls _impl.)
//
// NET_CompareBaseAdr itself is CORRECT — it is being fed a zero port. The fix
// is therefore surgical and leaves every engine function behaving as designed:
// detour NET_CompareBaseAdr_impl, and while a bot-add is in progress, report
// "not equal" for any NA_BOT-vs-NA_BOT comparison. The post-scan then skips the
// host, finds the genuine new bot slot, and returns.
//
// Scope of the detour:
//   - The hook is INERT unless s_botAddInProgress is set. It is set ONLY around
//     the SV_AddTestClient() call, so no unrelated caller is ever affected.
//   - During SV_AddTestClient(), SV_DirectConnect also runs an internal
//     NET_CompareBaseAdr reconnect scan (SV_DirectConnect at 0x82281718 calls
//     the wrapper). Our hook fires there too — forcing "not equal" means "this
//     bot is not a reconnecting client", which is correct for a fresh bot.
//   - Detouring _impl (not the wrapper) catches BOTH call paths, since the
//     wrapper 0x82278BD0 calls _impl 0x82278B20.
//
// Return convention (verified from the wrapper disassembly):
//   NET_CompareBaseAdr wrapper does cntlzw/rlwinm on _impl's return:
//     _impl returns 0       -> wrapper returns 1  ("equal")
//     _impl returns nonzero -> wrapper returns 0  ("not equal")
//   So the hook returns 1 to force "not equal".
//
// Everything else is r314 unchanged: Path C stays deleted, three detours
// (SV_BotUserMove, G_SelectWeaponIndex, SV_UserinfoChanged), SV_CalcPings not
// detoured (T4 frame fields inert).
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
    DbgPrint("sv_bots: ===== BW SYSTEM REPORT (r317) =====\n");

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
// r317 — NET_CompareBaseAdr_impl detour
// ===========================================================================
// While a bot-add is in progress, force NA_BOT-vs-NA_BOT comparisons to report
// "not equal" so SV_AddTestClient's post-scan does not match the host at slot 0.
// See the file header for the full rationale and the return convention.

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
    // a[0] / b[0] are the netadr_t.type ints. NA_BOT == 0.
    if (s_botAddInProgress && a != nullptr && b != nullptr &&
        *a == 0 && *b == 0)
    {
        // Both NA_BOT. Return nonzero -> wrapper reports "not equal" -> the
        // SV_AddTestClient post-scan skips this slot instead of matching it.
        return 1;
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
//
// r318 DIAGNOSTIC: body GUTTED to a pure pass-through. The detour is still
// installed and still intercepts every call, but the hook does NOTHING except
// forward to the original. This isolates "hook body bug" from "detour mechanism
// bug" for the Awaiting-challenge pregame freeze:
//   - pregame LOADS with this gutted   -> the freeze was in the hook BODY below.
//   - pregame STILL FREEZES gutted     -> the freeze is the detour MECHANISM
//                                         on this short function.
// The real body (one line: g_botai[clientNum].weapon = iWeaponIndex) is kept
// in the comment so r319 can restore it once the verdict is known.
//
//   ORIGINAL r317 BODY:
//     if (clientNum >= 0 && clientNum < MAX_CLIENTS_BW)
//         g_botai[clientNum].weapon = static_cast<unsigned char>(iWeaponIndex);

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    // r318: pure pass-through — no g_botai write.
    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ===========================================================================
// SV_UserinfoChanged detour — custom bot names
// ===========================================================================
//
// r318 DIAGNOSTIC: body GUTTED to a pure pass-through, same rationale as the
// G_SelectWeaponIndex hook above. The name-patch is suspended for this build.
// Note: SV_UserinfoChanged's userinfo buffer is at client+0x6DC (verified in
// the decompile: `addi r23, r28, 0x6dc`). When r319 restores the body, confirm
// clientBW_t.userinfo resolves to that offset.
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
    // r318: pure pass-through — no name patch.
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
// Stash an optional custom name, arm the NET_CompareBaseAdr_impl detour, call
// the REAL engine SV_AddTestClient(), disarm, clear the name. The
// SV_UserinfoChanged detour applies the name mid-connect.
//
// FLUSH markers bracket SV_AddTestClient(). r316 showed FLUSH-A printing and
// FLUSH-B not — if r317's detour works, FLUSH-B should now print and ent be
// non-NULL.

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
    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A before SV_AddTestClient "
             "(NET_CompareBaseAdr guard ARMED)\n");
    gentity_s *ent = SV_AddTestClient();
    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-B after SV_AddTestClient (ent=0x%08X)\n",
             reinterpret_cast<unsigned int>(ent));
    s_botAddInProgress = 0;

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
// r319 DIAGNOSTIC: ALL FOUR Install() calls are commented out.
//
// r318 proved the freeze is NOT in the hook bodies (both gutted, still froze).
// Two hypotheses remain:
//   (b1) the detour MECHANISM corrupts a function when it splices in, OR
//   (b2) the freeze is NOT our code at all (codxe-core / netplay session).
//
// r319 installs ZERO detours. The Detour objects are still constructed (cheap,
// inert — construction alone patches nothing; only Install() writes memory).
// If r319 loads past "Awaiting challenge" -> the detour MECHANISM is the cause.
// If r319 still freezes -> sv_bots is innocent; the freeze is codxe-core or the
// netplay/XLive session handshake, and the investigation moves entirely off
// this file.
//
// To restore: uncomment the four .Install() lines. The Detour() constructions
// are left in place so restoring is a pure uncomment, no retyping.

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: T4 BW module init (r319 — DIAG: ALL detours NOT installed)\n");

    CleanBotArray();
    s_pendingBotName[0]  = '\0';
    s_botAddInProgress   = 0;

    NET_CompareBaseAdr_impl_Detour =
        Detour(NET_CompareBaseAdr_impl, NET_CompareBaseAdr_impl_Hook);
    // NET_CompareBaseAdr_impl_Detour.Install();   // r319: NOT installed
    DbgPrint("sv_bots: NET_CompareBaseAdr_impl detour NOT installed (r319 diag)\n");

    G_SelectWeaponIndex_Detour = Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    // G_SelectWeaponIndex_Detour.Install();       // r319: NOT installed
    DbgPrint("sv_bots: G_SelectWeaponIndex detour NOT installed (r319 diag)\n");

    SV_BotUserMove_Detour = Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    // SV_BotUserMove_Detour.Install();            // r319: NOT installed
    DbgPrint("sv_bots: SV_BotUserMove detour NOT installed (r319 diag)\n");

    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    // SV_UserinfoChanged_Detour.Install();        // r319: NOT installed
    DbgPrint("sv_bots: SV_UserinfoChanged detour NOT installed (r319 diag)\n");

    DbgPrint("sv_bots: r319 — module loaded, ZERO detours active\n");
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: T4 BW module shutdown\n");

    // r319: nothing was installed, so Remove() is a no-op — but harmless to
    // leave; Detour::Remove() on an un-installed detour does nothing.
    NET_CompareBaseAdr_impl_Detour.Remove();
    G_SelectWeaponIndex_Detour.Remove();
    SV_BotUserMove_Detour.Remove();
    SV_UserinfoChanged_Detour.Remove();

    CleanBotArray();
}

} // namespace mp
} // namespace t4
