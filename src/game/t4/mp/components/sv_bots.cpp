//
// Bot Warfare T4 port — engine module.
//
// ===========================================================================
// r330 — TU7 re-derivation build: dump SV_AddTestClient bytes to the log
// ===========================================================================
//
// WHY THIS BUILD EXISTS
//   r328's one-instruction scan-patch @0x8228203C failed: the running TU7
//   image reported `scan-patch SKIPPED — unexpected instruction 0x480416ED`.
//   0x480416ED decodes to `bl 0x822C3728` — a CALL, not the expected
//   `cmpwi cr6,r10,0`. Investigation found the Ghidra project was built from
//   the BASE disc default_mp.xex (module version 0.0.0.8), NOT the TU7-patched
//   image (0.0.7.8) that Xenia actually executes. Every Ghidra-derived address
//   in the handoff doc is therefore suspect.
//
//   This build does NOT attempt any patch. It dumps the raw bytes of the
//   SV_AddTestClient region straight from the running (TU7-patched) process
//   to the log. That dump IS the real executing code — no binary-mismatch
//   risk possible. The bytes get disassembled offline to locate the real
//   scan loop, the real patch site, and the real register numbers.
//
// HISTORY
//   r314  deleted Path C; called the real engine SV_AddTestClient() directly.
//   r316  proved that call hangs on T4 Xenia: FLUSH-A prints, FLUSH-B never.
//   r317  installed all four detours -> froze in pregame.
//   r318  gutted hook bodies -> still froze -> hook bodies cleared.
//   r319  zero detours installed -> pregame fine, spawn still froze.
//   r327  installed ONLY NET_CompareBaseAdr_impl -> still froze pregame ->
//         detour mechanism itself breaks pregame. Detour route abandoned.
//   r328  one-instruction raw scan-patch @0x8228203C -> SKIPPED (wrong binary).
//   r330  THIS BUILD — no patch, no detours; dumps SV_AddTestClient bytes.
//
// Engine addresses below are from the OLD (base 0.0.0.8) Ghidra project and
// are NOT trusted. SV_AddTestClient's ENTRY (0x82281F08) is trusted only
// because FLUSH-A1 demonstrably prints from inside it on TU7.
//
//   SV_AddTestClient        0x82281F08   (entry — trusted: FLUSH-A1 prints)
//   NET_CompareBaseAdr      0x82278BD0   (SUSPECT — base-image address)
//   NET_CompareBaseAdr_impl 0x82278B20   (SUSPECT — base-image address)
//   SV_UserinfoChanged      0x82280690   (SUSPECT)
//   SV_BotUserMove          0x82286D68   (SUSPECT)
//   SV_ClientThink          0x82280F38   (SUSPECT)
//   G_SelectWeaponIndex     0x8225D6D8   (SUSPECT)
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
// r330 diagnostic: dump the SV_AddTestClient region from the live TU7 process
// ---------------------------------------------------------------------------
// SV_AddTestClient entry is 0x82281F08. We dump from a little before the entry
// through entry + 0x380 (896 bytes / 224 instructions) so the scan loop is
// captured with margin on both sides. Pure reads of executable memory — no
// detour, no patch, cannot perturb anything.

#define BW_DUMP_BASE   0x82281F08u   // SV_AddTestClient entry
#define BW_DUMP_LEN    0x380u        // bytes to dump (224 instructions)

static void BW_DumpAddTestClientRegion()
{
    DbgPrint("sv_bots: DUMP begin SV_AddTestClient region "
             "(base=0x%08X len=0x%X)\n", BW_DUMP_BASE, BW_DUMP_LEN);

    const volatile unsigned int *p =
        reinterpret_cast<const volatile unsigned int *>(BW_DUMP_BASE);

    for (unsigned int off = 0; off < BW_DUMP_LEN; off += 4)
    {
        const unsigned int addr = BW_DUMP_BASE + off;
        const unsigned int word = p[off / 4u];
        DbgPrint("sv_bots: DUMP %08X: %08X\n", addr, word);
    }

    DbgPrint("sv_bots: DUMP end SV_AddTestClient region\n");
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
    DbgPrint("sv_bots: ===== BW SYSTEM REPORT (r330) =====\n");

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
// Detours — r330: ALL constructed but NONE installed.
// ===========================================================================
// r327 proved the Detour MECHANISM breaks pregame. r330 installs zero detours.
// The hook/stub bodies are kept compilable for future use only.

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
    if (s_botAddInProgress && a != nullptr && b != nullptr)
    {
        const unsigned int ta = *a;
        const unsigned int tb = *b;
        const bool aDegenerate = (ta == 0u || ta == 2u);
        const bool bDegenerate = (tb == 0u || tb == 2u);
        if (aDegenerate && bDegenerate)
            return 1;
    }

    return NET_CompareBaseAdr_impl_Detour
        .GetOriginal<NET_CompareBaseAdr_impl_t>()(a, b, p3, p4, p5, p6, p7);
}

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

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

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
// r330: addtestclient IS in the dispatch table so GScr_AddTestClient runs.
// NO scan-patch is applied this build, so pressing D-Pad Up WILL still freeze
// inside SV_AddTestClient — that is expected and acceptable. The point of r330
// is the DUMP printed at init; the spawn test is not required for r330's goal.
// (You can still press D-Pad Up if you want to re-confirm FLUSH-A1, but the
// dump is already in the log by then.)
//
// FLUSH markers:
//   FLUSH-A1 : about to enter SV_AddTestClient
//   FLUSH-A2 : SV_AddTestClient RETURNED  (not expected to print this build)

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
             "(r330 — NO patch, freeze here is expected)\n");

    gentity_s *ent = SV_AddTestClient();

    DbgPrint("sv_bots: [ADDTESTCLIENT] FLUSH-A2 SV_AddTestClient RETURNED (ent=0x%08X)\n",
             reinterpret_cast<unsigned int>(ent));

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
    // r330: addtestclient IS in the table so GScr_AddTestClient runs.
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
// r330: ZERO detours installed (r327 proved the Detour mechanism breaks
// pregame). NO scan-patch (r328 proved 0x8228203C is wrong on TU7). The ONLY
// new behaviour is BW_DumpAddTestClientRegion() — it prints the real TU7
// bytes of SV_AddTestClient to the log for offline disassembly.

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: T4 BW module init (r330 — TU7 byte-dump diagnostic)\n");

    CleanBotArray();
    s_pendingBotName[0] = '\0';
    s_botAddInProgress  = 0;

    // ----- the whole point of r330 -----
    BW_DumpAddTestClientRegion();

    // Detours constructed but deliberately NOT installed.
    NET_CompareBaseAdr_impl_Detour =
        Detour(NET_CompareBaseAdr_impl, NET_CompareBaseAdr_impl_Hook);
    G_SelectWeaponIndex_Detour =
        Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    SV_BotUserMove_Detour =
        Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    SV_UserinfoChanged_Detour =
        Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);

    DbgPrint("sv_bots: r330 — module loaded, NO detours, NO patch, dump only\n");
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: T4 BW module shutdown\n");
    CleanBotArray();
}

} // namespace mp
} // namespace t4
