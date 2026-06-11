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

// v15 follow-spec compass test. 0x849f4288 (per-client, stride 0x14a0) is the
// ACTIVE HUD MENU GROUP id, written by Function_821EF880 (puVar2[0x526]=group).
// Group 0 = in-game HUD (contains the compass ownerdraw), 7 = scoreboard,
// 5 = quickmessage. Follow-spectate sets group 6 (via 821EF648), and group 6
// does not contain the compass item, so it is never iterated/drawn.
// Test: while the local client sits in group 6, rewrite it to 0 so the HUD
// draws the in-game group. ONLY rewrites 6 -> 0, so scoreboard (7), quick-
// message (5) and every other group are untouched. Data write, dvar-gated,
// fully reversible. Known risks: may pull in more of the in-game HUD than
// just the compass, and group-6 consumers (follow cam / input) may misbehave.
dvar_s *compass_group0 = nullptr;
dvar_s *compass_menu_force = nullptr;

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

typedef int (*CG_Compass_IsVisible_fn_t)(int);
static CG_Compass_IsVisible_fn_t const CG_Compass_IsVisible_fn =
    reinterpret_cast<CG_Compass_IsVisible_fn_t>(0x82304290u);

// v17 RETIRED: forcing bit 0x10 of the key-routing word at 0x82435a18 hijacks
// the input layer (CL_KeyEvent forwards everything to the HUD-menu handler) --
// team-select / spawn menus stop receiving input and the bit is never cleared
// outside spec entry. Game-breaking, confirmed in-game.
//
// v18: skip the routing layer. The engine opens the in-game HUD menu (the
// menulist whose ownerdraws 0xb4..0xbc ARE the compass) via
// Function_821D47E8(name @ 0x82052dc4) -- the tail of 821EF510. We make that
// exact call ourselves, ONCE per cast session, on the 0->1 edge of
// compass_menu_force, from OnCG_DrawActive (same thread CL_KeyEvent runs on).
// No input state is touched. The dispatcher gate byte at 0x82435a12 is still
// re-asserted each frame while casting (data only, read by 821F0E90 when
// iterating ownerdraws; it does not route input).
typedef void (*HudMenu_OpenByName_fn_t)(const char *name, int a2, int a3, int a4);
static HudMenu_OpenByName_fn_t const HudMenu_OpenByName_fn =
    reinterpret_cast<HudMenu_OpenByName_fn_t>(0x821D47E8u);

static unsigned int s_menuOpenCalls = 0;

static void ForceHudMenuRouting()
{
    static bool wasOn = false;

    const bool on = (compass_menu_force != nullptr && compass_menu_force->current.enabled);
    if (!on)
    {
        wasOn = false;
        return;
    }

    // Dispatcher gate byte (data read, not input routing).
    *reinterpret_cast<volatile unsigned char *>(0x82435a12u) = 1;

    if (!wasOn)
    {
        wasOn = true;
        HudMenu_OpenByName_fn(reinterpret_cast<const char *>(0x82052dc4u), 0, 0, 0);
        ++s_menuOpenCalls;
    }
}

static void ForceFollowHudGroup()
{
    if (compass_group0 == nullptr || !compass_group0->current.enabled)
        return;
    volatile int *group = reinterpret_cast<volatile int *>(0x849F4288u);
    if (*group == 6)
        *group = 0;
}

static void ReadCompassDiag()
{
    // Compass draw gate. Function_82348BD0 (default HUD-map draw, used when the
    // hud-map-mode dvar at 0x823f56e8 reads 0) skips the ENTIRE compass when the
    // per-client byte at 0x823a7408 + client*0xe34 is 0.
    //   g0   = that byte for local client 0 (0 in follow + 1 alive => this is the gate)
    //   gAny = first client index 0..17 whose byte is set, else -1
    //   map  = hud-map-mode dvar value (0 => 82348BD0 draws; nonzero => 82320308
    //          draws instead and the g0 byte is irrelevant)
    const uint32_t COMPASS_BYTE = 0x823a7408u;
    const uint32_t COMPASS_STRIDE = 0xe34u;

    int g0 = *reinterpret_cast<volatile uint8_t *>(COMPASS_BYTE);
    int gAny = -1;
    for (int c = 0; c < 18; ++c)
    {
        if (*reinterpret_cast<volatile uint8_t *>(COMPASS_BYTE + (uint32_t)c * COMPASS_STRIDE) != 0)
        {
            gAny = c;
            break;
        }
    }

    int map = -1;
    uint32_t mapDvar = *reinterpret_cast<volatile uint32_t *>(0x823F56E8u);
    if (mapDvar != 0)
        map = *reinterpret_cast<volatile int *>(mapDvar + 0xCu);

    uint32_t cg_base = *reinterpret_cast<volatile uint32_t *>(0x823F28A0u);
    int mode = -1;
    if (cg_base != 0)
    {
        uint32_t uiptr = *reinterpret_cast<volatile uint32_t *>(cg_base + 0x24u);
        if (uiptr != 0)
            mode = *reinterpret_cast<volatile int *>(uiptr + 0x10u);
    }
    int vis = CG_Compass_IsVisible_fn(0);
    int field = *reinterpret_cast<volatile int *>(0x849F4288u);

    char buff[256];
    int bits = (int)*reinterpret_cast<volatile unsigned int *>(0x82435a18u);
    sprintf_s(buff, "PDIAG6 oc=%u bits=%x g0=%d gAny=%d map=%d mode=%d vis=%d field=%d",
              s_menuOpenCalls, bits, g0, gAny, map, mode, vis, field);
    static Font_s *diagFont = R_RegisterFont("fonts/consoleFont");
    float col[4] = {0.3f, 1.0f, 1.0f, 1.0f};
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

    compass_group0 = Dvar_RegisterBool("compass_group0", true, 0, "Follow-spec: rewrite HUD group 6 to 0 so the in-game HUD (compass) draws");

    compass_menu_force = Dvar_RegisterBool("compass_menu_force", false, 0,
        "Follow-spec: one-shot open of the in-game HUD menu (compass) on 0->1; no input routing touched");

    // Build marker -- proves this cg.cpp compiled into the running codxe DLL.
    // Read back via `\compass_hook_v` in console (11 = this build).
    Dvar_RegisterInt("compass_hook_v", 18, 0, 100, 0, "Codxe compass hook build marker (v18 -- one-shot HudMenu open, no input routing)");

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
            ReadCompassDiag();
            ForceFollowHudGroup();
            ForceHudMenuRouting();

            if (cg_draw_player_info->current.enabled)
            {
                CG_DrawPlayerInfo();
            }
        });
}

cg::~cg()
{
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
