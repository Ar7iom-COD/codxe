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
dvar_s *cg_no_bulletfx = nullptr;

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

// --- Weapon FX suppression via in-place instruction patches ----------------
//
// Both the view muzzle flash and the bullet surface-impact FX live in
// functions whose prologue is `mfspr r12,LR; bl __savegprlr; ...`. codxe's
// Detour copies the first 16 bytes to a trampoline and the relocated
// `bl __savegprlr` lands in garbage, so detouring those two functions freezes
// the game. Instead we patch single instructions in place (no trampoline,
// no relocation), exactly the kind of `.text` write the working detours
// already perform. Patches are applied/reverted from the per-frame
// OnCG_DrawActive event so both behave as live toggles.
//
// Muzzle flash: CG_AddViewModelWeapon (0x82317880) plays weaponDef->viewFlash
//   (weaponDef + 0x164) guarded by:
//       0x82317910  cmplwi cr6, r6, 0       ; r6 = viewFlashEffect
//       0x82317914  beq    cr6, 0x82317928  ; skip the flash play if zero
//       0x82317918..0x82317924              ; play the flash effect
//       0x82317928  ...                     ; add the weapon model (kept)
//   Patching 0x82317914 to an unconditional `b 0x82317928` always skips the
//   flash play while leaving the model add and everything else intact.
//
// Bullet impact: CG_PlayBulletImpactFX (0x82312C08) is pure client FX (both
//   callers ignore its return value). Patching its first instruction to `blr`
//   makes it return immediately, so no surface-impact effects play. Damage,
//   explosions, smoke, and muzzle flash are untouched.

#define CG_VIEWWEAPON_FLASH_BRANCH 0x82317914u // beq cr6, 0x82317928 (flash guard)
#define CG_VIEWWEAPON_FLASH_SKIP 0x48000014u   // b 0x82317928 (always skip flash)
#define CG_BULLETIMPACT_ENTRY 0x82312C08u      // CG_PlayBulletImpactFX entry
#define PPC_BLR 0x4E800020u                    // blr

static unsigned int s_origFlashBranch = 0;
static unsigned int s_origImpactEntry = 0;
static bool s_muzzleApplied = false;
static bool s_bulletApplied = false;

static void PatchCodeDword(unsigned int address, unsigned int value)
{
    *reinterpret_cast<volatile unsigned int *>(address) = value;

    // Xenia caches translated guest code, so a runtime write to .text is not
    // seen until the instruction cache for that range is invalidated, which
    // forces Xenia to re-translate the patched bytes. Load-time detours don't
    // need this because they patch before the function is ever translated.
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), sizeof(value));
}

static void ApplyWeaponFxPatches()
{
    const bool wantMuzzle = (cg_no_muzzleflash != nullptr) && cg_no_muzzleflash->current.enabled;
    if (wantMuzzle != s_muzzleApplied)
    {
        if (wantMuzzle)
        {
            s_origFlashBranch = *reinterpret_cast<volatile unsigned int *>(CG_VIEWWEAPON_FLASH_BRANCH);
            PatchCodeDword(CG_VIEWWEAPON_FLASH_BRANCH, CG_VIEWWEAPON_FLASH_SKIP);
        }
        else
        {
            PatchCodeDword(CG_VIEWWEAPON_FLASH_BRANCH, s_origFlashBranch);
        }
        s_muzzleApplied = wantMuzzle;
    }

    const bool wantBullet = (cg_no_bulletfx != nullptr) && cg_no_bulletfx->current.enabled;
    if (wantBullet != s_bulletApplied)
    {
        if (wantBullet)
        {
            s_origImpactEntry = *reinterpret_cast<volatile unsigned int *>(CG_BULLETIMPACT_ENTRY);
            PatchCodeDword(CG_BULLETIMPACT_ENTRY, PPC_BLR);
        }
        else
        {
            PatchCodeDword(CG_BULLETIMPACT_ENTRY, s_origImpactEntry);
        }
        s_bulletApplied = wantBullet;
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

    // Weapon FX toggles. These are applied as in-place instruction patches from
    // the OnCG_DrawActive event (NOT detours), so they are safe on the
    // __savegprlr-prologue functions that would otherwise freeze the game.
    cg_no_muzzleflash = Dvar_RegisterBool("cg_no_muzzleflash", false, 0, "Disable first-person muzzle flash");
    cg_no_bulletfx = Dvar_RegisterBool("cg_no_bulletfx", false, 0, "Disable bullet impact effects");

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
            ApplyWeaponFxPatches();

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

    // Revert any in-place weapon-FX patches so the function bodies are clean.
    if (s_muzzleApplied)
        PatchCodeDword(CG_VIEWWEAPON_FLASH_BRANCH, s_origFlashBranch);
    if (s_bulletApplied)
        PatchCodeDword(CG_BULLETIMPACT_ENTRY, s_origImpactEntry);
}
} // namespace mp
} // namespace iw3
