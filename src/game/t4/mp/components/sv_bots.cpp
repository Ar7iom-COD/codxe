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
// r293 — vanilla bot spawn:
//
//   CoD Jumper proves vanilla SV_AddTestClient() works on Xenia. We call
//   it directly. The Path C bypass from r292 is removed.
//
// r295 — weapon hook enabled (superseded by r296):
//
//   G_SelectWeaponIndex_Hook captures weapon-select calls but the engine
//   only calls it once per client with an offhand slot. Cached value was
//   wrong for "current weapon".
//
// r296 — read engine truth directly:
//
//   cmd.weapon now comes from cl->gentity->s.weapon every tick. Same field
//   GSC getcurrentweapon() reads. Bots visibly hold their primaries.
//
// r299 — T4 console-name aliases for botAction:
//
//   Bot Warfare's GSC uses different action strings on T4 vs IW3:
//     IW3 BW: "+fire", "+ads",         "+gocrouch", "+goprone"
//     T4  BW: "+attack", "+speed_throw", "+crouch",   "+prone"
//
//   The PT4 upstream and the deployed T4 mod both use T4's native console
//   bind names (the strings the engine itself recognises in /bind), which
//   is the correct local idiom for T4. Our Scr_BotAction was a direct port
//   of the IW3 codxe table, which carries IW3-localized BW names. Strings
//   that didn't match raised Scr_ParamError silently — the button bit was
//   never set and BW never knew its call failed.
//
//   Net symptom: bots knife at close range (which uses "+melee", in both
//   tables) and jump (which uses "+gostand", in both tables) but never
//   fire, never ADS, never change stance via "+crouch"/"+prone". Fixed by
//   adding the four T4 names as aliases that map to the same bit values
//   as their IW3 counterparts.
//

// r298 — fs_* GSC builtins added for waypoint CSV loading:
//
//   On stock codxe T4 the fs_testfile / fs_fopen / fs_fclose / fs_readline /
//   fs_writeline GSC builtins didn't exist — these are codxe IW3 additions
//   (gsc_functions.cpp on the IW3 side). Bot Warfare's _bot_utility.gsc
//   waypoint loader is built around them: readWpsFromFile() asks
//   BotBuiltinFileExists("waypoints/<map>_wp.csv") first, then walks the
//   file line-by-line via BotBuiltinReadLine. With no engine-side
//   implementation, the deployed T4 adapter had to stub everything to
//   false/-1/"" — meaning level.waypoints stayed empty no matter what was
//   sitting in scriptdata/waypoints/. BW's combat AI ran (the bots would
//   knife you up close, the engine's class loadouts were assigned), but
//   the navigation graph was empty and _bot_internal::astar_move had
//   nothing to drive cmd.forwardmove / cmd.rightmove against, so the bots
//   stood still.
//
//   r298 lifts the IW3 implementation directly: 8 FILE* slots in a static
//   table, <cstdio> stdio, paths joined under Config::GetModBasePath().
//   The deployed adapter prefixes "scriptdata/" before calling these, so
//   relative paths land at e.g. _codxe\mods\bot_warfare\scriptdata\
//   waypoints\mp_asylum_wp.csv — exactly where the CSVs already ship.
//
//   No detour. Pure GSC builtin registration via sv_bots_functions[]; the
//   Scr_GetFunction_Hook in gsc_functions.cpp routes these into us before
//   falling through to the engine. T4 MP has no Events::OnVMShutdown hook
//   (unlike IW3), so handles are flushed in sv_bots::~sv_bots() instead.
//

// r297 — BW drives all input, engine fallback removed:
//
//   The r294 "Path 2-light" walk-forward + KEY_FIRE-pulse default was a
//   diagnostic to prove the SV_BotUserMove -> SV_ClientThink path was
//   live. It worked too well: bots literally walked forward and fired
//   into the air, looking like broken AI when in fact they had NO AI —
//   our fallback was their only input. That fallback is now removed.
//
//   When no GSC AI is driving a bot (g_botai[].doMove == 0 and not
//   mirroring), cmd stays at default (zero forwardmove/rightmove, zero
//   buttons). The bot stands still until BW's _bot_script::connected
//   threads its waypoint navigation and target acquisition and starts
//   calling botMoveTo / botAction +fire through Scr_BotMoveTo and
//   Scr_BotAction. Standing still is the correct idle behavior.
//
//   This relies on _callbacksetup.gsc r297 stamping pers["isBot"] = true
//   for engine-spawned test clients so BW's _bot.gsc::onPlayerConnect
//   actually recognises them and threads its AI tree.
//

#include "pch.h"
#include "sv_bots.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
    // IW3-style names (kept so this table stays a one-to-one port of the
    // upstream IW3 codxe BotActions[], and any IW3 GSC script ported in
    // continues to work as-is).
    {"gostand",     KEY_GOSTAND},
    {"gocrouch",    KEY_CROUCH},
    {"goprone",     KEY_PRONE},
    {"fire",        KEY_FIRE},
    {"melee",       KEY_MELEE},
    {"frag",        KEY_FRAG},
    {"smoke",       KEY_SMOKE},
    {"reload",      KEY_RELOAD},
    {"sprint",      KEY_SPRINT},
    {"leanleft",    KEY_LEANLEFT},
    {"leanright",   KEY_LEANRIGHT},
    {"ads",         KEY_ADSMODE | KEY_ADS},
    {"holdbreath",  KEY_HOLDBREATH},
    {"activate",    KEY_USE},

    // r299: T4 console-name aliases. Deployed T4 BW GSC (and the upstream
    // PT4 PC source it was derived from) uses these instead of the IW3
    // names. Same bit values as their IW3 counterparts above.
    {"attack",      KEY_FIRE},                  // alias of "fire"
    {"speed_throw", KEY_ADSMODE | KEY_ADS},     // alias of "ads"
    {"crouch",      KEY_CROUCH},                // alias of "gocrouch"
    {"prone",       KEY_PRONE},                 // alias of "goprone"
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
// G_SelectWeaponIndex detour — diagnostic only
// ---------------------------------------------------------------------------
//
// Kept installed for trace value but no longer drives anything. cmd.weapon
// reads cl->gentity->s.weapon directly in the stub. Rate-limited DbgPrint
// fires only when the value actually changes per client.

static Detour G_SelectWeaponIndex_Detour;

static void G_SelectWeaponIndex_Hook(int clientNum, int iWeaponIndex)
{
    if (clientNum >= 0 && clientNum < MAX_CLIENTS_BW)
    {
        const unsigned char prev = g_botai[clientNum].weapon;
        const unsigned char next = static_cast<unsigned char>(iWeaponIndex);
        g_botai[clientNum].weapon = next;

        if (prev != next)
        {
            DbgPrint("sv_bots: G_SelectWeaponIndex clientNum=%d weaponIndex=%d (prev=%d)\n",
                     clientNum, iWeaponIndex, static_cast<int>(prev));
        }
    }

    G_SelectWeaponIndex_Detour.GetOriginal<G_SelectWeaponIndex_t>()(clientNum, iWeaponIndex);
}

// ---------------------------------------------------------------------------
// SV_BotUserMove detour — core bot driver
// ---------------------------------------------------------------------------
//
// r297: walk-forward+fire fallback removed. When BW isn't driving the bot
// (doMove == 0, not mirroring), cmd stays zeroed and the bot stands still.
// BW's _bot_script::connected drives botMoveTo / botAction through GSC,
// which writes into g_botai[] via Scr_BotMoveTo / Scr_BotAction.

static Detour SV_BotUserMove_Detour;

// Rate-limit the weapon trace DbgPrint so each bot reports its current
// gentity weapon at most once per change.
static int s_lastWeaponLogMs[MAX_CLIENTS_BW] = {0};
static int s_lastWeaponLogVal[MAX_CLIENTS_BW] = {-1};

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

    // r296: read the engine's authoritative current weapon directly from
    // the gentity's entityState. This is what GSC's getcurrentweapon()
    // returns and what the player struct considers held.
    const int gentityWeapon = cl->gentity->s.weapon;
    cmd.weapon = static_cast<unsigned __int8>(gentityWeapon & 0xFF);

    // Trace at most once per second per client, and only when the value
    // changes, so the log isn't flooded.
    if (s_lastWeaponLogVal[clientNum] != gentityWeapon &&
        (svsHeader->time - s_lastWeaponLogMs[clientNum]) > 500)
    {
        DbgPrint("sv_bots: bot weapon clientNum=%d gentity.s.weapon=%d (was %d)\n",
                 clientNum, gentityWeapon, s_lastWeaponLogVal[clientNum]);
        s_lastWeaponLogVal[clientNum] = gentityWeapon;
        s_lastWeaponLogMs[clientNum]  = svsHeader->time;
    }

    // Buttons come from BW (Scr_BotAction writes here). Zero until BW drives.
    cmd.buttons = static_cast<button_mask>(g_botai[clientNum].buttons);

    // r297: when BW is driving via botMoveTo, compute the local-frame move
    // vector toward the target and stamp forwardmove/rightmove. When not
    // driving, cmd.forwardmove and cmd.rightmove stay zero (memset above)
    // and the bot stands still — engine renders idle, no random forward
    // movement, no fire-into-the-air. Standing still is the correct idle
    // state.
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
// r297 DIAGNOSTIC TOGGLES
// ===========================================================================
#define CODXE_DIAG_ENABLE_WEAPON_HOOK         1
#define CODXE_DIAG_ENABLE_USERINFO_HOOK       1
#define CODXE_DIAG_ENABLE_BOTUSERMOVE         1

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

// ===========================================================================
// fs_* GSC builtins (r298)
//
// Ports codxe IW3's gsc_functions.cpp fs_* family to T4 MP. Used by BW's
// scripts/mp/bots_adapter_pt4.gsc do_fs_* functions, which BW's
// _bot_utility.gsc routes BotBuiltinFileExists / BotBuiltinFileOpen / etc.
// through. The deployed adapter prefixes paths with "scriptdata/" so a BW
// call for "waypoints/mp_asylum_wp.csv" lands here as
// "scriptdata/waypoints/mp_asylum_wp.csv" and joins under
// Config::GetModBasePath() to the on-disk mod folder.
//
// 8 slots is the IW3 cap; BW never holds more than one at a time, and the
// w/p_editor menu (host-only) reaches 2. Slot 0 reserved (handle 0 == fail
// per IW3 convention).
// ===========================================================================

namespace
{

constexpr int MAX_SCRIPT_FILEHANDLES = 8;

struct ScriptFileHandle_t
{
    FILE *fh;
    char  filename[256];
};

static ScriptFileHandle_t s_scriptFiles[MAX_SCRIPT_FILEHANDLES];

static std::string BuildScriptFilePath(const char *filename)
{
    if (!filename || !*filename)
        return std::string();

    // Already absolute / virtual path? Don't second-guess it.
    if ((filename[0] && filename[1] == ':') || std::strncmp(filename, "game:\\", 6) == 0)
        return filename;

    std::string base = Config::GetModBasePath();
    if (base.empty())
        return filename;

    std::string rel(filename);
    for (size_t i = 0; i < rel.size(); ++i)
        if (rel[i] == '/')
            rel[i] = '\\';

    return base + "\\" + rel;
}

static void CloseAllScriptFiles()
{
    for (int i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (s_scriptFiles[i].fh)
        {
            std::fclose(s_scriptFiles[i].fh);
            std::memset(&s_scriptFiles[i], 0, sizeof(ScriptFileHandle_t));
        }
    }
}

} // anonymous namespace

static void GScr_FS_TestFile()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_testfile(<filename>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    std::string fullpath = BuildScriptFilePath(filename);

    FILE *f = std::fopen(fullpath.c_str(), "r");
    if (f)
    {
        std::fclose(f);
        Scr_AddInt_BW(1, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
    }
}

static void GScr_FS_FOpen()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error_BW("Usage: fs_fopen(<filename>, <mode>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    const char *modeStr  = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
    const char *fmode;

    if (modeStr && _stricmp(modeStr, "read") == 0)
        fmode = "rt";
    else if (modeStr && _stricmp(modeStr, "write") == 0)
        fmode = "wt";
    else if (modeStr && _stricmp(modeStr, "append") == 0)
        fmode = "at";
    else
    {
        Scr_Error_BW("fs_fopen: invalid mode. Valid modes are: read, write, append",
                     SCRIPTINSTANCE_SERVER);
        return;
    }

    std::string fullpath = BuildScriptFilePath(filename);

    // Write/append: don't bother creating parent directories — codxe T4's
    // filesystem helpers aren't exposed here and BW only ever writes into
    // pre-existing scriptdata/ (which is guaranteed to exist because GSC
    // loaded from it). If the host wants a new folder they make it manually.

    for (int i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (!s_scriptFiles[i].fh)
        {
            s_scriptFiles[i].fh = std::fopen(fullpath.c_str(), fmode);
            if (!s_scriptFiles[i].fh)
            {
                Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
                return;
            }
            std::strncpy(s_scriptFiles[i].filename, filename ? filename : "",
                         sizeof(s_scriptFiles[i].filename) - 1);
            s_scriptFiles[i].filename[sizeof(s_scriptFiles[i].filename) - 1] = '\0';
            Scr_AddInt_BW(i + 1, SCRIPTINSTANCE_SERVER);
            return;
        }
    }

    Scr_Error_BW("fs_fopen: exceeded maximum open file handles", SCRIPTINSTANCE_SERVER);
}

static void GScr_FS_FClose()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_fclose(<filehandle>)", SCRIPTINSTANCE_SERVER);

    const int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_fclose: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (slot.fh)
    {
        std::fclose(slot.fh);
        std::memset(&slot, 0, sizeof(ScriptFileHandle_t));
    }
}

static void GScr_FS_ReadLine()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_readline(<filehandle>)", SCRIPTINSTANCE_SERVER);

    const int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_readline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error_BW("fs_readline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    char buffer[8192];
    if (!std::fgets(buffer, sizeof(buffer), slot.fh))
    {
        Scr_AddUndefined_BW(SCRIPTINSTANCE_SERVER);
        return;
    }

    // Strip trailing CR/LF in either order (CRLF, LF, or bare CR).
    int len = static_cast<int>(std::strlen(buffer));
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
    {
        buffer[--len] = '\0';
    }

    Scr_AddString(buffer, SCRIPTINSTANCE_SERVER);
}

static void GScr_FS_WriteLine()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error_BW("Usage: fs_writeline(<filehandle>, <data>)", SCRIPTINSTANCE_SERVER);

    const int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_writeline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error_BW("fs_writeline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    const char *data = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
    if (!data) data = "";
    if (std::fprintf(slot.fh, "%s\n", data) < 0)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }

    Scr_AddInt_BW(1, SCRIPTINSTANCE_SERVER);
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
// Exported lookup tables (called by patched gsc_functions / gsc_client_methods)
// ---------------------------------------------------------------------------

static struct
{
    const char     *name;
    BuiltinFunction handler;
} sv_bots_functions[] = {
    // r294: addtestclient falls through to the engine native builtin.
    // GScr_AddTestClient is incomplete — engine native drives the client
    // through ClientBegin, raises "begin", completes Callback_PlayerConnect,
    // which is what gets bots onto teams with ranks. The function body is
    // kept compiled (warning 4505 disabled at file scope) so the revert is
    // one line to undo.
    // {"addtestclient", reinterpret_cast<BuiltinFunction>(GScr_AddTestClient)},
    {"kick",          reinterpret_cast<BuiltinFunction>(GScr_Kick)},

    // r298: file I/O for waypoint CSVs (and the in-game wp_editor save path).
    // Paths land under Config::GetModBasePath() after the adapter prefixes
    // "scriptdata/" — see do_fs_* in scripts/mp/bots_adapter_pt4.gsc.
    {"fs_testfile",   reinterpret_cast<BuiltinFunction>(GScr_FS_TestFile)},
    {"fs_fopen",      reinterpret_cast<BuiltinFunction>(GScr_FS_FOpen)},
    {"fs_fclose",     reinterpret_cast<BuiltinFunction>(GScr_FS_FClose)},
    {"fs_readline",   reinterpret_cast<BuiltinFunction>(GScr_FS_ReadLine)},
    {"fs_writeline",  reinterpret_cast<BuiltinFunction>(GScr_FS_WriteLine)},

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
    DbgPrint("sv_bots: installing T4 BW detours (r299 t4-actions)\n");
    DbgPrint("sv_bots: [DIAG] weapon=%d userinfo=%d botmove=%d\n",
             CODXE_DIAG_ENABLE_WEAPON_HOOK,
             CODXE_DIAG_ENABLE_USERINFO_HOOK,
             CODXE_DIAG_ENABLE_BOTUSERMOVE);

    CleanBotArray();
    s_pendingBotName[0] = '\0';

    for (int i = 0; i < MAX_CLIENTS_BW; ++i)
    {
        s_lastWeaponLogMs[i]  = 0;
        s_lastWeaponLogVal[i] = -1;
    }

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

    // r298: flush any GSC-opened file handles. T4 MP has no
    // Events::OnVMShutdown subsystem (unlike IW3), so this runs only on
    // plugin teardown rather than per-VM-restart. Acceptable: handles
    // are bounded at 8 and BW always pairs fopen/fclose in load_waypoints.
    CloseAllScriptFiles();
}

} // namespace mp
} // namespace t4
