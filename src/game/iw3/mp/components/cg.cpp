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
dvar_s *compass_group0 = nullptr;

// v17 native follow-spec compass: draw the engine's own compass actor blips
// (both teams: friendly arrows + enemy dots) by calling Function_82322868
// directly each frame, no .ff edit and no menu.
// v24: NO LONGER inert. The draw is now triggered from the R_DrawStretchPic
// detour, piggybacking on the GSC scMap blit (see R_DrawStretchPic_Hook).
dvar_s *compass_native = nullptr;
dvar_s *compass_native_bg = nullptr;
dvar_s *compass_native_mode = nullptr;
dvar_s *compass_native_x = nullptr;
dvar_s *compass_native_y = nullptr;
dvar_s *compass_native_size = nullptr;
dvar_s *compass_native_align = nullptr;

// v21 caster radar dvars (DLL-draw path -- now inert, kept for the fallback).
dvar_s *caster_dots = nullptr;
dvar_s *caster_dot_size = nullptr;

// v23 caster radar: GSC draws, DLL publishes. events.h exposes only
// OnCG_DrawActive (3D pass) -- no 2D/HUD draw event -- and the spectate 2D HUD
// chain (8230EB50/821F1AE8/...) all open with `mfspr r12,LR; bl ...`, the
// prologue codxe's Detour mis-relocates (freeze). So the DLL cannot reach the
// 2D pass via a chain detour in plain spectate. v23 answered this by having
// GSC hudelems draw everything; v24 adds the missing piece: R_DrawStretchPic
// (0x8216BAE8) IS detour-safe (its `bl __savefpr` is instruction 6, outside
// the 4-instruction relocation window) and it runs in the spectate 2D pass --
// proven by the GSC scMap hudelem rendering there. Detouring it gives the DLL
// a 2D-pass execution point in plain spectate.
dvar_s *caster_publish = nullptr;

// Forward declaration: drawn from the R_DrawStretchPic detour (v24).
static void DrawNativeSpecCompass();

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

// v23 caster radar publisher. The compass projection basis -- world origin
// (cg+0x4e480/4), world size (cg+0x4e488/c) and north vector (cg+0x4e478/4) --
// is STATIC per map, so we publish it once per map via Cbuf `set` (the string
// path getdvarfloat reads reliably). GSC then projects every player with this
// basis and draws the map + dots as hudelems, which render in plain spectate
// (unlike a DLL draw from the 3D OnCG_DrawActive pass). Pure data reads/writes,
// no detour, no render-pass dependency.
static void PublishCasterBounds()
{
    if (caster_publish == nullptr || !caster_publish->current.enabled)
        return;

    const char *mn = Dvar_GetString("mapname");
    if (mn == nullptr || mn[0] == '\0')
        return;

    static char lastMap[64] = {0};
    static bool published = false;

    bool sameMap = true;
    for (int k = 0; k < 63; ++k)
    {
        if (lastMap[k] != mn[k])
            sameMap = false;
        if (mn[k] == '\0')
            break;
    }
    if (!sameMap)
    {
        int k = 0;
        for (; mn[k] != '\0' && k < 63; ++k)
            lastMap[k] = mn[k];
        lastMap[k] = '\0';
        published = false;
    }
    if (published)
        return;

    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float wsx = *reinterpret_cast<volatile float *>(cgBase + 0x4e488);
    float wsy = *reinterpret_cast<volatile float *>(cgBase + 0x4e48c);
    if (wsx == 0.0f || wsy == 0.0f)
        return; // compass bounds not loaded yet this frame -- retry next frame

    float wox = *reinterpret_cast<volatile float *>(cgBase + 0x4e480);
    float woy = *reinterpret_cast<volatile float *>(cgBase + 0x4e484);
    float nx = *reinterpret_cast<volatile float *>(cgBase + 0x4e478);
    float ny = *reinterpret_cast<volatile float *>(cgBase + 0x4e474);

    char cmd[320];
    sprintf_s(cmd,
              "set caster_wox %f;set caster_woy %f;set caster_wsx %f;set caster_wsy %f;"
              "set caster_nx %f;set caster_ny %f\n",
              wox, woy, wsx, wsy, nx, ny);
    Cbuf_AddText(0, cmd);
    published = true;
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
    (void)localClientNum;

    DrawBranding();

    // v24: DrawNativeSpecCompass() REMOVED from this hook. Two reasons, both
    // proven: (1) UI_DrawBuildNumber does not fire during plain spectate, so
    // it can never be the spec draw site (v22 failure); (2) it DOES fire in
    // the main menu and gameplay, so with compass_native enabled it was the
    // "radar everywhere" leak. The spec draw now lives in
    // R_DrawStretchPic_Hook, triggered by the GSC scMap blit itself.

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

// Engine's own "valid followed player" test, lifted from Function_8231E070's
// early-out: followedSlot (0x849f3664) must be in [1..count(0x849f3620)] and
// the player-slot pointer it indexes via 0x849f2df0 + (slot+0x20c)*4 must be
// live. Local client 0 only.
static bool ValidFollowedPlayer()
{
    int followedSlot = *reinterpret_cast<volatile int *>(0x849F3664u);
    int idx = followedSlot - 1;
    int count = *reinterpret_cast<volatile int *>(0x849F3620u);
    if (idx < 0 || idx >= count)
        return false;
    uint32_t slot = *reinterpret_cast<volatile uint32_t *>(
        0x849F2DF0u + (((uint32_t)followedSlot + 0x20Cu) << 2));
    if (slot == 0)
        return false;
    return *reinterpret_cast<volatile uint32_t *>(slot) != 0;
}

// Native compass actor-blip drawer. Function_82322868 loops the compass-actor
// array (24 slots, engine-filled every frame from the snapshot) and draws BOTH
// teams (friendly arrows + enemy dots). v24: this is the SHIPPING blip drawer,
// fired from R_DrawStretchPic_Hook.
typedef void (*CompassDrawActors_fn_t)(int, int, void *, void *, float *);
static CompassDrawActors_fn_t const CompassDrawActors_fn =
    reinterpret_cast<CompassDrawActors_fn_t>(0x82322868u);

// R_RegisterMaterial(type, name) -> handle. type 4 = 2D/UI material.
typedef int (*R_RegisterMaterial_fn_t)(int, const char *);
static R_RegisterMaterial_fn_t const R_RegisterMaterial_fn =
    reinterpret_cast<R_RegisterMaterial_fn_t>(0x822A0298u);
// Compass projection (82323E28): fills oA,oB,oC,oD = x,y,w,h.
typedef void (*CompassProject_fn_t)(int, int, void *, void *, float *, float *, float *, float *);
static CompassProject_fn_t const CompassProject_fn =
    reinterpret_cast<CompassProject_fn_t>(0x82323E28u);
// Per-client compass matrix transform (822b36e8).
typedef void (*CompassMatrixXform_fn_t)(float *, float *, float *, float *, float *, int, int);
static CompassMatrixXform_fn_t const CompassMatrixXform_fn =
    reinterpret_cast<CompassMatrixXform_fn_t>(0x822B36E8u);
// R_DrawStretchPic (8216BAE8): (x,y,w,h, s0,t0,s1,t1, color, material).
typedef void (*R_DrawStretchPic_fn_t)(float, float, float, float, float, float, float, float, const float *, int);
static R_DrawStretchPic_fn_t const R_DrawStretchPic_fn =
    reinterpret_cast<R_DrawStretchPic_fn_t>(0x8216BAE8u);

struct compassRectDef_s
{
    float x;
    float y;
    float w;
    float h;
    int horzAlign;
    int vertAlign;
};

static bool CompassMatrixReady()
{
    const volatile float *m = reinterpret_cast<volatile float *>(0x8246F308u);
    float a = m[0];
    float b = m[1];
    if (a != a || b != b) // NaN
        return false;
    if (a == 0.0f && b == 0.0f)
        return false;
    return true;
}

static int RegisterCompassMapMaterial()
{
    const char *mn = Dvar_GetString("mapname");
    if (mn == nullptr || mn[0] == '\0')
        return 0;

    char name[160];
    const char prefix[] = "compass_map_";
    int i = 0;
    for (; prefix[i] != '\0'; ++i)
        name[i] = prefix[i];
    int j = 0;
    for (; mn[j] != '\0' && i < 159; ++j, ++i)
        name[i] = mn[j];
    name[i] = '\0';

    static char lastName[160] = {0};
    static int cachedHandle = 0;
    bool same = (cachedHandle != 0);
    for (int k = 0; same && k <= i; ++k)
        if (lastName[k] != name[k])
            same = false;
    if (same)
        return cachedHandle;

    cachedHandle = R_RegisterMaterial_fn(4, name);
    for (int k = 0; k <= i; ++k)
        lastName[k] = name[k];
    return cachedHandle;
}

static void DrawNativeSpecMap(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    int handle = RegisterCompassMapMaterial();
    if (handle == 0)
        return;

    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float *matrix = reinterpret_cast<float *>(0x8246F308u);

    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    CompassProject_fn(mode, cgBase, scratch, rect, &a, &b, &c, &d);
    CompassMatrixXform_fn(matrix, &a, &b, &c, &d, horzAlign, vertAlign);

    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    R_DrawStretchPic_fn(a, b, c, d, 0.0f, 0.0f, 1.0f, 1.0f, white, handle);
}

// v21 caster radar DLL draw -- INERT (caster_dots default OFF in v24). Kept as
// an A/B fallback alongside the native blips.
static int s_whiteMat = 0;

static void DrawCasterDots(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    if (s_whiteMat == 0)
        s_whiteMat = R_RegisterMaterial_fn(4, "white");
    if (s_whiteMat == 0)
        return;

    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float *matrix = reinterpret_cast<float *>(0x8246F308u);

    float ox = 0.0f, oy = 0.0f, ow = 0.0f, oh = 0.0f;
    CompassProject_fn(mode, cgBase, scratch, rect, &ox, &oy, &ow, &oh);

    float nX = *reinterpret_cast<volatile float *>(cgBase + 0x4e478);
    float nY = *reinterpret_cast<volatile float *>(cgBase + 0x4e474);
    float wOx = *reinterpret_cast<volatile float *>(cgBase + 0x4e480);
    float wOy = *reinterpret_cast<volatile float *>(cgBase + 0x4e484);
    float wW = *reinterpret_cast<volatile float *>(cgBase + 0x4e488);
    float wH = *reinterpret_cast<volatile float *>(cgBase + 0x4e48c);
    if (wW == 0.0f || wH == 0.0f)
        return;

    float ds = static_cast<float>(caster_dot_size->current.integer);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};

    const char *b = Dvar_GetString("caster_blips");
    while (b != nullptr && *b != '\0')
    {
        float px = static_cast<float>(atof(b));
        while (*b != '\0' && *b != ' ')
            ++b;
        while (*b == ' ')
            ++b;
        if (*b == '\0')
            break;
        float py = static_cast<float>(atof(b));
        while (*b != '\0' && *b != ' ')
            ++b;
        while (*b == ' ')
            ++b;

        float dx = px - wOx, dy = py - wOy;
        float u = (nX * dx - nY * dy) / wW;
        float v = -(nY * dx + nX * dy) / wH;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            continue;

        float qx = ox + u * ow - ds * 0.5f, qy = oy + v * oh - ds * 0.5f, qw = ds, qh = ds;
        CompassMatrixXform_fn(matrix, &qx, &qy, &qw, &qh, horzAlign, vertAlign);
        R_DrawStretchPic_fn(qx, qy, qw, qh, 0.0f, 0.0f, 1.0f, 1.0f, red, s_whiteMat);
    }

    float fdx = *reinterpret_cast<volatile float *>(cgBase + 0x478e0) - wOx;
    float fdy = *reinterpret_cast<volatile float *>(cgBase + 0x478e4) - wOy;
    float fu = (nX * fdx - nY * fdy) / wW;
    float fv = -(nY * fdx + nX * fdy) / wH;
    if (fu >= 0.0f && fu <= 1.0f && fv >= 0.0f && fv <= 1.0f)
    {
        float fs = ds * 1.6f;
        float fqx = ox + fu * ow - fs * 0.5f, fqy = oy + fv * oh - fs * 0.5f, fqw = fs, fqh = fs;
        float yellow[4] = {1.0f, 1.0f, 0.0f, 1.0f};
        CompassMatrixXform_fn(matrix, &fqx, &fqy, &fqw, &fqh, horzAlign, vertAlign);
        R_DrawStretchPic_fn(fqx, fqy, fqw, fqh, 0.0f, 0.0f, 1.0f, 1.0f, yellow, s_whiteMat);
    }
}

// v25 diagnostics: per-gate counters, published ~1/sec as compass_native_diag
// from OnCG_DrawActive (Cbuf on the cgame thread, same as PublishCasterBounds).
// h = hook saw the scMap material blit, f = ValidFollowedPlayer failed,
// m = compass matrix not ready, d = actor-draw actually invoked.
static unsigned int s_diagHookHits = 0;
static unsigned int s_diagFollowFail = 0;
static unsigned int s_diagMatrixFail = 0;
static unsigned int s_diagDrawCalls = 0;

static void DrawNativeSpecCompass()
{
    if (compass_native == nullptr || !compass_native->current.enabled)
        return;
    if (!ValidFollowedPlayer())
    {
        ++s_diagFollowFail;
        return;
    }
    if (!CompassMatrixReady())
    {
        ++s_diagMatrixFail;
        return;
    }

    compassRectDef_s rect;
    rect.x = static_cast<float>(compass_native_x->current.integer);
    rect.y = static_cast<float>(compass_native_y->current.integer);
    rect.w = static_cast<float>(compass_native_size->current.integer);
    rect.h = static_cast<float>(compass_native_size->current.integer);
    rect.horzAlign = compass_native_align->current.integer;
    rect.vertAlign = compass_native_align->current.integer;

    compassRectDef_s scratch = {0.0f, 0.0f, 0.0f, 0.0f, 0, 0};

    int mode = compass_native_mode->current.integer;

    if (compass_native_bg != nullptr && compass_native_bg->current.enabled)
        DrawNativeSpecMap(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (caster_dots != nullptr && caster_dots->current.enabled)
        DrawCasterDots(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);
    else
        CompassDrawActors_fn(0, mode, &scratch, &rect, color);

    ++s_diagDrawCalls;
}

// --- v24: R_DrawStretchPic detour -- the spec 2D-pass execution point --------
//
// Detour safety verified in Ghidra: 0x8216BAE8 opens
//     mfspr r12,LR / stw r12,-8(r1) / std r30,-0x18(r1) / std r31,-0x10(r1)
// -- four position-independent, branch-free instructions, which is exactly the
// 16-byte window codxe's Detour relocates (WriteFarBranch = 4 instructions).
// The `bl __savefpr` sits at instruction 6, outside the window, so the
// mis-relocation freeze (early-`bl` prologues) does not apply here.
//
// Trigger: the GSC caster map hudelem (scMap, material compass_map_<mapname>)
// renders through this function in plain follow-spectate. When we see that
// exact material blit and compass_native is on, the caster map was JUST drawn
// at the right place, in the right pass -- so we composite the native actor
// blips immediately after it. By construction this can never draw in the main
// menu (no scMap there), during gameplay (shoutcaster auto-off destroys
// scMap), or for playing clients (spectator-team hudelem).
//
// s_inNativeBlit guards re-entrancy: the blip drawer itself submits pics
// through this same function.

Detour R_DrawStretchPic_Detour;
static bool s_inNativeBlit = false;

void R_DrawStretchPic_Hook(float x, float y, float w, float h, float s0, float t0, float s1, float t1,
                           const float *color, int material)
{
    R_DrawStretchPic_Detour.GetOriginal<R_DrawStretchPic_fn_t>()(x, y, w, h, s0, t0, s1, t1, color, material);

    if (s_inNativeBlit)
        return;
    if (compass_native == nullptr || !compass_native->current.enabled)
        return;

    const int mapHandle = RegisterCompassMapMaterial();
    if (mapHandle == 0 || material != mapHandle)
        return;

    ++s_diagHookHits;

    s_inNativeBlit = true;
    DrawNativeSpecCompass();
    s_inNativeBlit = false;
}

cg::cg()
{
    Menus_OpenByName_Detour = Detour(Menus_OpenByName, Menus_OpenByName_Hook);
    Menus_OpenByName_Detour.Install();

    UI_DrawBuildNumber_Detour = Detour(UI_DrawBuildNumber, UI_DrawBuildNumber_Hook);
    UI_DrawBuildNumber_Detour.Install();

    // v24: 2D-pass execution point for the native spec compass. Prologue
    // verified branch-free for the 4-instruction relocation window (see the
    // comment block above R_DrawStretchPic_Hook).
    R_DrawStretchPic_Detour = Detour(reinterpret_cast<void *>(0x8216BAE8u),
                                     reinterpret_cast<const void *>(R_DrawStretchPic_Hook));
    R_DrawStretchPic_Detour.Install();

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

    compass_group0 = Dvar_RegisterBool("compass_group0", false, 0, "Retired: legacy follow-spec HUD group force (no-op)");

    // v24 native spec compass: enabled by GSC (caster_radar) while casting;
    // drawn from the R_DrawStretchPic detour when the GSC scMap blit is seen.
    compass_native = Dvar_RegisterBool("compass_native", false, 0,
        "Draw native engine compass blips over the GSC caster map (set by GSC while casting)");
    compass_native_bg = Dvar_RegisterBool("compass_native_bg", false, 0,
        "Also draw the native compass map texture under the blips (off: GSC scMap is the backdrop)");
    compass_native_mode = Dvar_RegisterInt("compass_native_mode", 1, 0, 1, 0,
        "Native spec compass projection: 0=player-centered, 1=static full-map");
    compass_native_x = Dvar_RegisterInt("compass_native_x", 16, -2000, 2000, 0,
        "Native spec compass rect X");
    compass_native_y = Dvar_RegisterInt("compass_native_y", 16, -2000, 2000, 0,
        "Native spec compass rect Y");
    compass_native_size = Dvar_RegisterInt("compass_native_size", 180, 8, 1024, 0,
        "Native spec compass rect size (square, w=h)");
    compass_native_align = Dvar_RegisterInt("compass_native_align", 0, 0, 7, 0,
        "Native spec compass rect horz/vert align enum (0-7)");

    // v24: caster_dots default OFF so the native actor blips (arrows + enemy
    // dots, fn 82322868) draw instead of the v21 red-square fallback.
    caster_dots = Dvar_RegisterBool("caster_dots", false, 0,
        "Fallback DLL draw: all players as red dots instead of native team blips");
    caster_dot_size = Dvar_RegisterInt("caster_dot_size", 6, 1, 64, 0,
        "Fallback DLL draw: dot size (projected units)");

    // v23 caster radar publisher: DLL publishes the static compass projection
    // basis once per map; GSC uses it for objective markers (and the dots, when
    // they existed).
    caster_publish = Dvar_RegisterBool("caster_publish", true, 0,
        "Publish compass projection basis (caster_wox/woy/wsx/wsy/nx/ny) for the GSC caster radar");

    // Build marker -- proves this cg.cpp compiled into the running codxe DLL.
    // GSC gates compass_native enablement on this being >= 24.
    Dvar_RegisterString("compass_native_diag", "", DVAR_FLAG_NONE,
        "v25 gate counters: h=hook hits, f=follow fail, m=matrix fail, d=draws");

    Dvar_RegisterInt("compass_hook_v", 26, 0, 100, 0,
        "Codxe compass hook build marker (v26 -- stretchpic native draw + on-screen gate diagnostics)");

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
            PublishCasterBounds();

            // v26: counters drawn on screen directly (R_AddCmdDrawText from
            // OnCG_DrawActive renders in plain follow-spec -- proven by the
            // PDIAG runs). The Cbuf publish below stays for the GSC print.
            if (compass_native != nullptr && compass_native->current.enabled)
            {
                char ndiag[128];
                sprintf_s(ndiag, "NDIAG h=%u f=%u m=%u d=%u",
                          s_diagHookHits, s_diagFollowFail, s_diagMatrixFail, s_diagDrawCalls);
                static Font_s *ndiagFont = R_RegisterFont("fonts/consoleFont");
                float ndiagCol[4] = {0.3f, 1.0f, 1.0f, 1.0f};
                R_AddCmdDrawText(ndiag, 128, ndiagFont, 10.f, 150.f, 1.0f, 1.0f, 0.0f, ndiagCol, 0);

                static unsigned int lastPublish = 0;
                const unsigned int now = GetTickCount();
                if (now - lastPublish >= 1000)
                {
                    lastPublish = now;
                    char diag[160];
                    sprintf_s(diag, "set compass_native_diag h=%u_f=%u_m=%u_d=%u\n",
                              s_diagHookHits, s_diagFollowFail, s_diagMatrixFail, s_diagDrawCalls);
                    Cbuf_AddText(0, diag);
                }
            }

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
    R_DrawStretchPic_Detour.Remove();
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
