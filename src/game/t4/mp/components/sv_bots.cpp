//
// Bot Warfare T4 port — engine module.
//
// ===========================================================================
// r328 — SV_AddTestClient slot-0 skip patch (no detours)
// ===========================================================================
//
// THE BUG (confirmed from Ghidra decompile + disassembly of SV_AddTestClient
// @ 0x82281F08):
//   After SV_DirectConnect creates the bot's client slot, SV_AddTestClient
//   runs a scan loop (0x82282038–0x8228207C) that walks every client slot
//   calling NET_CompareBaseAdr(botAddr, slotAddr) to find which slot the bot
//   landed in. The FIRST slot whose address "equals" the bot's is taken as
//   the bot — marked isTestClient, sent gamestate, entered into the world.
//
//   On Xenia (XLive-offline) the bot's netadr is all-zero (memset, port 0)
//   AND the host at slot 0 also has a degenerate, port-0 netadr. For two
//   degenerate addresses NET_CompareBaseAdr returns "equal". So the scan
//   matches slot 0 (the HOST) on iteration 0, then runs SV_SendClientGameState
//   + SV_ClientEnterWorld against the already-in-world host -> freeze.
//   ("Larry" appears because SV_DirectConnect DID create the bot slot first;
//   then the scan misidentifies the host and corrupts it.)
//
// THE FIX (r328):
//   The scan loop must skip slot 0 (the host). Slot 0 is always the host on
//   an offline/listen server; a bot is never slot 0. We patch ONE instruction
//   inside the loop:
//
//     0x8228203C  original: cmpwi cr6, r10, 0   (2F 0A 00 00)
//                           -- tests the slot's connectState
//     0x8228203C  patched:  cmpwi cr6, r30, 0   (2F 1E 00 00)
//                           -- tests r30, the loop counter, instead
//
//   The existing 'beq cr6, LAB_8228206C' immediately below then skips the
//   iteration whenever the counter is 0 — i.e. iteration 0 (the host) is
//   always skipped; iterations 1+ run normally. The degenerate-netadr
//   false-match against the host can no longer occur.
//
//   This is codxe's own proven raw-instruction-patch pattern (see
//   iw4/mp/components/patches.cpp): *(volatile uint32_t*)ADDR = INSTRUCTION.
//   No detour, no trampoline, no Install(). 0x8228203C is reached ONLY by
//   SV_AddTestClient's scan — pregame never executes it, so the r317/r327
//   pregame-freeze failure mode is structurally impossible here.
//
// HISTORY
//   r314  deleted Path C; called the real engine SV_AddTestClient() directly.
//   r316  proved that call hangs on T4 Xenia.
//   r317  added NET_CompareBaseAdr detour; froze in PREGAME ("Awaiting
//         challenge") — installed all 4 detours at once.
//   r318  gutted hook bodies — still froze in pregame (detour mechanism).
//   r319  all detours off — pregame fine, spawn froze (guard disarmed).
//   r322/r326  zero detours — pregame fine, spawn froze (no guard).
//   r327  installed ONLY NET_CompareBaseAdr_impl detour — froze in pregame,
//         proving the detour INSTALLATION on that function breaks pregame.
//   r328  abandons detours entirely. Direct one-instruction patch to
//         SV_AddTestClient's scan loop. addtestclient back in the dispatch
//         table so spawn routes through GScr_AddTestClient (FLUSH markers).
//
// Verified engine addresses (TU7 default_mp.xex):
//   SV_AddTestClient        0x82281F08   (returns gentity_s*, no args)
//   SV_AddTestClient scan   0x82282038–0x8228207C
//   patch site              0x8228203C   (cmpwi in the scan loop)
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
// r328 — SV_AddTestClient scan-loop patch
// ---------------------------------------------------------------------------
// Patch site and instruction words. See file header for full rationale.

static const unsigned int BW_SCANPATCH_ADDR    = 0x8228203C;
static const unsigned int BW_SCANPATCH_ORIG    = 0x2F0A0000;  // cmpwi cr6,r10,0
static const unsigned int BW_SCANPATCH_NEW     = 0x2F1E0000;  // cmpwi cr6,r30,0

static void BW_ApplyScanPatch()
{
    volatile unsigned int *site =
        reinterpret_cast<volatile unsigned int *>(BW_SCANPATCH_ADDR);

    const unsigned int before = *site;

    // Only patch if the site still holds the expected original instruction.
    // Guards against patching twice or against an unexpected binary.
    if (before == BW_SCANPATCH_ORIG)
    {
        *site = BW_SCANPATCH_NEW;
        DbgPrint("sv_bots: r328 — scan-patch APPLIED @0x%08X (0x%08X -> 0x%08X)\n",
                 BW_SCANPATCH_ADDR, before, BW_SCANPATCH_NEW);
    }
    else if (before == BW_SCANPATCH_NEW)
    {
        DbgPrint("sv_bots: r328 — scan-patch ALREADY APPLIED @0x%08X\n",
                 BW_SCANPATCH_ADDR);
    }
    else
    {
        DbgPrint("sv_bots: r328 — scan-patch SKIPPED @0x%08X — unexpected "
                 "instruction 0x%08X (expected 0x%08X)\n",
                 BW_SCANPATCH_ADDR, before, BW_SCANPATCH_ORIG);
    }
}

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
    DbgPrint("sv_bots: ===== BW SYSTEM REPORT (r328) =====\n");

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
// SV_BotUserMove detour — the AI input driver
// ===========================================================================
// r328: detour constructed but NOT installed. r328 tests only spawn; the AI
// input driver is re-enabled in a later build once spawn is confirmed.

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
// r328: detour constructed but NOT installed. Body is pure pass-through.

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ===========================================================================
// SV_UserinfoChanged detour — custom bot names
// ===========================================================================
// r328: detour constructed but NOT installed. Body is pure pass-through.

static char   s_pendingBotName[32] = {0};
static Detour SV_UserinfoChanged_Detour;

static void SV_UserinfoChanged_Hook(clientBW_t *cl)
{
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
// r328: addtestclient IS in the dispatch table — codjumper's addtestclient()
// routes here. GScr_AddTestClient calls the real engine SV_AddTestClient(),
// which now has the slot-0-skip patch applied, so the host can no longer be
// false-matched.
//
// FLUSH markers:
//   FLUSH-A1 : about to enter SV_AddTestClient
//   FLUSH-A2 : SV_AddTestClient RETURNED (r316/r319/r326 never hit this)
//   FLUSH-B  : done

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

    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A1 entering SV_AddTestClient "
             "(r328 — slot-0-skip patch active)\n");

    gentity_s *ent = SV_AddTestClient();

    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A2 SV_AddTestClient RETURNED (ent=0x%08X)\n",
             reinterpret_cast<unsigned int>(ent));

    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-B done\n");

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
// r328: addtestclient IS in the table — routes to GScr_AddTestClient, which
// calls the real engine SV_AddTestClient() (now slot-0-skip patched).

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
// r328: NO detours installed. The only engine modification is the one-
// instruction scan-loop patch applied by BW_ApplyScanPatch(). The three
// detour objects are still constructed (for later builds) but never installed.

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: T4 BW module init (r328 — SV_AddTestClient slot-0 skip patch)\n");

    CleanBotArray();
    s_pendingBotName[0] = '\0';

    // The fix: one-instruction patch to SV_AddTestClient's scan loop so it
    // skips slot 0 (the host) and cannot false-match it as the bot.
    BW_ApplyScanPatch();

    G_SelectWeaponIndex_Detour = Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    // G_SelectWeaponIndex_Detour.Install();    // r328: NOT installed
    DbgPrint("sv_bots: G_SelectWeaponIndex detour NOT installed (r328)\n");

    SV_BotUserMove_Detour = Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    // SV_BotUserMove_Detour.Install();         // r328: NOT installed
    DbgPrint("sv_bots: SV_BotUserMove detour NOT installed (r328)\n");

    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    // SV_UserinfoChanged_Detour.Install();     // r328: NOT installed
    DbgPrint("sv_bots: SV_UserinfoChanged detour NOT installed (r328)\n");

    DbgPrint("sv_bots: r328 — module loaded, NO detours, scan-patch only\n");
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: T4 BW module shutdown\n");

    // Detours were never installed; Remove() on an uninstalled Detour is safe.
    G_SelectWeaponIndex_Detour.Remove();
    SV_BotUserMove_Detour.Remove();
    SV_UserinfoChanged_Detour.Remove();

    // NOTE: the scan-patch is intentionally NOT reverted. codxe's own
    // patch convention (iw4/mp/components/patches.cpp) leaves patches in
    // place; the xex is reloaded fresh each run.

    CleanBotArray();
}

} // namespace mp
} // namespace t4
