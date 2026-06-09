#include "pch.h"
#include "events.h"
#include "cj_tas.h"
#include "cg.h"

namespace iw3
{
namespace mp
{
dvar_s *bg_bobIdle = nullptr;

dvar_s *cg_scoreboardLabel_Score = nullptr;
dvar_s *cg_scoreboardLabel_Kills = nullptr;
dvar_s *cg_scoreboardLabel_Assists = nullptr;
dvar_s *cg_scoreboardLabel_Deaths = nullptr;

dvar_s *cg_draw_player_info = nullptr;

dvar_s *cg_no_muzzleflash = nullptr;

// v8 compass-spec diagnostics. Set by TickForceCompassMpForSpec each frame.
// Readable from GSC via getdvarint("compass_diag_*") to verify writes.
dvar_s *compass_diag_gate = nullptr;   // byte at 0x82435a12 after write
dvar_s *compass_diag_field = nullptr;  // int at cg+0x4e508 after write
dvar_s *compass_diag_ticks = nullptr;  // increments each tick

// CG_Compass_IsVisible (engine-internal, no symbol exported by codxe).
// Inline prologue at 0x82304290 -- detour-safe per the codxe constraint.
typedef int (*CG_Compass_IsVisible_t)(int clientNum);
static CG_Compass_IsVisible_t const CG_Compass_IsVisible =
    reinterpret_cast<CG_Compass_IsVisible_t>(0x82304290u);
Detour CG_Compass_IsVisible_Detour;

// Original returns:
//   1 iff (cg_drawCompass != 0) && (FUN_822cf188(clientNum) != 0)
//   0 otherwise.
// FUN_822cf188 returns 0 specifically for spec-follow clients (bit 0x10
// in per-client array DAT_82435a18), which is why the compass disappears
// the moment a shoutcaster locks onto a player. We replicate the dvar
// check and skip the spec-gate entirely.
//
// cg_drawCompass is cached per-client in the cg_t struct:
//   cg_t base ptr at DAT_823f28a0 = 0x823f28a0
//   sizeof(cg_t) = 0xf0a68
//   drawCompass field at offset 0x4e2d0
//
// Side effect: dead players in killcam now also see the compass
// (FUN_822cf188 would have returned 0 for them too). Desirable for a
// shoutcaster build; gate behind a dvar if you ship for ranked.
int CG_Compass_IsVisible_Hook(int clientNum)
{
    // Unconditional return 1: compass always visible for any client,
    // any spec mode, any round state. Sacrifices the cg_drawCompass
    // dvar gate -- shoutcaster build wants the compass on at all
    // times anyway. Avoids reading the per-client cg_t cache which
    // appears to be reset to 0 by the engine when entering spec-
    // follow, breaking the v2 hook that respected the dvar.
    (void)clientNum;
    return 1;
}

Detour BG_CalculateWeaponPosition_IdleAngles_Detour;

void BG_CalculateWeaponPosition_IdleAngles_Hook(weaponState_t *ws, float *angles)
{
    if (!bg_bobIdle->current.enabled)
        return;

    BG_CalculateWeaponPosition_IdleAngles_Detour.GetOriginal<decltype(BG_CalculateWeaponPosition_IdleAngles)>()(ws,
                                                                                                                angles);
}

Detour BG_CalculateView_IdleAngles_Detour;

void BG_CalculateView_IdleAngles_Hook(viewState_t *vs, float *angles)
{
    if (!bg_bobIdle->current.enabled)
        return;

    BG_CalculateView_IdleAngles_Detour.GetOriginal<decltype(BG_CalculateView_IdleAngles)>()(vs, angles);
}

Detour R_DrawAllDynEnt_Detour;

void R_DrawAllDynEnt_Hook(const GfxViewInfo *viewInfo)
{
    if (Dvar_GetBool("r_drawDynEnts"))
        R_DrawAllDynEnt_Detour.GetOriginal<decltype(R_DrawAllDynEnt)>()(viewInfo);
}

// --- Muzzle flash suppression via weapon-def field nulling ------------------
//
// CG_AddViewModelWeapon (0x82317880) plays the first-person muzzle flash by
// reading weaponDef->viewFlashEffect (weaponDef + 0x164) every frame and only
// playing it when the handle is non-zero:
//
//     iVar1 = (&bg_weaponDefs)[ent->weaponIndex];   // 0x823B9F60[idx]
//     if (*(uint *)(iVar1 + 0x164) != 0)
//         CG_PlayEffect(..., *(uint *)(iVar1 + 0x164));
//
// So zeroing that field for every loaded weapon suppresses the flash with no
// code patching. This is a DATA write (the field is read fresh each frame), so
// Xenia's translated-code cache is irrelevant - unlike a runtime .text patch,
// it takes effect immediately. The generic effect-play function (0x8230A110)
// also no-ops on a null handle, confirming this is safe.
//
// Detouring CG_AddViewModelWeapon directly is NOT an option: its prologue is
// `mfspr r12,LR; bl __savegprlr; ...`, and codxe's Detour mis-relocates that
// `bl` into the trampoline, which freezes the game.

#define BG_WEAPONDEFS_ARRAY 0x823B9F60u // array of weaponDef* indexed by weapon index
#define BG_NUMWEAPONS_ADDR 0x85027498u  // highest valid weapon index (weapon count)
#define WEAPONDEF_VIEWFLASH 0x164u      // viewFlashEffect handle offset within a weaponDef

static const unsigned int MAX_TRACKED_WEAPONS = 2048;
static unsigned int s_savedViewFlash[MAX_TRACKED_WEAPONS];
static bool s_savedViewFlashValid[MAX_TRACKED_WEAPONS];
static bool s_muzzleApplied = false;

static void ApplyMuzzleFlashState()
{
    const bool want = (cg_no_muzzleflash != nullptr) && cg_no_muzzleflash->current.enabled;

    unsigned int count = *reinterpret_cast<volatile unsigned int *>(BG_NUMWEAPONS_ADDR);
    if (count > MAX_TRACKED_WEAPONS - 1)
        count = MAX_TRACKED_WEAPONS - 1;

    unsigned int *const defs = reinterpret_cast<unsigned int *>(BG_WEAPONDEFS_ARRAY);

    if (want)
    {
        // Index 0 is the "none" weapon; valid weapons are 1..count.
        for (unsigned int i = 1; i <= count; ++i)
        {
            const unsigned int wd = defs[i];
            if (wd == 0)
                continue;

            volatile unsigned int *flash = reinterpret_cast<volatile unsigned int *>(wd + WEAPONDEF_VIEWFLASH);
            if (*flash != 0)
            {
                // Save the original the first time we see a non-zero handle for
                // this slot (also re-catches freshly precached defs after a map
                // change); steady state is read-only.
                if (!s_savedViewFlashValid[i])
                {
                    s_savedViewFlash[i] = *flash;
                    s_savedViewFlashValid[i] = true;
                }
                *flash = 0;
            }
        }
        s_muzzleApplied = true;
    }
    else if (s_muzzleApplied)
    {
        for (unsigned int i = 1; i <= count; ++i)
        {
            if (!s_savedViewFlashValid[i])
                continue;

            const unsigned int wd = defs[i];
            if (wd != 0)
                *reinterpret_cast<volatile unsigned int *>(wd + WEAPONDEF_VIEWFLASH) = s_savedViewFlash[i];

            s_savedViewFlashValid[i] = false;
        }
        s_muzzleApplied = false;
    }
}

void DrawBranding()
{
    const char *brandingWithBuild = branding::GetBrandingString();

    static Font_s *font = R_RegisterFont("fonts/consoleFont");
    float color[4] = {1.0, 1.0, 1.0, 0.4};

    R_AddCmdDrawText(brandingWithBuild, 256, font, 10, 20, 1.0, 1.0, 0.0, color, 0);
}

Detour UI_DrawBuildNumber_Detour;

void UI_DrawBuildNumber_Hook(const int localClientNum)
{
    DrawBranding();
    // Omit the original build number drawing
    // UI_DrawBuildNumber_Detour.GetOriginal<decltype(UI_DrawBuildNumber)>()
}

Detour UI_SafeTranslateString_Detour;

const char *UI_SafeTranslateString_Hook(char *reference)
{
    if (_ReturnAddress() == (void *)0x822FDDDC) // CG_DrawScoreboard_ListColumnHeaders -> UI_SafeTranslateString
    {
        if (strcmp(reference, "CGAME_SB_SCORE") == 0)
        {
            const auto val = Dvar_GetString("cg_scoreboardLabel_Score");
            if (val && *val)
                return val;
        }
        else if (strcmp(reference, "CGAME_SB_KILLS") == 0)
        {
            const char *val = Dvar_GetString("cg_scoreboardLabel_Kills");
            if (val && *val)
                return val;
        }
        else if (strcmp(reference, "CGAME_SB_ASSISTS") == 0)
        {
            const char *val = Dvar_GetString("cg_scoreboardLabel_Assists");
            if (val && *val)
                return val;
        }
        else if (strcmp(reference, "CGAME_SB_DEATHS") == 0)
        {
            const char *val = Dvar_GetString("cg_scoreboardLabel_Deaths");
            if (val && *val)
                return val;
        }
    }

    return UI_SafeTranslateString_Detour.GetOriginal<decltype(UI_SafeTranslateString)>()(reference);
}

Detour Menus_OpenByName_Detour;

void Menus_OpenByName_Hook(UiContext *dc, const char *menuName)
{
    if (strcmp(menuName, "settings_map_splitscreen") == 0)
        // Splitscreen map list only contains a subset of maps
        // Opening the full map settings menu allows selecting any map
        Menus_OpenByName_Detour.GetOriginal<decltype(Menus_OpenByName)>()(dc, "settings_map");
    else
        Menus_OpenByName_Detour.GetOriginal<decltype(Menus_OpenByName)>()(dc, menuName);

    // Increase the maximum number of clients in xboxlive private match and systemlink from 18 > 24
    if (strcmp(menuName, "menu_xboxlive_privatelobby") == 0)
    {
        Cbuf_AddText(0, "set party_maxplayers 24");
    }
    else if (strcmp(menuName, "menu_gamesetup_systemlink") == 0)
    {
        Cbuf_AddText(0, "set sv_maxclients 24");
    }
}


static const float colorWhiteRGBA[4] = {1.0f, 1.0f, 1.0f, 1.0f};

void CG_DrawPlayerInfo()
{
    auto ps = CG_GetPredictedPlayerState(0);
    int speed2D = static_cast<int>(sqrtf(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]));

    char buff[256];
    sprintf_s(buff,
              "x: %.6f\n"
              "y: %.6f\n"
              "z: %.6f\n"
              "pitch: %.6f\n"
              "yaw: %.6f\n"
              "speed: %d\n",
              ps->origin[0], ps->origin[1], ps->origin[2], ps->viewangles[0], ps->viewangles[1], speed2D);

    static Font_s *consoleFont = R_RegisterFont("fonts/consoleFont");
    const float x = 10.f * scrPlaceFullUnsafe.scaleVirtualToFull[0];
    const float y = 50.f;

    R_AddCmdDrawText(buff, 256, consoleFont, x, y, 1.0, 1.0, 0.0, colorWhiteRGBA, 0);
}

#define CG_BASE_PTR_ADDR 0x823F28A0u

// v9 READ-ONLY COMPASS GATE DIFF. No forcing -- observe which per-client
// compass-state values differ between free-spec (compass shows) and follow
// (compass hidden). Flip between the two on hardware; whatever value below
// changes between the screenshots IS the gate. Drawn top-left under the HUD.
static void ReadCompassDiff()
{
    uint32_t cg_base = *reinterpret_cast<volatile uint32_t *>(CG_BASE_PTR_ADDR);
    if (cg_base == 0)
        return;

    int dc      = *reinterpret_cast<volatile int *>(cg_base + 0x4e2d0u);   // cg_drawCompass cached
    int gateA   = *reinterpret_cast<volatile int *>(cg_base + 0x4e508u);   // dispatcher gate field
    int menuTs  = *reinterpret_cast<volatile int *>(cg_base + 0x4e490u);   // Compass_mp menu-open timestamp
    uint8_t cB  = *reinterpret_cast<volatile uint8_t *>(0x82435a12u);      // caller-gate byte [client0]
    uint8_t cC  = *reinterpret_cast<volatile uint8_t *>(0x82435a18u);      // FUN_822cf188 array byte [client0] (bit 0x10)
    int stateD  = *reinterpret_cast<volatile int *>(0x849F4288u);          // per-client state (==2 gate)

    char buff[256];
    sprintf_s(buff, "CDIFF dc=%d A=%d B=%d C=0x%02x D=%d menu=%d",
              dc, gateA, (int)cB, (int)cC, stateD, menuTs);

    static Font_s *diagFont = R_RegisterFont("fonts/consoleFont");
    float col[4] = {1.0f, 1.0f, 0.2f, 1.0f};
    const float x = 10.f * scrPlaceFullUnsafe.scaleVirtualToFull[0];
    R_AddCmdDrawText(buff, 256, diagFont, x, 150.f, 1.0f, 1.0f, 0.0f, col, 0);
}

cg::cg()
{
    Menus_OpenByName_Detour = Detour(Menus_OpenByName, Menus_OpenByName_Hook);
    Menus_OpenByName_Detour.Install();

    UI_DrawBuildNumber_Detour = Detour(UI_DrawBuildNumber, UI_DrawBuildNumber_Hook);
    UI_DrawBuildNumber_Detour.Install();

    // Default to true for idle gun sway
    // This is the default behavior in the original game.
    bg_bobIdle = Dvar_RegisterBool("bg_bobIdle", true, 0, "Idle gun sway");

    BG_CalculateWeaponPosition_IdleAngles_Detour =
        Detour(BG_CalculateWeaponPosition_IdleAngles, BG_CalculateWeaponPosition_IdleAngles_Hook);
    BG_CalculateWeaponPosition_IdleAngles_Detour.Install();

    BG_CalculateView_IdleAngles_Detour = Detour(BG_CalculateView_IdleAngles, BG_CalculateView_IdleAngles_Hook);
    BG_CalculateView_IdleAngles_Detour.Install();

    R_DrawAllDynEnt_Detour = Detour(R_DrawAllDynEnt, R_DrawAllDynEnt_Hook);
    R_DrawAllDynEnt_Detour.Install();

    Dvar_RegisterBool("r_drawDynEnts", true, 0, "Draw dynamic entities");

    // Muzzle flash toggle. Applied by nulling weaponDef->viewFlashEffect as data
    // from the OnCG_DrawActive event (no detour, no runtime code patch).
    cg_no_muzzleflash = Dvar_RegisterBool("cg_no_muzzleflash", false, 0, "Disable first-person muzzle flash");

    // Build marker -- proves cg.cpp constructor ran. User can verify
    // via `\compass_hook_v` in console. If the dvar reads back as 4,
    // this exact cg.cpp was compiled into the codxe DLL the user is
    // running. If undefined/missing, the build didn't pick up our
    // changes.
    Dvar_RegisterInt("compass_hook_v", 10, 0, 100, 0, "Codxe compass hook build marker (v10 -- follow-spec compass .text patch)");

    // ---- Follow-spec compass restore (load-time .text patch) ---------------
    // Function_8231E070 @ 0x8231E070 decides whether to (re)apply the in-game
    // HUD menu group, which the compass belongs to. For a follow-spectator the
    // per-client UI state field (0x849f4288 + client*0x14a0) == 6, so at
    // 0x8231E0A0 it runs `bne cr6, 0x8231E0DC` (take the HUD-apply path only
    // when field != 6); field == 6 instead falls into a validity check that
    // early-returns and skips Function_821EF880(client, 0, ...) -- the call
    // that wires the compass into the active HUD set. Free-spectate (field !=
    // 6) does not early-return, which is why its compass shows.
    //
    // Make that branch UNCONDITIONAL so every path applies the HUD group, just
    // like free-spectate. The skipped block only computed the early-return
    // condition (no state writes), so bypassing it is clean.
    //   0x8231E0A0:  40 9A 00 3C  (bne cr6, 0x8231E0DC)
    //            ->  48 00 00 3C  (b        0x8231E0DC)
    // Load-time .text write (constructor runs before Xenia translates the
    // function), same mechanism as the other codxe .text patches.
    *reinterpret_cast<volatile uint32_t *>(0x8231E0A0u) = 0x4800003Cu;

    compass_diag_gate =
        Dvar_RegisterInt("compass_diag_gate", -1, -1, 0xff, 0, "Read-back of byte at 0x82435a12 after v8 write (1 = stuck, 0 = clobbered)");
    compass_diag_field =
        Dvar_RegisterInt("compass_diag_field", -1, INT32_MIN, INT32_MAX, 0, "Read-back of int at cg+0x4e508 after v8 write");
    compass_diag_ticks =
        Dvar_RegisterInt("compass_diag_ticks", 0, 0, INT32_MAX, 0, "Tick count of TickForceCompassMpForSpec, proves OnCG_DrawActive fires");

    // Compass spec-gate bypass via full-function detour. The raw .text
    // write attempt at 0x823042C8 did not take effect under Xenia; the
    // Detour path (which the rest of cg.cpp already relies on for
    // load-time code modification) does. Hook replaces Function_82304290
    // (CG_Compass_IsVisible) wholesale: returns 1 iff cg_drawCompass != 0,
    // ignoring the spec-follow gate that hid the compass when locked onto
    // another player.
    // v9: forcing DISABLED for the read-only diff -- observe vanilla compass
    // behavior (shows in free-spec, hidden in follow). Re-enable to force.
    CG_Compass_IsVisible_Detour = Detour(CG_Compass_IsVisible, CG_Compass_IsVisible_Hook);
    // CG_Compass_IsVisible_Detour.Install();

    UI_SafeTranslateString_Detour = Detour(UI_SafeTranslateString, UI_SafeTranslateString_Hook);
    UI_SafeTranslateString_Detour.Install();

    cg_scoreboardLabel_Score = Dvar_RegisterString("cg_scoreboardLabel_Score", "", DVAR_FLAG_NONE,
                                                   "Override label for 'Score' column on scoreboard");

    cg_scoreboardLabel_Kills = Dvar_RegisterString("cg_scoreboardLabel_Kills", "", DVAR_FLAG_NONE,
                                                   "Override label for 'Kills' column on scoreboard");

    cg_scoreboardLabel_Assists = Dvar_RegisterString("cg_scoreboardLabel_Assists", "", DVAR_FLAG_NONE,
                                                     "Override label for 'Assists' column on scoreboard");

    cg_scoreboardLabel_Deaths = Dvar_RegisterString("cg_scoreboardLabel_Deaths", "", DVAR_FLAG_NONE,
                                                    "Override label for 'Deaths' column on scoreboard");

    cg_draw_player_info =
        Dvar_RegisterBool("cg_draw_player_info", false, 0, "Draw player info (origin, viewangles, speed) on screen");

    Events::OnCG_DrawActive(
        []()
        {
            ApplyMuzzleFlashState();
            ReadCompassDiff(); // v9: read-only gate diff (forcing disabled)

            if (cg_draw_player_info->current.enabled)
            {
                CG_DrawPlayerInfo();
            }
        });
}

cg::~cg()
{
    // CG_Compass_IsVisible_Detour.Remove(); // v9: not installed (diff build)
    Menus_OpenByName_Detour.Remove();
    UI_DrawBuildNumber_Detour.Remove();
    UI_SafeTranslateString_Detour.Remove();
    BG_CalculateWeaponPosition_IdleAngles_Detour.Remove();
    BG_CalculateView_IdleAngles_Detour.Remove();

    // Restore any view-flash handles we zeroed so the weapon defs are clean.
    if (s_muzzleApplied)
    {
        unsigned int count = *reinterpret_cast<volatile unsigned int *>(BG_NUMWEAPONS_ADDR);
        if (count > MAX_TRACKED_WEAPONS - 1)
            count = MAX_TRACKED_WEAPONS - 1;

        unsigned int *const defs = reinterpret_cast<unsigned int *>(BG_WEAPONDEFS_ARRAY);
        for (unsigned int i = 1; i <= count; ++i)
        {
            if (!s_savedViewFlashValid[i])
                continue;

            const unsigned int wd = defs[i];
            if (wd != 0)
                *reinterpret_cast<volatile unsigned int *>(wd + WEAPONDEF_VIEWFLASH) = s_savedViewFlash[i];
        }
    }
}
} // namespace mp
} // namespace iw3
