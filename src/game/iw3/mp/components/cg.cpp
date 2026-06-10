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

// v17 native follow-spec compass: draw the engine's own compass actor blips
// (both teams: friendly arrows + enemy dots) by calling Function_82322868
// directly each frame, no .ff edit and no menu. Placement is dvar-tunable so
// it can be dialed in on Xenia without rebuilding.
dvar_s *compass_native = nullptr;
dvar_s *compass_native_bg = nullptr;
dvar_s *compass_native_mode = nullptr;
dvar_s *compass_native_x = nullptr;
dvar_s *compass_native_y = nullptr;
dvar_s *compass_native_size = nullptr;
dvar_s *compass_native_align = nullptr;

// v21 caster radar dvars: replace native team-arrow blips with uniform red dots
// for ALL players + a yellow followed-player marker.
dvar_s *caster_dots = nullptr;
dvar_s *caster_dot_size = nullptr;

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

// Engine's own "valid followed player" test, lifted from Function_8231E070's
// early-out: followedSlot (0x849f3664) must be in [1..count(0x849f3620)] and
// the player-slot pointer it indexes via 0x849f2df0 + (slot+0x20c)*4 must be
// live. Group 6 is the GENERIC spectate/intermission UI state -- it is also
// active at the CHOOSE TEAM screen (v15 broke team select by forcing there).
// This check is what distinguishes "locked onto a player" from "sitting in
// spec UI", so the group rewrite only fires mid-follow. Local client 0 only.
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
// teams -- friendly directional arrows + enemy dots -- through the per-client
// compass transform at 0x8246f308. Verified in Ghidra: every wrapper around it
// is a NOP or __savefpr/__restfpr register thunk, so it has NO menu-paint
// context dependency, and the full-map path is NOT behind the follow-spec gate
// that hides the rotating compass. So we can call it straight from the per-
// frame draw with a hand-built rectDef, no menu open and no .ff edit.
//   p1 = local client (0)
//   p2 = mode: 0 = full-map projection, 1 = rotating compass
//   p3 = scratch rect ptr (only read in rotating mode -- we pass a zeroed rect)
//   p4 = rectDef { float x,y,w,h; int horzAlign, vertAlign } -- the screen box
//   p5 = base color RGBA floats (p5[3] alpha is written/clamped by the fn)
typedef void (*CompassDrawActors_fn_t)(int, int, void *, void *, float *);
static CompassDrawActors_fn_t const CompassDrawActors_fn =
    reinterpret_cast<CompassDrawActors_fn_t>(0x82322868u);

// Native compass MAP BACKGROUND -- self-drawn so WE own the material. We do NOT
// call 82325E78: that fn paints whatever 2D material the menu bound via stack
// pass-through, which we can't reproduce out of context (that unbound material
// was the v18 hang, not unsafety). Instead we replicate its body and pass the
// material explicitly, so map + blips share the exact same projection + matrix
// and line up by construction. Verified call chain in Ghidra (82310BE8 shows
// the same recipe: register material, then draw through the compass matrix).
//
// R_RegisterMaterial(type, name) -> handle. type 4 = 2D/UI material.
typedef int (*R_RegisterMaterial_fn_t)(int, const char *);
static R_RegisterMaterial_fn_t const R_RegisterMaterial_fn =
    reinterpret_cast<R_RegisterMaterial_fn_t>(0x822A0298u);
// Compass projection (82323E28): fills oA,oB,oC,oD = x,y,w,h (rect * compass
// scale in full-map mode). Identical call the blip drawer makes internally.
//   (mode, cgBase, scratch, rectDef, &oA, &oB, &oC, &oD)
typedef void (*CompassProject_fn_t)(int, int, void *, void *, float *, float *, float *, float *);
static CompassProject_fn_t const CompassProject_fn =
    reinterpret_cast<CompassProject_fn_t>(0x82323E28u);
// Per-client compass matrix transform (822b36e8) -- the alignment glue shared
// with the blips. (matrix, &a, &b, &c, &d, horzAlign, vertAlign)
typedef void (*CompassMatrixXform_fn_t)(float *, float *, float *, float *, float *, int, int);
static CompassMatrixXform_fn_t const CompassMatrixXform_fn =
    reinterpret_cast<CompassMatrixXform_fn_t>(0x822B36E8u);
// R_DrawStretchPic (8216BAE8): (x,y,w,h, s0,t0,s1,t1, color, material). color +
// material are the 9th/10th args; declaring them explicitly puts the material
// in the slot the fn reads -- which the menu pass-through chain denied v18.
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

// The actor projection runs through the per-client compass transform matrix at
// 0x8246f308 (stride 0x44). If that matrix is stale/zero in follow (no menu
// open) the projected coords would be garbage, so bail rather than risk a bad
// draw -- a stale matrix then shows NOTHING instead of crashing, which itself
// tells us the matrix needs explicit setup.
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

// Register compass_map_<mapname> and cache the handle (re-register only when the
// map name changes). Returns 0 if unavailable -> caller skips the map draw.
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

// Draw the real map texture through the SAME projection + matrix the blips use.
// rect is the compassRectDef_s; horz/vertAlign come from it.
static void DrawNativeSpecMap(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    int handle = RegisterCompassMapMaterial();
    if (handle == 0)
        return;

    // client 0 (the host spectator). cgBase only used by the rotating branch;
    // full-map ignores it. matrix is the per-client compass transform.
    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float *matrix = reinterpret_cast<float *>(0x8246F308u);

    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    CompassProject_fn(mode, cgBase, scratch, rect, &a, &b, &c, &d);
    CompassMatrixXform_fn(matrix, &a, &b, &c, &d, horzAlign, vertAlign);

    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    R_DrawStretchPic_fn(a, b, c, d, 0.0f, 0.0f, 1.0f, 1.0f, white, handle);
}

// v21 caster radar. The native compass actor array is team-filtered to the
// followed player's view (teammates + UAV enemies), so it can't show everyone.
// Instead we take ALL players' world x/y from GSC via the "caster_blips" dvar
// (an "x y x y ..." string the spectator publishes every frame) and draw each
// as a uniform red dot, plus the followed player (cg+0x478e0/4) as a larger
// yellow marker. Each dot is projected with the mode-1 (static full-map) math
// reversed from FUN_82323740's else-branch:
//   u = (north.x*dx - north.y*dy) / worldW
//   v = -(north.y*dx + north.x*dy) / worldH        (dx,dy = pos - worldOrigin)
//   screen = projRect(ox,oy,ow,oh) -> (ox + u*ow, oy + v*oh)
// then pushed through the SAME CompassProject + CompassMatrixXform as the map,
// so dots land exactly on the map by construction (matches the native mode-1
// blip placement that was confirmed aligned). cg reads at client 0.
static int s_whiteMat = 0;

static void DrawCasterDots(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    if (s_whiteMat == 0)
        s_whiteMat = R_RegisterMaterial_fn(4, "white");
    if (s_whiteMat == 0)
        return;

    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float *matrix = reinterpret_cast<float *>(0x8246F308u);

    // Same projected rect the map uses (pre-matrix): ox,oy,ow,oh.
    float ox = 0.0f, oy = 0.0f, ow = 0.0f, oh = 0.0f;
    CompassProject_fn(mode, cgBase, scratch, rect, &ox, &oy, &ow, &oh);

    // Compass world basis: north vector, world origin, world size.
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

    // Followed player (cg+0x478e0/4) -> yellow, larger, drawn on top.
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

static void DrawNativeSpecCompass()
{
    if (compass_native == nullptr || !compass_native->current.enabled)
        return;
    // Only mid-follow (locked onto a player). Skips CHOOSE TEAM / spec UI.
    if (!ValidFollowedPlayer())
        return;
    if (!CompassMatrixReady())
        return;

    compassRectDef_s rect;
    rect.x = static_cast<float>(compass_native_x->current.integer);
    rect.y = static_cast<float>(compass_native_y->current.integer);
    rect.w = static_cast<float>(compass_native_size->current.integer);
    rect.h = static_cast<float>(compass_native_size->current.integer);
    rect.horzAlign = compass_native_align->current.integer;
    rect.vertAlign = compass_native_align->current.integer;

    // p3 scratch (unused in full-map mode; zeroed for the rotating path).
    compassRectDef_s scratch = {0.0f, 0.0f, 0.0f, 0.0f, 0, 0};

    int mode = compass_native_mode->current.integer;

    // Real native map background first, so blips/dots draw on top. Self-drawn
    // with our own material handle through the same projection -> aligned.
    if (compass_native_bg != nullptr && compass_native_bg->current.enabled)
        DrawNativeSpecMap(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);

    // p5 alpha is written by the draw fn, so refresh every frame.
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Caster radar: all players as red dots + yellow followed marker. Falls
    // back to the native team blips when caster_dots is off.
    if (caster_dots != nullptr && caster_dots->current.enabled)
        DrawCasterDots(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);
    else
        CompassDrawActors_fn(0, mode, &scratch, &rect, color);
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

    // compass_group0 is now inert (the field 6->0 force was retired -- it was
    // built on the disproven "82348BD0 is the map" theory). Left registered so
    // existing configs referencing it don't error; it no longer does anything.
    compass_group0 = Dvar_RegisterBool("compass_group0", false, 0, "Retired: legacy follow-spec HUD group force (no-op in v17)");

    // Native follow-spec compass (v17). Toggle + dvar-tunable placement so the
    // map box can be dialed in on Xenia with no rebuild.
    compass_native = Dvar_RegisterBool("compass_native", true, 0,
        "Follow-spec: draw native engine compass blips (both teams) via fn 82322868");
    // Native map background (v20): self-drawn compass_map_<mapname> through the
    // same projection+matrix as the blips. Safe -- we own the material handle.
    compass_native_bg = Dvar_RegisterBool("compass_native_bg", true, 0,
        "Follow-spec: draw the native compass map texture under the blips");
    compass_native_mode = Dvar_RegisterInt("compass_native_mode", 0, 0, 1, 0,
        "Native spec compass projection: 0=full map, 1=rotating");
    compass_native_x = Dvar_RegisterInt("compass_native_x", 16, -2000, 2000, 0,
        "Native spec compass rect X");
    compass_native_y = Dvar_RegisterInt("compass_native_y", 16, -2000, 2000, 0,
        "Native spec compass rect Y");
    compass_native_size = Dvar_RegisterInt("compass_native_size", 112, 8, 1024, 0,
        "Native spec compass rect size (square, w=h)");
    compass_native_align = Dvar_RegisterInt("compass_native_align", 0, 0, 7, 0,
        "Native spec compass rect horz/vert align enum (0-7)");

    // v21 caster radar: all players as red dots + yellow followed marker.
    // Player x/y are published by GSC into the "caster_blips" dvar each frame.
    caster_dots = Dvar_RegisterBool("caster_dots", true, 0,
        "Follow-spec: draw all players as red dots (caster radar) instead of native team blips");
    caster_dot_size = Dvar_RegisterInt("caster_dot_size", 6, 1, 64, 0,
        "Caster radar dot size (projected units)");

    // Build marker -- proves this cg.cpp compiled into the running codxe DLL.
    // Read back via `\compass_hook_v` in console.
    Dvar_RegisterInt("compass_hook_v", 21, 0, 100, 0, "Codxe compass hook build marker (v21 -- caster radar: all-player red dots + yellow followed marker over native map)");

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
            DrawNativeSpecCompass();

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
