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

// Engine Com_Printf @ 0x82271C60. Full detourable prologue (the deeper
// sink 0x82271AE8 is an 8-byte __savegprlr stub and cannot be detoured).
// r3 = channel, r4 = format string. Every console line — including the
// GSC compiler's "unknown function" error — passes through here.
// The hook logs the format string raw; for engine error messages the
// text is already substituted into the format string by upstream va().
static void* const CODXE_COM_PRINTF_ADDR = reinterpret_cast<void*>(0x82271C60);
typedef void (*Com_Printf_t)(int, const char*, ...);

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
    bool          is_bot;   // set by GScr_AddTestClient; OUR bot flag.
                            // SV_IsTestClient is unreliable here -
                            // it reads gclient_s+0x39BC which is not
                            // synced with the client_t flag SV_AddTestClient
                            // sets, so it returns 0 for live bots.
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

static Detour SV_BotFrame_Detour;

// BW_DriveBot: synthesise one usercmd for a test client and push it
// through SV_ClientThink. Formerly SV_BotUserMove_Stub. SV_BotUserMove
// (0x8228AB98) has a __savegprlr prologue and cannot be detoured; instead
// SV_BotFrame_Stub calls this directly per bot, replicating the engine loop.
static void BW_DriveBot(clientBW_t *cl)
{
    if (!cl->gentity)
        return;  // no entity yet - silent (throttled probe below covers state)

    const int clientNum = static_cast<int>(cl - reinterpret_cast<clientBW_t *>(svsHeader->clients));
    if (clientNum < 0 || clientNum >= MAX_CLIENTS_BW)
        return;

    const bool isOurBot = g_botai[clientNum].is_bot;
    const int  isTC     = SV_IsTestClient(clientNum);  // diagnostic only

    // Throttled probe: log once per (clientNum) so the loop does not spam.
    {
        static int s_lastBD = -999;
        if (clientNum != s_lastBD)
        {
            s_lastBD = clientNum;
            DbgPrint("sv_bots: [CTPROBE] BW_DriveBot cn=%d is_bot=%d (SV_IsTestClient=%d) state=%d\n",
                     clientNum, isOurBot ? 1 : 0, isTC,
                     static_cast<int>(cl->header.state));
        }
    }

    // Gate on OUR flag, not SV_IsTestClient (unreliable on T4).
    if (!isOurBot)
        return;

    // Engine truth: SV_ClientThink does `if (*(int*)cl == 4)`. Only drive a
    // bot once it has climbed to CS_ACTIVE; stay inert during connect/pregame.
    if (static_cast<int>(cl->header.state) != 4)
    {
        DbgPrint("sv_bots: [BOTMOVE] cn=%d waiting (state=%d)\n",
                 clientNum, static_cast<int>(cl->header.state));
        return;
    }

    DbgPrint("sv_bots: [BOTMOVE] cn=%d driving (state=4)\n", clientNum);

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
// SV_BotFrame detour - the real per-frame bot driver
// ---------------------------------------------------------------------------
// Engine SV_BotFrame (0x8228AD80) runs once per server frame, loops every
// client slot, and calls SV_BotUserMove for each slot with state!=0 and
// netadr.type(cl+0x20)==0. SV_BotUserMove (0x8228AB98) cannot be detoured
// (__savegprlr prologue), so we detour SV_BotFrame itself and replicate its
// loop, calling BW_DriveBot directly. 0x82286510 is the engine's per-frame
// bot bookkeeping tick; we call it to stay faithful to the original.
typedef void (*SV_BotFramePrep_t)();
static SV_BotFramePrep_t SV_BotFramePrep =
    reinterpret_cast<SV_BotFramePrep_t>(0x82286510);

static void SV_BotFrame_Stub()
{
    SV_BotFramePrep();

    char *clientsBase = reinterpret_cast<char *>(svsHeader->clients);

    for (int i = 0; i < MAX_CLIENTS_BW; ++i)
    {
        clientBW_t *cl = reinterpret_cast<clientBW_t *>(
            clientsBase + static_cast<size_t>(i) * 0xB762C);

        // Engine gate 1: slot not empty (header.state != 0).
        if (static_cast<int>(cl->header.state) == 0)
            continue;

        // Engine gate 2: netadr.type at cl+0x20 == 0 (NA_BOT).
        if (*reinterpret_cast<int *>(reinterpret_cast<char *>(cl) + 0x20) != 0)
            continue;

        // BW_DriveBot applies its own SV_IsTestClient + CS_ACTIVE guards.
        BW_DriveBot(cl);
    }
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

// ---------------------------------------------------------------------------
// Com_Printf detour — mirror ALL engine console output into xenia.log.
//
// Diagnostic. The GSC compiler writes "unknown function" / "bad syntax" to
// the engine console; Xbox 360 has no console, so this copies every line
// into DbgPrint. Leave enabled until the GSC layer compiles clean.
//
// Targets 0x82271C60 (full prologue, detourable). It is varargs; the hook
// reads only the format string (r4) — sufficient for error messages, which
// arrive pre-substituted. The original is varargs too: we forward the two
// named args; r5-r10 (the varargs home) are preserved by the trampoline's
// copy of the original prologue (std r5..r10), so the original still sees
// its variadic arguments intact.
// ---------------------------------------------------------------------------

static Detour Com_Printf_Detour;

static void Com_Printf_Hook(int channel, const char* fmt, int a2, int a3,
                            int a4, int a5, int a6)
{
    if (fmt && fmt[0])
    {
        char buf[1024];
        size_t n = 0;
        for (; fmt[n] && n < sizeof(buf) - 1; ++n)
            buf[n] = fmt[n];
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            --n;
        buf[n] = '\0';

        if (n > 0)
            DbgPrint("sv_bots: [CON] %s\n", buf);
    }

    Com_Printf_Detour.GetOriginal<Com_Printf_t>()(channel, fmt, a2, a3, a4, a5, a6);
}

// ===========================================================================
// r293 DIAGNOSTIC TOGGLES
// ===========================================================================
#define CODXE_DIAG_ENABLE_WEAPON_HOOK         0
#define CODXE_DIAG_ENABLE_USERINFO_HOOK       0   // r344: detours OFF — isolate detour mechanism
#define CODXE_DIAG_ENABLE_BOTUSERMOVE         0   // r350: detours OFF — SV_BotFrame detour froze pregame (codjumper baseline). NO MORE DETOURS.

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

// botdbg(<tag>): diagnostic builtin. GSC has no visible logging (BW's
// println does not reach xenia.log), so GSC calls this to emit a
// DbgPrint that DOES appear in the log. Used to trace how far the bot
// connect chain progresses. Pure diagnostic - safe to leave in.
static void Scr_BotDbg(scr_entref_t entref)
{
    (void)entref;
    const char *tag = "(null)";
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) >= 1)
        tag = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    DbgPrint("sv_bots: [GSCDBG] %s\n", tag);
}

// botclientthink(): GSC-driven per-frame bot driver. The engine routes
// SV_BotFrame -> SV_BotUserMove -> SV_ClientThink to feed test clients a
// usercmd; neither engine function can be detoured here (SV_BotUserMove
// has a __savegprlr prologue; detouring SV_BotFrame freezes pregame).
// Instead BW's GSC runs a per-bot wait-loop that calls this method every
// frame. It builds the usercmd and calls SV_ClientThink via BW_DriveBot -
// the same work SV_BotUserMove would do, through the safe builtin channel.
static void Scr_BotClientThink(scr_entref_t entref)
{
    {
        static int s_lastCT = -999;
        if (static_cast<int>(entref.entnum) != s_lastCT)
        {
            s_lastCT = static_cast<int>(entref.entnum);
            DbgPrint("sv_bots: [CTPROBE] Scr_BotClientThink called, entnum=%d classnum=%d\n",
                     static_cast<int>(entref.entnum), static_cast<int>(entref.classnum));
        }
    }
    BW_RequirePlayerEntity(entref);

    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 0)
        Scr_Error_BW("Usage: <bot> botClientThink();", SCRIPTINSTANCE_SERVER);

    clientBW_t *cl = BW_GetClient(entref.entnum);
    BW_DriveBot(cl);
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

        // --- r349: NA_BOT gate fix --------------------------------------
        // SV_BotFrame (0x8228AD80) only calls SV_BotUserMove for a client
        // whose netadr.type at (cl + 0x20) == 0 (NA_BOT). SV_AddTestClient
        // leaves it non-zero, so the engine never drives our bots -> they
        // never get a usercmd, SV_ClientThink never runs, they never spawn.
        // Force the address type to NA_BOT here. cl + 0x20 == netchan
        // remote-address type field (verified: SV_AddTestClient passes
        // cl+0x20 into NET_CompareAdr; SV_BotFrame tests *(int*)(cl+0x20)).
        {
            clientBW_t *bw = reinterpret_cast<clientBW_t *>(
                reinterpret_cast<char *>(svsHeader->clients) +
                static_cast<size_t>(ent->s.number) * 0xB762C);
            int *adrType = reinterpret_cast<int *>(
                reinterpret_cast<char *>(bw) + 0x20);
            DbgPrint("sv_bots: [ADDTESTCLIENT] cn=%d netadr.type was %d -> forcing 0 (NA_BOT)\n",
                     ent->s.number, *adrType);
            *adrType = 0;
        }
        // ----------------------------------------------------------------

        // Mark this entnum as one of OUR bots. BW_DriveBot gates on this
        // instead of engine SV_IsTestClient (which is unreliable on T4).
        if (ent->s.number >= 0 && ent->s.number < MAX_CLIENTS_BW)
            g_botai[ent->s.number].is_bot = true;

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

    // Clear our bot flag so a real player reusing this slot is not driven.
    g_botai[clientNum].is_bot = false;
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
    {"botclientthink",    Scr_BotClientThink},
    {"botdbg",            Scr_BotDbg},
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
    DbgPrint("sv_bots: init T4 BW (r350, detours disabled)\n");
    DbgPrint("sv_bots: r350 build - all engine detours OFF (Detour mechanism freezes pregame)\n");
    DbgPrint("sv_bots: [DIAG] r350 | weapon=%d userinfo=%d botmove=%d\n",
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
    // SV_BotUserMove (0x8228AB98) is un-detourable (__savegprlr prologue).
    // Detour SV_BotFrame (0x8228AD80) instead and drive bots ourselves.
    SV_BotFrame_Detour = Detour(reinterpret_cast<void *>(0x8228AD80), SV_BotFrame_Stub);
    SV_BotFrame_Detour.Install();
    DbgPrint("sv_bots: SV_BotFrame detour INSTALLED (drives bots via BW_DriveBot)\n");
#else
    DbgPrint("sv_bots: SV_BotFrame detour SKIPPED (diagnostic)\n");
#endif

#if CODXE_DIAG_ENABLE_USERINFO_HOOK
    SV_UserinfoChanged_Detour = Detour(SV_UserinfoChanged, SV_UserinfoChanged_Hook);
    SV_UserinfoChanged_Detour.Install();
    DbgPrint("sv_bots: SV_UserinfoChanged detour INSTALLED\n");
#else
    DbgPrint("sv_bots: SV_UserinfoChanged detour SKIPPED (diagnostic)\n");
#endif

    Com_Printf_Detour = Detour(CODXE_COM_PRINTF_ADDR, Com_Printf_Hook);
    Com_Printf_Detour.Install();
    DbgPrint("sv_bots: Com_Printf console-mirror detour INSTALLED @ 0x82271C60\n");

    // SV_CalcPings deliberately NOT detoured — see file header.
}

sv_bots::~sv_bots()
{
    DbgPrint("sv_bots: removing T4 BW detours\n");

#if CODXE_DIAG_ENABLE_WEAPON_HOOK
    G_SelectWeaponIndex_Detour.Remove();
#endif
#if CODXE_DIAG_ENABLE_BOTUSERMOVE
    SV_BotFrame_Detour.Remove();
#endif
#if CODXE_DIAG_ENABLE_USERINFO_HOOK
    SV_UserinfoChanged_Detour.Remove();
#endif

    Com_Printf_Detour.Remove();

    CleanBotArray();
}

} // namespace mp
} // namespace t4
