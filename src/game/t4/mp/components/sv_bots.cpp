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
//   it directly, the same way CoD Jumper's GSC does. The Path C bypass
//   from r292 is removed — it was solving a problem that didn't exist
//   in practice (the NET_CompareBaseAdr fall-through is real in the
//   disassembly but doesn't manifest as host corruption on Xenia).
//
// r299 — angle preservation + host-leak diagnostic:
//
//   BW_R299_FIX_BOT_ANGLES: copy cl->lastUsercmd.angles into the
//   synthesized usercmd. Matches CoD4x SV_BotUserMove pattern. Without
//   this, bots aim at world-origin (0,0,0) every tick because the cmd
//   was memset-zeroed and never had angles assigned in the normal path.
//
//   BW_R299_DIAG_LOG_ENTRY: rate-limited log line on every stub entry
//   to confirm/refute "host slips through guards" theory on Xenia.
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
    // r297: raw forward/right components written by Scr_BotMovement.
    // BW's PT4-style doBotMovement_loop computes dir[0]/dir[1] every 50ms
    // and calls BotBuiltinBotMovement(forward, right). With doMove unset
    // (BW never calls botMoveTo), SV_BotUserMove_Stub left forwardmove
    // and rightmove at zero and bots stood still. These two fields let
    // the stub honor BW's existing computation without changing GSC.
    signed char   forwardMove;
    signed char   rightMove;
    int           hasRawMove;  // 1 if forward/right were set this tick
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
    {"gostand",     KEY_GOSTAND},
    {"gocrouch",    KEY_CROUCH},
    {"crouch",      KEY_CROUCH},    // r297: PT4 BW alias for "gocrouch"
    {"goprone",     KEY_PRONE},
    {"prone",       KEY_PRONE},     // r297: PT4 BW alias for "goprone"
    {"fire",        KEY_FIRE},
    {"attack",      KEY_FIRE},      // r297: PT4 BW alias for "fire"
                                    //       (IW3 BW used "fire", PT4 uses "attack")
    {"melee",       KEY_MELEE},
    {"frag",        KEY_FRAG},
    {"smoke",       KEY_SMOKE},
    {"reload",      KEY_RELOAD},
    {"sprint",      KEY_SPRINT},
    {"leanleft",    KEY_LEANLEFT},
    {"leanright",   KEY_LEANRIGHT},
    {"ads",         KEY_ADSMODE | KEY_ADS},
    {"speed_throw", KEY_ADSMODE | KEY_ADS},  // r297: PT4 BW alias for "ads"
                                             //       (used while throwing grenades)
    {"holdbreath",  KEY_HOLDBREATH},
    {"activate",    KEY_USE},
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
    // r294: trace every weapon-select call so we can correlate engine activity
    // with what GSC's getcurrentweapon() sees a moment later. We still write
    // g_botai[].weapon as a fallback cache, but cmd.weapon now reads directly
    // from the gentity (see SV_BotUserMove_Stub) — this hook is informational.
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

static Detour SV_BotUserMove_Detour;

// r296: per-client rate-limit state for the gentity-weapon trace.
// Initialised to "no log yet" sentinel (-1 weapon value, 0 ms).
static int s_lastWeaponLogMs[MAX_CLIENTS_BW]  = {0};
static int s_lastWeaponLogVal[MAX_CLIENTS_BW] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                                  -1, -1, -1, -1, -1, -1, -1, -1,
                                                  -1, -1};

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

    // [r299] DIAG: log every entry, rate-limited per client. Confirms or
    // refutes "host slips through guards as NA_BOT" theory on Xenia. If
    // we see cn=0 (host) here, the outer caller (SV_UpdateBots or
    // equivalent on T4) is iterating the host. The guards below should
    // still early-return for it (NA_BOT + isTest checks), but presence
    // alone is the signal we want.
    {
        static int s_lastEntryLogMs[MAX_CLIENTS_BW] = {0};
        const int now = svsHeader->time;
        if ((now - s_lastEntryLogMs[clientNum]) > 2000)
        {
            s_lastEntryLogMs[clientNum] = now;
            DbgPrint("sv_bots: BUM_Stub entry cn=%d remAdr.type=%d isTest=%d state=%d\n",
                     clientNum,
                     static_cast<int>(cl->header.netchan.remoteAddress.type),
                     static_cast<int>(cl->isTestClient),
                     static_cast<int>(cl->header.state));
        }
    }

    // r307: state guard for zombie/free slots.
    // PROVEN BUG (r306b kick diagnostic, 2026-05-24):
    //   SV_DropClient correctly set cl->state = CS_ZOMBIE on kicked bot.
    //   But our stub kept injecting input every frame because the guards
    //   below only check NA_BOT and isTestClient — both still true on a
    //   zombie. Engine sees inputs flowing → never transitions zombie to
    //   CS_FREE. Bot stuck forever. Kick "doesn't work" symptom.
    //
    // Fix: skip the stub entirely if not CS_ACTIVE. We DON'T fall through
    // to the engine path for non-active either — zombies should be left
    // alone by the bot-input path so SV_CheckTimeouts can reap them.
    if (cl->header.state != CS_ACTIVE)
    {
        return;  // no input for non-active clients; do not call the engine path
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
    // returns and what the player struct considers held. Caching via
    // G_SelectWeaponIndex_Hook turned out to be wrong for "current
    // weapon" because the engine only calls G_SelectWeaponIndex on
    // initial spawn for an offhand slot — bots held grenades until they
    // fired. Reading the gentity each tick fixes it.
    const int gentityWeapon = cl->gentity->s.weapon;
    cmd.weapon = static_cast<unsigned __int8>(gentityWeapon & 0xFF);

    // Rate-limited trace so the log isn't flooded: only when the value
    // changes per client, and at most once per 500 ms.
    if (s_lastWeaponLogVal[clientNum] != gentityWeapon &&
        (svsHeader->time - s_lastWeaponLogMs[clientNum]) > 500)
    {
        DbgPrint("sv_bots: bot weapon clientNum=%d gentity.s.weapon=%d (was %d)\n",
                 clientNum, gentityWeapon, s_lastWeaponLogVal[clientNum]);
        s_lastWeaponLogVal[clientNum] = gentityWeapon;
        s_lastWeaponLogMs[clientNum]  = svsHeader->time;
    }

    cmd.buttons = static_cast<button_mask>(g_botai[clientNum].buttons);

    // [r299] Preserve the bot's current usercmd angles. Without this,
    // memset()-zeroed angles get processed by SV_ClientThink, which
    // writes them into ps.viewangles, snapping the bot's aim to
    // (0, 0, 0) every tick — bullet traces fire toward world origin
    // and miss everything. CoD4x's SV_BotUserMove does the equivalent
    // via ent->client->sess.cmd.angles; clientBW_t.lastUsercmd
    // (verified at +0x20EF4 in structs_bw_ext.h) is the same data
    // from a different struct path. Format is packed-16-bit-in-int
    // (CoD4x usercmd_s.angles is `int angles[3]`, PACKED_ANGLE).
    cmd.angles[0] = cl->lastUsercmd.angles[0];
    cmd.angles[1] = cl->lastUsercmd.angles[1];
    cmd.angles[2] = cl->lastUsercmd.angles[2];

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
    else if (g_botai[clientNum].hasRawMove)
    {
        // r297: PT4 BW's doBotMovement_loop computes forward/right in bot
        // local frame every 50ms and pushes it through Scr_BotMovement.
        // The values were already projected into bot space and clamped to
        // [-127, 127] by GSC, so apply verbatim.
        cmd.forwardmove = g_botai[clientNum].forwardMove;
        cmd.rightmove   = g_botai[clientNum].rightMove;
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

// r297: raw forward/right input from GSC. T4 BW's doBotMovement_loop in
// _bot_internal.gsc computes a 2D direction every 50ms and calls
// self botmovement(int(dir[0]), int(dir[1])). Without this method bound,
// BW's adapter would no-op the call and bots would stand still.
//
// Inputs are signed bytes in [-127, 127]. Bot's local frame: +forward
// moves the bot in the direction it's facing, +right strafes right.
static void Scr_BotMovement(scr_entref_t entref)
{
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error_BW("Usage: <bot> botMovement(<forward>, <right>);", SCRIPTINSTANCE_SERVER);

    int forward = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    int right   = Scr_GetInt_BW(1, SCRIPTINSTANCE_SERVER);

    // Clamp to signed-byte range (engine usercmd uses signed char).
    if (forward >  127) forward =  127;
    if (forward < -127) forward = -127;
    if (right   >  127) right   =  127;
    if (right   < -127) right   = -127;

    g_botai[entref.entnum].forwardMove = static_cast<signed char>(forward);
    g_botai[entref.entnum].rightMove   = static_cast<signed char>(right);
    g_botai[entref.entnum].hasRawMove  = 1;
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
    g_botai[entref.entnum].hasRawMove          = 0;
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
    // r306: full diagnostic trace. We've shipped three failed fixes on kick;
    // the only honest path forward is to log every step and see what's
    // actually broken. Each DbgPrint is gated nowhere — we want them ALL on
    // a kick attempt so the log line sequence tells us where the chain
    // breaks. After kick works, these can be removed.
    DbgPrint("sv_bots: [KICK] handler entered\n");

    const int nparam = Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER);
    DbgPrint("sv_bots: [KICK] nparam=%d\n", nparam);

    if (nparam < 1 || nparam > 2)
    {
        DbgPrint("sv_bots: [KICK] BAD nparam — calling Scr_Error_BW\n");
        Scr_Error_BW("Usage: kick(<clientNum>) or kick(<clientNum>, <reason>)",
                     SCRIPTINSTANCE_SERVER);
        return;
    }

    const int clientNum = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    DbgPrint("sv_bots: [KICK] clientNum=%d\n", clientNum);

    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
    {
        DbgPrint("sv_bots: [KICK] clientNum %d out of range (MAX=%d)\n",
                 clientNum, MAX_CLIENTS_BW);
        Scr_ParamError_BW(0, va_BW("kick: clientNum %i out of range", clientNum),
                          SCRIPTINSTANCE_SERVER);
        return;
    }

    const char *reason = "EXE_PLAYERKICKED";
    if (nparam == 2)
    {
        const char *r = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
        DbgPrint("sv_bots: [KICK] reason arg = %s\n", r ? r : "(null)");
        if (r && *r) reason = r;
    }
    DbgPrint("sv_bots: [KICK] final reason='%s'\n", reason);

    clientBW_t *cl = BW_GetClient(clientNum);
    DbgPrint("sv_bots: [KICK] BW_GetClient(%d) = %p\n", clientNum, (void *)cl);

    if (!cl)
    {
        DbgPrint("sv_bots: [KICK] NULL client — abort\n");
        return;
    }

    DbgPrint("sv_bots: [KICK] cl->header.state = %d (CS_CONNECTED=%d)\n",
             cl->header.state, CS_CONNECTED);

    if (cl->header.state < CS_CONNECTED)
    {
        DbgPrint("sv_bots: [KICK] state < CONNECTED — abort drop\n");
        return;
    }

    // r306b: SV_DropClient declared in symbols_bw_ext.h (r335 TU7 audit) at
    // 0x82283BF0. Address Ghidra-fingerprinted today against EXE_PLAYERKICKED
    // xref chain: state==CS_ZOMBIE guard, name read from cl+0x21328,
    // clientNum via stride 0xB762C, sets state=CS_ZOMBIE before return.
    // If kick still doesn't work after this build, the bug is upstream of
    // the call (struct offsets, cl pointer, gsc params) or downstream
    // (game-loop client cleanup not running, not a wrong-address problem).
    DbgPrint("sv_bots: [KICK] >>> calling SV_DropClient(cl=%p, '%s', true)\n",
             (void *)cl, reason);
    SV_DropClient(cl, reason, true);
    DbgPrint("sv_bots: [KICK] <<< SV_DropClient returned\n");
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

// r304: register jumpbuttonpressed() as a player method.
// T4 TU7 does NOT expose jumpbuttonpressed natively (absent from all 6
// PLAYER_METHODS sub-tables — verified via namespace dump). BW's _menu.gsc
// MenuSelect() waits on self jumpbuttonpressed() which silently no-ops
// without this, so menu select never fires — kick (and every other menu
// confirm) silently does nothing.
//
// Implementation mirrors codjumper-iw3's PlayerCmd_JumpButtonPressed:
// read player's current + just-pressed buttons, AND with the jump bit.
// On T4 there is no KEY_JUMP — jump maps to KEY_GOSTAND (0x400).
static void PlayerCmd_JumpButtonPressed(scr_entref_t entref)
{
    gentity_s *ent = BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <player> jumpbuttonpressed()", SCRIPTINSTANCE_SERVER);

    if (!ent->client)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }

    const int combined = ent->client->buttons | ent->client->buttonsSinceLastFrame;
    Scr_AddInt_BW((combined & KEY_GOSTAND) != 0 ? 1 : 0, SCRIPTINSTANCE_SERVER);
}

// r308: istestclient — engine-native bot detection.
// Reads cl->isTestClient via SV_IsTestClient(clientNum). bw11's
// `do_isbot` fallback chain checks this when self.pers["isBot"] isn't set
// (which happens if PlayerConnect fires before userinfo is parsed, so the
// "Larry" name-prefix branch never matched). Mirrors what CoD4x IW3 BW
// achieves with `return self.isbot;` — same outcome, different access path.
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
    // r294: "addtestclient" deliberately omitted — BW_LookupFunction
    // routes it to a fall-through so the engine's stock GScr_AddTestClient
    // handles bot spawn. Our GScr_AddTestClient is kept in this file
    // (further up) only as reference; nothing dispatches to it. See
    // BW_LookupFunction above for the routing.
    {"kick",          reinterpret_cast<BuiltinFunction>(GScr_Kick)},
    {nullptr, nullptr},
};

static struct
{
    const char   *name;
    BuiltinMethod handler;
} sv_bots_methods[] = {
    {"botmoveto",          Scr_BotMoveTo},
    {"botmovement",        Scr_BotMovement},
    {"botaction",          Scr_BotAction},
    {"botmirror",          Scr_BotMirror},
    {"botstop",            Scr_BotStop},
    {"getentitynumber",    PlayerCmd_GetEntityNumber},
    {"getguid",            PlayerCmd_GetGuid},
    {"jumpbuttonpressed",  PlayerCmd_JumpButtonPressed},  // r304: needed for BW menu select
    {"istestclient",       PlayerCmd_IsTestClient},        // r308: engine bot detection
    {nullptr, nullptr},
};

extern "C" BuiltinFunction BW_LookupFunction(const char *name)
{
    if (!name)
        return nullptr;

    // r294: route "addtestclient" to engine native. Our GScr_AddTestClient
    // doesn't drive ClientBegin, so test clients connected but never picked
    // a class — they stayed in spectators while the engine's stock spawn
    // path runs the full ClientBegin chain. Falling through here lets the
    // stock GScr_AddTestClient handle the call. The wider effect: bots
    // spawn with engine-assigned classes, ranks, and primaries.
    if (_stricmp(name, "addtestclient") == 0)
    {
        DbgPrint("sv_bots: [HOOK] 'addtestclient' -> fall through to engine\n");
        return nullptr;
    }

    for (const auto *f = sv_bots_functions; f->name != nullptr; ++f)
    {
        if (_stricmp(name, f->name) == 0)
        {
            // Heartbeat trace: first GSC use of any BW function tells us
            // the dispatch chain is alive. Cheap; one log line per name.
            DbgPrint("sv_bots: [HOOK] '%s' -> BW port handler\n", name);
            return f->handler;
        }
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
    DbgPrint("sv_bots: installing T4 BW detours (r308 istestclient-method)\n");
    DbgPrint("sv_bots: [DIAG] weapon=%d userinfo=%d botmove=%d\n",
             CODXE_DIAG_ENABLE_WEAPON_HOOK,
             CODXE_DIAG_ENABLE_USERINFO_HOOK,
             CODXE_DIAG_ENABLE_BOTUSERMOVE);

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
