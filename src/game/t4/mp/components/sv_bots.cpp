//
// Bot Warfare T4 port — engine module.
//
// Ports the codxe IW3 sv_bots.cpp to T4 X360 (TU7). Adds the BW C++ surface
// that the GSC layer (scripts/mp/bots*.gsc + maps/mp/bots/_bot*.gsc) drives:
//
//   GSC functions
//     addtestclient(<name>)        — spawn a bot (returns the bot entity)
//
//   GSC entity methods (on a bot entity)
//     <bot> botMoveTo(<vec3>)      — drive forward/strafe toward a world point
//     <bot> botAction("+fire")     — set/clear a button bit
//     <bot> botMirror(<player>)    — copy another client's lastUsercmd 1:1
//     <bot> botStop()              — clear all bot input state
//
// Architecture notes:
//
//   - ALL Scr_*, va, and SV_ClientThink calls go through symbols_bw_ext.h's
//     verified addresses (suffix `_BW`). The stock codxe T4 symbols.h is
//     largely incorrect on TU7 default_mp.xex; see audit comments in
//     symbols_bw_ext.h.
//
//   - GSC builtin registration: sv_bots exports BW_LookupFunction and
//     BW_LookupMethod (`extern "C"`). The existing gsc_functions and
//     gsc_client_methods modules call them BEFORE falling through to the
//     engine.
//
//   - gentity is at client_t + 0x21324 on T4 (stock codxe T4 struct.h says
//     +0x213F4 — wrong; that's lastPacketTime). We access via the parallel
//     `clientBW_t` view defined in structs_bw_ext.h.
//
//   - On T4 the entity-from-script-arg path is Scr_GetEntityNum (returns
//     int entnum) → &g_entities[entnum]. IW3's Scr_GetEntity returned a
//     gentity_s* directly; T4 does not have an equivalent.
//
// r341 — addtestclient interception A/B (engine-builtin fall-through):
//
//   ISOLATION EXPERIMENT. Two r339-bw logs proved the class-select stall is
//   NOT any BW server-path detour: both runs had weapon=0 userinfo=0
//   botmove=0 (zero detours installed) and bw v11 bots still stuck at class.
//   The detours are exonerated for class-select.
//
//   The ONLY remaining behavioural difference between r339-bw and stock
//   r261 (which has no sv_bots.cpp and on which bw v11 bots DO pick a class
//   and spawn) is the addtestclient GSC route:
//     r261     addtestclient() -> engine native GSC builtin (full connect
//              sequence: SV_AddTestClient + whatever the builtin does after)
//     r339-bw  addtestclient() -> BW GScr_AddTestClient (bare SV_AddTestClient
//              only; does NOT replicate the post-spawn builtin work)
//
//   r341 comments the addtestclient entry out of sv_bots_functions[], so
//   codxe-core's Scr_GetFunction_Hook falls through to the engine builtin —
//   i.e. exactly the r261 code path on the patched binary.
//
//   DECISIVE OUTCOMES:
//     bots pick class + spawn  -> GScr_AddTestClient IS the regression.
//                                 r342 reworks it to delegate to / replicate
//                                 the engine builtin.
//     bots still stuck         -> the regression is in a codxe-CORE change
//                                 between r261 and HEAD, not sv_bots.cpp.
//                                 Bisect core next.
//
//   All three BW server-path detours remain compiled OUT (toggles all 0).
//   kick() interception is kept (bw v11 calls kick(); harmless, not on the
//   class path). The BW player methods are kept (bw v11 only calls
//   getentitynumber/getguid from them, both return entref.entnum-equivalent
//   values identical to the engine).
//
//   BANNER: prints an unambiguous r341 tag + the live addtestclient route.
//   If xenia.log does NOT show "r341" in the [DIAG] line, the installed
//   codxe.xex is stale — the run is void, re-deploy before debugging.
//
// r293 — vanilla bot spawn:
//
//   CoD Jumper proves vanilla SV_AddTestClient() works on Xenia. We call
//   it directly, the same way CoD Jumper's GSC does. The Path C bypass
//   from r292 is removed — it was solving a problem that didn't exist
//   in practice (the NET_CompareBaseAdr fall-through is real in the
//   disassembly but doesn't manifest as host corruption on Xenia).
//

#include "pch.h"
#include "sv_bots.h"

#include <cmath>
#include <cstring>

// MSVC C4505: "unreferenced local function has been removed". Our static
// hook bodies (G_SelectWeaponIndex_Hook, SV_BotUserMove_Stub,
// SV_UserinfoChanged_Hook) are address-only references — their function
// pointers are passed to Detour() but they are never called directly from
// C++. The optimizer can't see the address-take through the constructor
// argument, so it complains. /WX promotes this to an error.
#pragma warning(disable: 4505)

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
// Button mapping (T4 button_mask → IW3-style names used by upstream GSC)
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
    return &reinterpret_cast<clientBW_t *>(svsHeader->clients)[clientNum];
}

// Validate the entref points at a real player entity. T4 entref carries
// {entnum, classnum, localclientnum}; classnum 0 means "entity" (vs world
// or fielded object).
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
// G_SelectWeaponIndex detour — track per-client weapon for usercmd synthesis
// ---------------------------------------------------------------------------

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    if (clientNum >= 0 && clientNum < MAX_CLIENTS_BW)
        g_botai[clientNum].weapon = static_cast<unsigned char>(iWeaponIndex);

    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ---------------------------------------------------------------------------
// SV_BotUserMove detour — core bot driver
// ---------------------------------------------------------------------------

static Detour SV_BotUserMove_Detour;

static void SV_BotUserMove_Stub(clientBW_t *cl)
{
    if (!cl->gentity)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }

    const int clientNum = static_cast<int>(cl - reinterpret_cast<clientBW_t *>(svsHeader->clients));
    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
    {
        SV_BotUserMove_Detour.GetOriginal<SV_BotUserMove_t>()(cl);
        return;
    }

    // Defense in depth: only inject input for real bot clients.
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

    cmd.serverTime = svsHeader->time;
    cmd.weapon     = g_botai[clientNum].weapon;

    cmd.buttons = static_cast<button_mask>(g_botai[clientNum].buttons);

    if (g_botai[clientNum].doMove)
    {
        gentity_s *ent = cl->gentity;

        // Vector to target (XY plane).
        float move_pos[2];
        move_pos[0] = g_botai[clientNum].moveTo[0] - ent->r.currentOrigin[0];
        move_pos[1] = g_botai[clientNum].moveTo[1] - ent->r.currentOrigin[1];

        const float distance = std::sqrt(move_pos[0] * move_pos[0] + move_pos[1] * move_pos[1]);
        g_botai[clientNum].doMove = (distance > 7.0f) ? 1 : 0;

        // Rotate world-space offset into bot's local frame (negate yaw).
        const float yaw_rad = -ent->r.currentAngles[1] * (3.14159265358979323846f / 180.0f);
        const float s = std::sin(yaw_rad);
        const float c = std::cos(yaw_rad);
        const float rx = move_pos[0] * c - move_pos[1] * s;
        const float ry = move_pos[0] * s + move_pos[1] * c;
        move_pos[0] = rx;
        move_pos[1] = ry;

        // Scale dominant axis to 127, preserve direction.
        const float absX = move_pos[0] < 0.0f ? -move_pos[0] : move_pos[0];
        const float absY = move_pos[1] < 0.0f ? -move_pos[1] : move_pos[1];
        const float maxabs = absX > absY ? absX : absY;
        if (maxabs > 0.0f)
        {
            move_pos[0] = move_pos[0] * (127.0f / maxabs);
            move_pos[1] = move_pos[1] * (127.0f / maxabs);
        }

        // Floor and flip Y to match usercmd semantics.
        move_pos[0] =  std::floor(move_pos[0]);
        move_pos[1] = -std::floor(move_pos[1]);

        cmd.forwardmove = static_cast<char>(static_cast<int>(move_pos[0]) & 0xFF);
        cmd.rightmove   = static_cast<char>(static_cast<int>(move_pos[1]) & 0xFF);

        // Edge-fire "movedone" notify on arrival.
        if (!g_botai[clientNum].doMove)
        {
            static const auto scr_const_movedone = Scr_AllocString("movedone");
            Scr_Notify(ent, static_cast<unsigned __int16>(scr_const_movedone), 0);
        }
    }

    // Mirror mode: 1:1 copy of another client's last usercmd.
    if (g_botai[clientNum].is_mirroring_client)
    {
        const int mirror_num = g_botai[clientNum].mirror_client_num;
        if (mirror_num >= 0 && mirror_num < MAX_CLIENTS_BW)
        {
            const usercmd_s &last = BW_GetClient(mirror_num)->lastUsercmd;
            cmd.buttons     = last.buttons;
            cmd.angles[0]   = last.angles[0]; // pitch
            cmd.angles[1]   = last.angles[1]; // yaw
            cmd.forwardmove = last.forwardmove;
            cmd.rightmove   = last.rightmove;
        }
    }

    cl->header.deltaMessage = cl->header.netchan.outgoingSequence - 1;
    SV_ClientThink_BW(cl, &cmd);
}

// ---------------------------------------------------------------------------
// SV_UserinfoChanged detour — stamp pending bot name before first parse
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
// r293 DIAGNOSTIC TOGGLES
// ===========================================================================
#define CODXE_DIAG_ENABLE_WEAPON_HOOK         0
#define CODXE_DIAG_ENABLE_USERINFO_HOOK       0   // r339: detour-free
#define CODXE_DIAG_ENABLE_BOTUSERMOVE         0   // r339: detour-free

// r334 — TU7 address dump. Reads raw .text words to xenia.log so the last
// 4 BW symbols can be signature-matched offline. Pure reads, no detour,
// no patch — runs once at module init, before any bot is spawned.
// r338: addresses are finalised in symbols_bw_ext.h — dump disabled.
#define CODXE_DIAG_DUMP4                      0

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
//
// CoD Jumper proves vanilla SV_AddTestClient() works on Xenia. We do the
// same thing — call it directly, capture the returned entity, push it onto
// the GSC stack. The optional name argument is stashed and stamped onto
// the new client's userinfo by our SV_UserinfoChanged_Hook.

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

    DbgPrint("sv_bots: [ADDTESTCLIENT] name='%s'\n", name);

    // Stash the requested name. SV_UserinfoChanged_Hook will stamp it onto
    // the slot's userinfo during the engine's first userinfo-changed call
    // for the new bot.
    std::strncpy(s_pendingBotName, name, sizeof(s_pendingBotName) - 1);
    s_pendingBotName[sizeof(s_pendingBotName) - 1] = '\0';

    gentity_s *ent = SV_AddTestClient();

    s_pendingBotName[0] = '\0';

    if (ent)
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] success, entnum=%d\n", ent->s.number);
        Scr_AddEntityNum(ent->s.number, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        DbgPrint("sv_bots: [ADDTESTCLIENT] vanilla SV_AddTestClient returned NULL\n");
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

// <player> istestclient()  ->  returns 1 if the client is an engine test
// client (bot), else 0.
//
// Fix B: BW's GSC do_isbot() previously read self.pers["isBot"], a GSC-set
// field the engine clears when it rebuilds pers during the connect cycle.
// That made the is_bot() gate in _bot.gsc::connected() reject the bot, so
// teamWatch() never started and the bot sat in spectator. This method asks
// the engine directly via SV_IsTestClient (TU7 0x8221F6D0) — engine truth,
// immune to pers timing. The adapter's do_isbot() should call this.
static void PlayerCmd_IsTestClient(scr_entref_t entref)
{
    if (entref.classnum != 0)
        Scr_ObjectError_BW("not a player entity", SCRIPTINSTANCE_SERVER);
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <player> istestclient()", SCRIPTINSTANCE_SERVER);

    if (entref.entnum < 0 || entref.entnum >= MAX_CLIENTS_BW)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }

    const int result = SV_IsTestClient(static_cast<int>(entref.entnum));
    Scr_AddInt_BW(result != 0 ? 1 : 0, SCRIPTINSTANCE_SERVER);
}

// ---------------------------------------------------------------------------
// Exported lookup tables (called by patched gsc_functions / gsc_client_methods)
// ---------------------------------------------------------------------------

static struct
{
    const char     *name;
    BuiltinFunction handler;
} sv_bots_functions[] = {
    // r338 A/B TEST — addtestclient interception DISABLED.
    //
    // On r261 (no sv_bots.cpp) addtestclient() reached the engine's own GSC
    // builtin and the bot JOINED A TEAM (named "Larry N" by the engine).
    // With BW's GScr_AddTestClient intercepting, the bot is named "BOT1/2"
    // by mod GSC and sits inert in spectator. GScr_AddTestClient only calls
    // the bare SV_AddTestClient() — the engine builtin evidently does more
    // (the connect/team sequence) after that call.
    //
    // Commenting this line out makes codxe's Scr_GetFunction_Hook fall
    // through to the engine builtin — i.e. exactly the r261 code path.
    // If bots now join a team, the fix belongs here (replicate or delegate
    // to the engine builtin). If they still sit in spectator, the team-join
    // was in r261-era mod GSC and the current mod's GSC is the regression.
    //
    // r341 A/B: addtestclient interception DISABLED. With this line
    // commented out, codxe-core's Scr_GetFunction_Hook finds no BW match
    // and falls through to the engine's native addtestclient GSC builtin —
    // exactly the stock r261 path. GScr_AddTestClient is still compiled
    // but no longer referenced; the file-scope #pragma warning(disable:4505)
    // at the top of this file suppresses the resulting C4505 under /WX.
    // { "addtestclient", reinterpret_cast<BuiltinFunction>(GScr_AddTestClient) },
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
    {"istestclient",      PlayerCmd_IsTestClient},
    {nullptr, nullptr},
};

extern "C" BuiltinFunction BW_LookupFunction(const char *name)
{
    // r343: inert EXCEPT kick. r342 proved (screenshot: "Server script
    // compile error / unknown function") that kick MUST be a live BW
    // builtin — bw v11's GSC calls kick() as a bare function and the stock
    // T4 engine exposes no kick GSC builtin. The r342 build kept kick in
    // sv_bots_functions[] but BW_LookupFunction never iterated it, so the
    // lookup returned nullptr and the engine aborted compilation.
    //
    // r343 iterates the table again. The table contains exactly one live
    // entry — { "kick", GScr_Kick } — because the addtestclient row is
    // commented out (it falls through to the engine builtin, the proven
    // r341 path). So this resolves kick and nothing else. BW_LookupMethod
    // stays fully inert. Net effect vs r261: kick is BW-provided (it always
    // was; r261 had no sv_bots.cpp but the engine also had no kick — the
    // mod's kick path is BW's to supply). addtestclient + all methods are
    // engine-routed, exactly r261.
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
    // r342 INERT: BW registers NO player methods. Every method lookup misses
    // and falls through to the engine. bw v11 only calls getentitynumber /
    // getguid / kick from the BW surface on the connect path; with this
    // inert, all of those resolve to the engine, exactly as on r261.
    (void)name;
    (void)sv_bots_methods;
    return nullptr;
}

// ---------------------------------------------------------------------------
// r334 — TU7 address dump (DUMP4)
//
// Resolves the last 4 unresolved TU7 symbols by dumping raw .text memory:
//   SV_IsTestClient          window  0x8221A000 .. 0x82223000
//   Scr_Add* / Scr_Get* run  window  0x82345000 .. 0x8234C000
//
// These run inside the live TU7 process — Xenia has already decrypted the
// base image and applied default_mp.xexp by the time module ctors fire, so
// the bytes read here are the real, final TU7 instruction stream. The
// region is pure executable .text (vaddr base 0x820D0000+), so reads cannot
// fault. No detour and no patch is installed; this is read-only.
//
// Output line format (6 words per line, address-prefixed, stride 0x18):
//   sv_bots: DUMP4 <addr>: w0 w1 w2 w3 w4 w5
//
// r333 used 8 words/line and stride 0x20. T4's DbgPrint is fixed-arity and
// only reads the PowerPC register window (r3..r10 = 8 args); words 6 and 7
// were arg 9/10, spilled to stack, and printed as stale garbage. r334 uses
// 6 words (7 args incl. addr) so every word is a real in-register read, and
// stride 0x18 makes the dump fully contiguous with no 8-byte gap per row.
//
// NOTE: base->TU7 function ordering is NOT preserved (proven from the r333
// Scr_Add* family — TU7 reverses AddInt/AddUndefined vs base). Match each
// function by full-body signature, never by relative position.
// ---------------------------------------------------------------------------

static void BW_DumpRegion(unsigned int start, unsigned int end, const char *tag)
{
    DbgPrint("sv_bots: DUMP4 BEGIN %s [%08X .. %08X]\n", tag, start, end);

    // r334: 6 words per line, stride 0x18 — fully contiguous, no gaps.
    // The DbgPrint call passes 7 args (addr + 6 words); PowerPC passes the
    // first 8 integer args in r3..r10, so every argument stays in-register.
    // r333 used 8 words / 9 args — args 9 and 10 spilled to stack and
    // T4's fixed-arity DbgPrint printed stale garbage for words 6 and 7.
    for (unsigned int addr = start; addr < end; addr += 0x18)
    {
        const volatile unsigned int *p =
            reinterpret_cast<const volatile unsigned int *>(addr);

        DbgPrint("sv_bots: DUMP4 %08X: %08X %08X %08X %08X %08X %08X\n",
                 addr,
                 p[0], p[1], p[2], p[3], p[4], p[5]);
    }

    DbgPrint("sv_bots: DUMP4 END %s\n", tag);
}

static void BW_DumpRegions()
{
    DbgPrint("sv_bots: DUMP4 ==== r334 TU7 address dump start ====\n");
    // SV_IsTestClient: r333 window topped out at 0x82221000 and the
    // function (base 0x8221D1E0 + ~+0x3E20 SV-region delta) sat at/past
    // that edge. Widened the top to 0x82223000.
    BW_DumpRegion(0x8221A000, 0x82223000, "SV_ISTESTCLIENT");
    BW_DumpRegion(0x82345000, 0x8234C000, "SCR_FAMILY");
    DbgPrint("sv_bots: DUMP4 ==== r334 TU7 address dump end ====\n");
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

sv_bots::sv_bots()
{
    DbgPrint("sv_bots: r343 INERT-EXCEPT-KICK build (kick registered; methods+addtestclient engine-routed)\n");
    DbgPrint("sv_bots: [DIAG] r343 | sv_bots=KICK-ONLY | weapon=%d userinfo=%d botmove=%d\n",
             CODXE_DIAG_ENABLE_WEAPON_HOOK,
             CODXE_DIAG_ENABLE_USERINFO_HOOK,
             CODXE_DIAG_ENABLE_BOTUSERMOVE);

#if CODXE_DIAG_DUMP4
    BW_DumpRegions();
#endif

    CleanBotArray();
    s_pendingBotName[0] = '\0';

#if CODXE_DIAG_ENABLE_WEAPON_HOOK
    G_SelectWeaponIndex_Detour = Detour(G_SelectWeaponIndex, G_SelectWeaponIndex_Hook);
    G_SelectWeaponIndex_Detour.Install();
    DbgPrint("sv_bots: G_SelectWeaponIndex detour INSTALLED\n");
#else
    DbgPrint("sv_bots: G_SelectWeaponIndex detour SKIPPED (diagnostic)\n");
#endif

#if CODXE_DIAG_ENABLE_BOTUSERMOVE
    SV_BotUserMove_Detour = Detour(SV_BotUserMove, SV_BotUserMove_Stub);
    SV_BotUserMove_Detour.Install();
    DbgPrint("sv_bots: SV_BotUserMove detour INSTALLED\n");
#else
    DbgPrint("sv_bots: SV_BotUserMove detour SKIPPED (diagnostic)\n");
#endif

#if CODXE_DIAG_ENABLE_USERINFO_HOOK
    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    SV_UserinfoChanged_Detour.Install();
    DbgPrint("sv_bots: SV_UserinfoChanged detour INSTALLED\n");
#else
    DbgPrint("sv_bots: SV_UserinfoChanged detour SKIPPED (diagnostic)\n");
#endif

    // SV_CalcPings deliberately NOT detoured — see file header.
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: removing T4 BW detours\n");

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
