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

// v65 class-menu cap chamfer flip. GSC sets this true while the GSC class
// menu is open; the R_DrawStretchPic detour then V-flips every
// button_highlight_end cap blit so its chamfer sits bottom-right (native
// X360) instead of top-right. Body rects use "white" and are untouched.
dvar_s *menu_capflip = nullptr;

// v67 class-menu faded row panels (native look, stage 1). GSC sets
// classmenu_panels while the class menu is open and publishes the selected
// index / row count; the DLL draws per-row gradient_fadein bars. Geometry is
// in GSC virtual 640x480 units (scaled by scaleVirtualToFull), all dvar-tunable
// so layout is a GSC redeploy, not a DLL rebuild.
dvar_s *classmenu_panels = nullptr;
dvar_s *classmenu_behind = nullptr; // v71: draw panels behind text (via cap-blit piggyback)
dvar_s *classmenu_cap = nullptr;   // v70 chamfered left cap on/off
dvar_s *classmenu_capw = nullptr;  // v70 cap width (per-mille screen W)
dvar_s *classmenu_caps = nullptr;  // v70 cap s-flip (mirror horizontally)
dvar_s *classmenu_capt = nullptr;  // v70 cap t-flip (mirror vertically)
dvar_s *classmenu_capright = nullptr; // v80 chamfer cap on the right (text) end vs left
dvar_s *classmenu_cr = nullptr;    // v70 panel red   (0-255)
dvar_s *classmenu_cg = nullptr;    // v70 panel green (0-255)
dvar_s *classmenu_cb = nullptr;    // v70 panel blue  (0-255)
dvar_s *classmenu_idx = nullptr;
dvar_s *classmenu_n = nullptr;
dvar_s *classmenu_sep = nullptr;
dvar_s *classmenu_px = nullptr;
dvar_s *classmenu_pw = nullptr;
dvar_s *classmenu_py = nullptr;
dvar_s *classmenu_pitch = nullptr;
dvar_s *classmenu_ph = nullptr;
dvar_s *classmenu_fade = nullptr;
dvar_s *classmenu_solid = nullptr;
dvar_s *classmenu_a0 = nullptr;
dvar_s *classmenu_a1 = nullptr;

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
dvar_s *caster_arrows = nullptr;
dvar_s *caster_font = nullptr;    // v51: stat-table font index (0-6)
dvar_s *caster_obj_size = nullptr; // v52: objective icon size, decoupled from dots
dvar_s *caster_arrow_friendly = nullptr; // v54: caster-map team arrow size
dvar_s *caster_arrow_player = nullptr;   // v54: caster-map followed-arrow size

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
// v28: takes the hooked scMap blit rect (final screen coords).
static void DrawNativeSpecCompass(float bx, float by, float bw, float bh);

// ============================================================================
// v45 caster stat columns. ZERO hudelems: GSC publishes 10 pipe-separated row
// strings ("k/d/p r.rr") in caster_stats, this draws them as screen text from
// OnCG_DrawActive (the NDIAG-proven path -- renders in plain follow-spec).
// Rows 0-4 = allies column (left of screen, right of the HP bars), rows 5-9 =
// axis column (left of the right-side bars). Empty field = row hidden. GSC
// publishes "" when the shoutcaster overlay is off, which is the on/off gate.
// Positions are dvars so alignment tuning is a GSC redeploy, not a rebuild.
// Coordinate space is the R_AddCmdDrawText space (~1024x768 virtual,
// calibrated against the NDIAG line at 10,150).
// ============================================================================
// v47: frame counter (ticks in OnCG_DrawActive) + the frame the caster
// map composite last drew. Stats render only while the map is live on
// screen, so they can never outlive the overlay (the death-screen /
// gameplay leak). Declared here, above DrawCasterStats, for /W4 /WX.
static unsigned int s_frameCounter = 0;
static unsigned int s_lastMapBlitFrame = 0;

// v49: the followed client slot, published for GSC every frame
// (caster_followed; -1 = none/freelook). Backs the round-end re-follow.
dvar_s *caster_followed = nullptr;

dvar_s *caster_stats_xl = nullptr;
dvar_s *caster_stats_xr = nullptr;
dvar_s *caster_stats_y = nullptr;
dvar_s *caster_stats_dy = nullptr;
dvar_s *caster_stats_dx = nullptr;  // v55: K->D column gap (text units)
dvar_s *caster_stats_hdr = nullptr; // v55: draw the Kills/Deaths header row
dvar_s *caster_hdr_scale = nullptr; // v56: header text scale, percent
dvar_s *caster_imginfo = nullptr;   // v58: image-spec probe (donor recon)

// v51: selectable stat-table font. Index -> stock IW3 font asset name;
// out-of-range falls back to 0. R_RegisterFont returning null (font
// asset absent in this build) also falls back to consoleFont so the
// table never draws with a null font handle. Cache is index-keyed and
// flushed by the v46 4s tick (same never-latch rule as materials).
static const char *const kCasterFonts[7] = {
    "fonts/consoleFont",  // 0
    "fonts/smallFont",    // 1
    "fonts/normalFont",   // 2
    "fonts/boldFont",     // 3
    "fonts/objectiveFont",// 4
    "fonts/bigFont",      // 5
    "fonts/extraBigFont", // 6
};
static Font_s *s_statFont = nullptr;
static int s_statFontIdx = -1;

static void DrawCasterStats()
{
    const char *s = Dvar_GetString("caster_stats");
    if (s == nullptr || *s == '\0')
        return;

    // v47: draw only while the caster map composited within the last
    // second -- stats track the overlay exactly, killing the brief leak
    // into gameplay/death screens.
    if (s_frameCounter - s_lastMapBlitFrame > 60u)
        return;

    // Same menu gate as the scMap blit swallow (v35): KEYCATCH_UI bit set =
    // a real engine menu is open, draw nothing over it.
    const uint32_t catchers = *reinterpret_cast<volatile uint32_t *>(0x82435a18u);
    if ((catchers & 0x10u) != 0)
        return;

    int fidx = (caster_font != nullptr) ? caster_font->current.integer : 0;
    if (fidx < 0 || fidx > 6)
        fidx = 0;
    if (s_statFont == nullptr || fidx != s_statFontIdx)
    {
        s_statFont = R_RegisterFont(kCasterFonts[fidx]);
        if (s_statFont == nullptr)
            s_statFont = R_RegisterFont("fonts/consoleFont");
        s_statFontIdx = fidx;
    }
    if (s_statFont == nullptr)
        return;
    Font_s *statFont = s_statFont;
    static const float statCol[4] = {1.0f, 1.0f, 1.0f, 0.9f};

    const float xl = static_cast<float>(caster_stats_xl->current.integer);
    const float xr = static_cast<float>(caster_stats_xr->current.integer);
    const float y0 = static_cast<float>(caster_stats_y->current.integer);
    const float dy = static_cast<float>(caster_stats_dy->current.integer);
    const float dx = static_cast<float>(
        (caster_stats_dx != nullptr) ? caster_stats_dx->current.integer : 45);

    // v55: header one row above both columns, same x anchors as the
    // sub-fields below, so it aligns by construction. v56: full words
    // at reduced scale (caster_hdr_scale percent) so they fit the
    // column footprint without widening dx.
    if (caster_stats_hdr == nullptr || caster_stats_hdr->current.enabled)
    {
        // v57: back to K / D -- the full words clipped the right-side
        // bars (v56 RETIRED before deploy). caster_hdr_scale kept.
        char hk[8] = "K";
        char hd[8] = "D";
        const float hs = static_cast<float>(
            (caster_hdr_scale != nullptr) ? caster_hdr_scale->current.integer : 75) / 100.0f;
        const float hy = y0 - dy;
        R_AddCmdDrawText(hk, 8, statFont, xl, hy, hs, hs, 0.0f, statCol, 0);
        R_AddCmdDrawText(hd, 8, statFont, xl + dx, hy, hs, hs, 0.0f, statCol, 0);
        R_AddCmdDrawText(hk, 8, statFont, xr, hy, hs, hs, 0.0f, statCol, 0);
        R_AddCmdDrawText(hd, 8, statFont, xr + dx, hy, hs, hs, 0.0f, statCol, 0);
    }

    int row = 0;
    const char *p = s;
    while (row < 10)
    {
        // Extract field [p, q) up to '|' or end.
        const char *q = p;
        while (*q != '\0' && *q != '|')
            ++q;

        if (q > p)
        {
            char field[48];
            int n = static_cast<int>(q - p);
            if (n > 47)
                n = 47;
            for (int i = 0; i < n; ++i)
                field[i] = p[i];
            field[n] = '\0';

            const float fx = (row < 5) ? xl : xr;
            const float fy = y0 + static_cast<float>(row % 5) * dy;
            // v55: "K D" sub-columns. GSC v102 publishes the row as
            // kills + space + deaths; split and column-place both.
            int sp = -1;
            for (int i = 0; i < n; ++i)
            {
                if (field[i] == ' ')
                {
                    sp = i;
                    break;
                }
            }
            if (sp > 0)
            {
                field[sp] = '\0';
                R_AddCmdDrawText(field, 48, statFont, fx, fy, 1.0f, 1.0f, 0.0f, statCol, 0);
                R_AddCmdDrawText(field + sp + 1, 48, statFont, fx + dx, fy, 1.0f, 1.0f, 0.0f, statCol, 0);
            }
            else
            {
                R_AddCmdDrawText(field, 48, statFont, fx, fy, 1.0f, 1.0f, 0.0f, statCol, 0);
            }
        }

        if (*q == '\0')
            break;
        p = q + 1;
        ++row;
    }
}

// ============================================================================
// v60: DLL-drawn HP bars. GSC hudelem bars RETIRED: the +10 cap elems of
// gsc v109 overflowed the 31-slot current team-hudelem pool (red names and
// the stats publisher died mid-creation, on-box). Zero hudelems here:
// GSC publishes caster_hp (10 pipe fields, int 1-100, empty = no bar) and
// caster_bar_rgb1/rgb2 ("r g b" floats). Geometry via caster_bar_* ints
// (render pixels), set by GSC like the caster_stats_* anchors.
// Body = material "white" stretched (sharp at any width); cap = donor
// materials rank_sgt1 (right bulge, allies) / rank_rec1 (left, axis),
// drawn minified -- the codxe image_loader replaces their pixels from
// images/*.dds at map load.
dvar_s *caster_bar_xl = nullptr;   // allies bar LEFT edge
dvar_s *caster_bar_xr = nullptr;   // axis bar RIGHT edge
dvar_s *caster_bar_y = nullptr;    // row 0 top
dvar_s *caster_bar_dy = nullptr;   // row step
dvar_s *caster_bar_w = nullptr;    // full-HP bar width
dvar_s *caster_bar_h = nullptr;    // bar height
dvar_s *caster_bar_cap = nullptr;  // cap piece width

static int s_barBodyMat = 0;
static int s_capMat = 0; // v65: button_highlight_end handle for the menu cap V-flip

// v61 build fix: DrawCasterBars sits above the compass-section
// definitions it uses. Forward-declare the resolver and hoist the
// R_DrawStretchPic pointer here (single definition; the compass
// section below now only carries the documenting comment).
static int ResolveMaterialStrict(const char *name);
// R_DrawStretchPic (8216BAE8): (x,y,w,h, s0,t0,s1,t1, color, material).
typedef void (*R_DrawStretchPic_fn_t)(float, float, float, float, float, float, float, float, const float *, int);
static R_DrawStretchPic_fn_t const R_DrawStretchPic_fn =
    reinterpret_cast<R_DrawStretchPic_fn_t>(0x8216BAE8u);

static void ParseRGB(const char *s, float *out)
{
    out[0] = out[1] = out[2] = 1.0f;
    if (s == nullptr || *s == '\0')
        return;
    float v[3] = {1.0f, 1.0f, 1.0f};
    int i = 0;
    const char *p = s;
    while (i < 3 && *p != '\0')
    {
        v[i] = static_cast<float>(atof(p));
        while (*p != '\0' && *p != ' ')
            ++p;
        while (*p == ' ')
            ++p;
        ++i;
    }
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
}

static void DrawCasterBars()
{
    const char *s = Dvar_GetString("caster_hp");
    if (s == nullptr || *s == '\0')
        return;
    // same overlay gates as the stat text (map recency + menu catcher)
    if (s_frameCounter - s_lastMapBlitFrame > 60u)
        return;
    const uint32_t catchers = *reinterpret_cast<volatile uint32_t *>(0x82435a18u);
    if ((catchers & 0x10u) != 0)
        return;

    if (s_barBodyMat == 0)
        s_barBodyMat = ResolveMaterialStrict("white");
    if (s_barBodyMat == 0)
        return;

    const float xl = static_cast<float>((caster_bar_xl != nullptr) ? caster_bar_xl->current.integer : 40);
    const float xr = static_cast<float>((caster_bar_xr != nullptr) ? caster_bar_xr->current.integer : 1240);
    const float y0 = static_cast<float>((caster_bar_y != nullptr) ? caster_bar_y->current.integer : 399);
    const float dy = static_cast<float>((caster_bar_dy != nullptr) ? caster_bar_dy->current.integer : 27);
    const float fw = static_cast<float>((caster_bar_w != nullptr) ? caster_bar_w->current.integer : 300);
    const float fh = static_cast<float>((caster_bar_h != nullptr) ? caster_bar_h->current.integer : 21);
    const float cw = static_cast<float>((caster_bar_cap != nullptr) ? caster_bar_cap->current.integer : 14);

    float col1[4]; float col2[4];
    ParseRGB(Dvar_GetString("caster_bar_rgb1"), col1);
    ParseRGB(Dvar_GetString("caster_bar_rgb2"), col2);
    col1[3] = 0.85f; col2[3] = 0.85f;

    int row = 0;
    const char *p = s;
    while (row < 10)
    {
        const char *q = p;
        while (*q != '\0' && *q != '|')
            ++q;
        if (q > p)
        {
            int hp = atoi(p);
            if (hp > 100) hp = 100;
            if (hp > 0)
            {
                const bool axis = (row >= 5);
                const float *col = axis ? col2 : col1;
                float len = fw * static_cast<float>(hp) / 100.0f;
                if (len < cw) len = cw;  // cap alone at minimum
                const float fy = y0 + static_cast<float>(row % 5) * dy;
                // v64: caps RETIRED (rounded capsule experiment abandoned
                // -- magnification/mirror churn not worth it). Plain flat
                // rectangle, "white" material, full bar length. Mirror
                // kept so each table hugs its outer edge: allies grow
                // right from xl, axis grow left from xr.
                (void)cw;
                if (!axis)
                    R_DrawStretchPic_fn(xl, fy, len, fh, 0.0f, 0.0f, 1.0f, 1.0f, col, s_barBodyMat);
                else
                    R_DrawStretchPic_fn(xr - len, fy, len, fh, 0.0f, 0.0f, 1.0f, 1.0f, col, s_barBodyMat);
            }
        }
        if (*q == '\0')
            break;
        p = q + 1;
        ++row;
    }
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
// (ValidFollowedPlayer removed in v27 -- falsified by NDIAG, see DrawNativeSpecCompass)

// Native compass actor-blip drawer. Function_82322868 loops the compass-actor
// array (24 slots, engine-filled every frame from the snapshot) and draws BOTH
// teams (friendly arrows + enemy dots). v24: this is the SHIPPING blip drawer,
// fired from R_DrawStretchPic_Hook.
typedef void (*CompassDrawActors_fn_t)(int, int, void *, void *, float *);
static CompassDrawActors_fn_t const CompassDrawActors_fn =
    reinterpret_cast<CompassDrawActors_fn_t>(0x82322868u);

// v39: native viewed-player arrow drawer (ownerdraw 0xb7 case in dispatcher
// 82307B28). Mode 1 projects the FOLLOWED position cg+0x478e0 through the
// engine's own projector (FUN_82323740), rotates by refdef yaw cg+0x4b964
// (per-frame smooth, unlike the snapshot-stepped entity yaw), sizes from the
// native compass dvars, draws via the icon wrapper 82318A88. Args mirror the
// dispatcher call: r3 client, r4 mode, r5 scratch, r6 rect, r7 material,
// r8 color (alpha read at +0xc).
typedef void (*CompassDrawPlayer_fn_t)(int, int, void *, void *, int, float *);
static CompassDrawPlayer_fn_t const CompassDrawPlayer_fn =
    reinterpret_cast<CompassDrawPlayer_fn_t>(0x82324C08u);

// R_RegisterMaterial(type, name) -> handle. type 4 = 2D/UI material.
typedef int (*R_RegisterMaterial_fn_t)(int, const char *);
static R_RegisterMaterial_fn_t const R_RegisterMaterial_fn =
    reinterpret_cast<R_RegisterMaterial_fn_t>(0x822A0298u);

// v41: STRICT material resolve. The v36 sentinel is RETIRED -- evidence
// (default-material blobs at Crash bomb sites, wrong player icon) shows
// R_RegisterMaterial creates a DISTINCT default-backed handle per unknown
// name, so it matches neither 0 nor a probe sentinel. Instead verify the
// resolved Material's own name: iw3 Material.info.name sits at +0; a
// default-backed material carries its own name, not the requested one.
static int ResolveMaterialStrict(const char *name)
{
    const int h = R_RegisterMaterial_fn(4, name);
    if (h == 0)
        return 0;
    const char *matName = *reinterpret_cast<const char *const volatile *>(h);
    if (matName == nullptr || strcmp(matName, name) != 0)
        return 0;
    return h;
}
// Compass projection (82323E28): fills oA,oB,oC,oD = x,y,w,h.
typedef void (*CompassProject_fn_t)(int, int, void *, void *, float *, float *, float *, float *);
static CompassProject_fn_t const CompassProject_fn =
    reinterpret_cast<CompassProject_fn_t>(0x82323E28u);
// Per-client compass matrix transform (822b36e8).
typedef void (*CompassMatrixXform_fn_t)(float *, float *, float *, float *, float *, int, int);
static CompassMatrixXform_fn_t const CompassMatrixXform_fn =
    reinterpret_cast<CompassMatrixXform_fn_t>(0x822B36E8u);
// R_DrawStretchPic pointer: HOISTED above DrawCasterBars (v61 build
// fix) -- single definition now lives there.

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

// v46: ALL material-handle caches are process-lifetime statics, but
// material handles are per-map-load pointers. After a map change the old
// pointers dangle; on Xenia the deterministic allocator often hands the
// same addresses back (so stale handles "work"), until a different map
// order shifts the pool -- then every cached handle is garbage, the blit
// match never fires again, and the whole overlay is dead until process
// restart. That is the "infected session" / "random per-map" failure
// class. Fix: a 4s refresh tick (in OnCG_DrawActive) zeroes every cache
// and lazy re-resolution repopulates them. Worst case: icons blank for a
// few seconds after map load.
static char s_mapMatLastName[160] = {0};
static int s_mapMatHandle = 0;
static int s_pingMat = 0;           // hoisted from DrawEnemyDots (v46)
static int s_playerArrowMat = -2;   // hoisted; -2 unprobed, 0 none found

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

    bool same = (s_mapMatHandle != 0);
    for (int k = 0; same && k <= i; ++k)
        if (s_mapMatLastName[k] != name[k])
            same = false;
    if (same)
        return s_mapMatHandle;

    s_mapMatHandle = R_RegisterMaterial_fn(4, name);
    for (int k = 0; k <= i; ++k)
        s_mapMatLastName[k] = name[k];
    return s_mapMatHandle;
}

static void DrawNativeSpecMap(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    s_lastMapBlitFrame = s_frameCounter; // v47: overlay is live this frame
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

// v32: native-styled arrows for our own blips. The engine's per-actor icon
// chain bottoms out in Function_8216B420(corners[8], color[4], material):
// four screen-space corner pairs, color, material -- rotation is just corner
// math, done by the caller (82318A88 via sin/cos). We call the bottom of the
// chain directly with the engine's own arrow material
// ("compassping_friendly_mp", the actor icon; its firing/yelling variants are
// the overlay handles the drawer uses) and OUR color: red for enemies,
// yellow for the followed player. NOTE: the old "compassping_* draws blank"
// retirement was about GSC setShader (hudelem shader pool); DLL-side
// R_RegisterMaterial is the path compass_map_* already resolves through.
typedef void (*R_DrawRotQuad_fn_t)(const float *corners8, const float *color4, int material);
static R_DrawRotQuad_fn_t const R_DrawRotQuad_fn =
    reinterpret_cast<R_DrawRotQuad_fn_t>(0x8216B420u);

static int s_arrowMat = 0;

// yawDeg: actor world yaw. Screen rotation = mapNorthRef (cg+0x4e470, the
// same reference the engine drawer subtracts actor yaw from) minus yawDeg.
// ASSUMPTION flagged: corner order TL,TR,BR,BL -- if the arrow renders
// mirrored/flipped in-game, the order is permuted (cosmetic, one-line fix).
static void DrawNativeArrow(float cx, float cy, float size, float yawDeg, const float *color)
{
    if (s_arrowMat == 0)
        s_arrowMat = R_RegisterMaterial_fn(4, "compassping_friendly_mp");
    if (s_arrowMat == 0)
        return;

    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    float northRef = *reinterpret_cast<volatile float *>(cgBase + 0x4e470);

    const float deg2rad = 0.017453292f;
    float a = (northRef - yawDeg) * deg2rad;
    float sa = sinf(a);
    float ca = cosf(a);
    float h = size * 0.5f;

    // unrotated corners relative to center: TL(-h,-h) TR(h,-h) BR(h,h) BL(-h,h)
    float c[8];
    c[0] = cx + (-h * ca - -h * sa); c[1] = cy + (-h * sa + -h * ca);
    c[2] = cx + ( h * ca - -h * sa); c[3] = cy + ( h * sa + -h * ca);
    c[4] = cx + ( h * ca -  h * sa); c[5] = cy + ( h * sa +  h * ca);
    c[6] = cx + (-h * ca -  h * sa); c[7] = cy + (-h * sa +  h * ca);

    R_DrawRotQuad_fn(c, color, s_arrowMat);
}

// v54: caster-only native arrow sizing. v99/v100 set the cg_hudMap*
// engine dvars from GSC, which leaked the big icons into every other
// mode-1 surface (they are global client dvars). Instead the size
// values are swapped in ONLY around our two native draw calls and
// restored immediately after: the mode-1 size code reads the dvar
// value (+0xc) through four fixed pointer slots -- actors w/h via
// 0x823E0418/0x823E03EC (823239A8 mode!=0), player arrow w/h via
// 0x823E03CC/0x823E0464 (82324C08 mode 1). Same-thread, same-call
// save/write/restore: no other surface can observe the big values.
static const uint32_t kArrowDvarSlots[4] = {
    0x823E0418u, 0x823E03ECu, // actor blip w, h
    0x823E03CCu, 0x823E0464u, // player arrow w, h
};

static float ArrowSizePush(int slot, float newVal)
{
    const uint32_t dv = *reinterpret_cast<volatile uint32_t *>(kArrowDvarSlots[slot]);
    if (dv == 0)
        return -1.0f;
    volatile float *val = reinterpret_cast<volatile float *>(dv + 0xc);
    const float old = *val;
    *val = newVal;
    return old;
}

static void ArrowSizePop(int slot, float oldVal)
{
    if (oldVal < 0.0f)
        return;
    const uint32_t dv = *reinterpret_cast<volatile uint32_t *>(kArrowDvarSlots[slot]);
    if (dv != 0)
        *reinterpret_cast<volatile float *>(dv + 0xc) = oldVal;
}

static void DrawCasterDotsFrom(const char *blipDvar, const float *dotColor, bool nativePing,
                               int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
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

    // v31: the dvar carries CLIENT IDS, not positions. GSC keeps the slow,
    // authoritative filtering (team / alive via sessionstate+health, 4Hz);
    // positions are resolved HERE every frame from cg_entities (base ptr at
    // 0x823F5054[client0], stride 0x1d0, origin x/y at +0x1c/+0x20, entity
    // type at +0xc4 == 1 for players; player entityNum == clientNum). This
    // removes the 4Hz dot tick -- dots now move per frame like the arrows.
    const int entBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F5054u));
    if (entBase == 0)
        return;

    const char *b = Dvar_GetString(blipDvar);
    while (b != nullptr && *b != '\0')
    {
        int id = atoi(b);
        while (*b != '\0' && *b != ' ')
            ++b;
        while (*b == ' ')
            ++b;
        if (id < 0 || id > 63)
            continue;
        const int ent = entBase + id * 0x1d0;
        if (*reinterpret_cast<volatile int *>(ent + 0xc4) != 1)
            continue;
        float px = *reinterpret_cast<volatile float *>(ent + 0x1c);
        float py = *reinterpret_cast<volatile float *>(ent + 0x20);

        float dx = px - wOx, dy = py - wOy;
        float u = (nX * dx - nY * dy) / wW;
        float v = -(nY * dx + nX * dy) / wH;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            continue;

        // v37: the game's own red ping texture ("compassping_enemy", in the
        // xex string table) instead of a flat tinted square -- the native
        // UAV-dot look. Texture carries its own red; tint stays the caller's
        // color so the rare freecam ally pass can still tint. Soft-edged
        // texture reads smaller than its quad, so 2x the square size.
        // Sentinel-guarded; flat square is the fallback, never a yellow blob.
        if (s_pingMat == 0)
        {
            const int m = ResolveMaterialStrict("compassping_enemy");
            s_pingMat = (m != 0) ? m : s_whiteMat;
        }
        const bool usePing = nativePing && s_pingMat != s_whiteMat;
        const int dotMat = usePing ? s_pingMat : s_whiteMat;
        const float sz = usePing ? ds * 2.0f : ds;
        float acx = ox + u * ow, acy = oy + v * oh;
        float qx = acx - sz * 0.5f, qy = acy - sz * 0.5f, qw = sz, qh = sz;
        CompassMatrixXform_fn(matrix, &qx, &qy, &qw, &qh, horzAlign, vertAlign);
        static const float pingWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float *tint = usePing ? pingWhite : dotColor;
        R_DrawStretchPic_fn(qx, qy, qw, qh, 0.0f, 0.0f, 1.0f, 1.0f, tint, dotMat);
    }

    float fdx = *reinterpret_cast<volatile float *>(cgBase + 0x478e0) - wOx;
    float fdy = *reinterpret_cast<volatile float *>(cgBase + 0x478e4) - wOy;
    float fu = (nX * fdx - nY * fdy) / wW;
    float fv = -(nY * fdx + nX * fdy) / wH;
    if (fu >= 0.0f && fu <= 1.0f && fv >= 0.0f && fv <= 1.0f)
    {
        float yellow[4] = {1.0f, 0.85f, 0.1f, 1.0f};
        float fcx = ox + fu * ow, fcy = oy + fv * oh;
        const bool wantArrow = (caster_arrows != nullptr && caster_arrows->current.enabled);

        // v39: NATIVE viewed-player arrow -- the exact menu-compass look,
        // engine-projected and refdef-yaw-rotated every frame. The arrow
        // material is menu-defined (no name in the xex string table), so
        // probe candidates once, sentinel-guarded; white tint preserves the
        // texture's own yellow. All probes missing -> the previous
        // hand-rotated arrow stays as fallback.
        // v41: probe list RETIRED (one of the loose names resolved to a
        // wrong/default-backed material -> "unidentified icon"). Single
        // known material: compassping_player, the stock minimap self-arrow
        // (white texture), strict-verified by name, tinted yellow.
        if (s_playerArrowMat == -2)
            s_playerArrowMat = ResolveMaterialStrict("compassping_player");

        if (wantArrow && s_playerArrowMat != 0)
        {
            static float arrowYellow[4] = {1.0f, 0.85f, 0.1f, 1.0f};
            arrowYellow[3] = 1.0f; // alpha read at +0xc; mode 1 skips the fade
            const float ps = static_cast<float>(
                (caster_arrow_player != nullptr) ? caster_arrow_player->current.integer : 44);
            const float opw = ArrowSizePush(2, ps);
            const float oph = ArrowSizePush(3, ps);
            CompassDrawPlayer_fn(0, mode, scratch, rect, s_playerArrowMat, arrowYellow);
            ArrowSizePop(2, opw);
            ArrowSizePop(3, oph);
        }
        else if (wantArrow)
        {
            // fallback: snapshot-yaw hand-rotated arrow (pre-v39 path)
            int snap2 = *reinterpret_cast<volatile int *>(cgBase + 0x24);
            int vc = (snap2 != 0) ? *reinterpret_cast<volatile int *>(snap2 + 0xec) : -1;
            float fyaw = 0.0f;
            bool haveYaw = false;
            if (vc >= 0 && vc <= 63 && entBase != 0)
            {
                const int vent = entBase + vc * 0x1d0;
                if (*reinterpret_cast<volatile int *>(vent + 0xc4) == 1)
                {
                    fyaw = *reinterpret_cast<volatile float *>(vent + 0x44);
                    haveYaw = true;
                }
            }
            if (haveYaw)
            {
                DrawNativeArrow(fcx, fcy, ds * 2.6f, fyaw, yellow);
            }
            else
            {
                float fs = ds * 1.6f;
                float fqx = fcx - fs * 0.5f, fqy = fcy - fs * 0.5f, fqw = fs, fqh = fs;
                CompassMatrixXform_fn(matrix, &fqx, &fqy, &fqw, &fqh, horzAlign, vertAlign);
                R_DrawStretchPic_fn(fqx, fqy, fqw, fqh, 0.0f, 0.0f, 1.0f, 1.0f, yellow, s_whiteMat);
            }
        }
    }
}

static void DrawCasterDots(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    DrawCasterDotsFrom("caster_blips", red, true, mode, scratch, rect, horzAlign, vertAlign);
}

// v29 enemy radar layer. The engine fills the 24-slot compass actor array from
// the FOLLOWED player's compass view: teammates always, enemies only under
// UAV (see the 82322868 annotation). Forcing g_compassShowEnemies is a
// server-wide radar leak (retired, v52). So the enemy team is drawn by us:
// GSC publishes both teams' world positions (caster_blips_al / caster_blips_ax,
// refreshed each radar tick), and we pick the list OPPOSITE to the followed
// player's team -- read from the same per-client clientinfo block the native
// drawer gates on: clientIdx = *(cg+0x24)+0xec, team = clientinfo+0xe8f84
// (stride 0x4e4 from cg base; 1=axis, 2=allies, 0/3 invalid).
static void DrawEnemyDots(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
    int snap = *reinterpret_cast<volatile int *>(cgBase + 0x24);
    if (snap == 0)
        return;
    int clientIdx = *reinterpret_cast<volatile int *>(snap + 0xec);
    if (clientIdx < 0 || clientIdx > 63)
        return;
    int info = clientIdx * 0x4e4 + cgBase;
    int team = 0;
    if (*reinterpret_cast<volatile int *>(info + 0xe8f58) != 0)
        team = *reinterpret_cast<volatile int *>(info + 0xe8f84);

    static const float red[4] = {1.0f, 0.15f, 0.15f, 1.0f};
    static const float green[4] = {0.25f, 0.9f, 0.3f, 1.0f};

    if (team == 1)      // following axis -> enemies are allies
    {
        DrawCasterDotsFrom("caster_blips_al", red, true, mode, scratch, rect, horzAlign, vertAlign);
    }
    else if (team == 2) // following allies -> enemies are axis
    {
        DrawCasterDotsFrom("caster_blips_ax", red, true, mode, scratch, rect, horzAlign, vertAlign);
    }
    else
    {
        // v30: freecam (viewed client is the spectating host, team 0/3). The
        // native drawer's own gate blanks the arrows here BY DESIGN -- there
        // is no followed compass view to fill the actor array from. So in
        // freecam we draw BOTH teams from the GSC blip feed: allies green,
        // axis red. Following someone returns to native arrows + enemy reds.
        DrawCasterDotsFrom("caster_blips_al", green, false, mode, scratch, rect, horzAlign, vertAlign);
        DrawCasterDotsFrom("caster_blips_ax", red, true, mode, scratch, rect, horzAlign, vertAlign);
    }
}

// v34 objective icons. GSC publishes "x y material " triples in caster_objs
// once per map (bomb sites / dom flags / hq radios; compass_waypoint_*
// materials are precached by the gametype scripts, so R_RegisterMaterial
// resolves them by name -- a wrong name returns 0 and that icon is silently
// skipped). Projection identical to the player blips.
static void DrawObjectiveIcons(int mode, void *scratch, void *rect, int horzAlign, int vertAlign)
{
    int cgBase = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));

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

    float *matrix = reinterpret_cast<float *>(0x8246F308u);
    // v52: decoupled from caster_dot_size (was x2.6 of it -- the v98 dot
    // bump ballooned every objective icon incl. the bomb). Absolute size.
    const float iconSize = static_cast<float>(
        (caster_obj_size != nullptr) ? caster_obj_size->current.integer : 24);
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    const char *b = Dvar_GetString("caster_objs");
    while (b != nullptr && *b != '\0')
    {
        float px = static_cast<float>(atof(b));
        while (*b != '\0' && *b != ' ') ++b;
        while (*b == ' ') ++b;
        if (*b == '\0') break;
        float py = static_cast<float>(atof(b));
        while (*b != '\0' && *b != ' ') ++b;
        while (*b == ' ') ++b;
        if (*b == '\0') break;
        char name[64];
        int k = 0;
        while (*b != '\0' && *b != ' ' && k < 63)
            name[k++] = *b++;
        name[k] = '\0';
        while (*b == ' ') ++b;

        // v44: a bare "_a"/"_b" token is an SD site label (gsc v78+); pick
        // the shield per the FOLLOWED team like the native compass does --
        // red target when following the attacking team, green defend
        // otherwise (incl. freecam). caster_atk carries the engine team
        // name of the attackers; clientinfo team ints are 1=axis 2=allies.
        char composed[64];
        const char *drawName = name;

        // v50: '&<entnum>' = bomb tracked at a LIVE entity (the carrier).
        // The published x/y are only a fallback -- the entity position is
        // read here EVERY FRAME from cg_entities (same base/stride/type
        // check as the blips: 0x823F5054, stride 0x1d0, origin +0x1c/
        // +0x20, type +0xc4 == 1), so the icon moves as smoothly as the
        // arrows instead of teleporting on the 1s GSC republish.
        if (name[0] == '&')
        {
            int en = 0;
            for (int d = 1; name[d] >= '0' && name[d] <= '9'; ++d)
                en = en * 10 + (name[d] - '0');
            const int entBase2 = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F5054u));
            if (entBase2 != 0 && en >= 0 && en <= 63)
            {
                const int ent = entBase2 + en * 0x1d0;
                if (*reinterpret_cast<volatile int *>(ent + 0xc4) == 1)
                {
                    px = *reinterpret_cast<volatile float *>(ent + 0x1c);
                    py = *reinterpret_cast<volatile float *>(ent + 0x20);
                }
            }
            drawName = "waypoint_bomb";
        }

        // v47: '!' prefix = PLANTED site label from the sd publisher
        // ("!a"/"!b"). Per the followed team, like the bare-label path:
        // following the attacking team -> green lettered DEFEND (they
        // protect the bomb), otherwise -> red lettered DEFUSE. Both
        // material families precached by sd.gsc.
        bool plantedTok = (name[0] == '!');
        // v48/v49: team tokens. First char picks the pair, the digit is
        // the relevant team's clientinfo int (1=axis, 2=allies), then an
        // optional letter. Following that team -> first material, else
        // second. All on the waypoint_* art family -- every variant is
        // precached by its own gametype (the compass_* lettered twins
        // are NOT, e.g. dom never precaches compass capture letters).
        //   '@' owned site (sab):    defend / target
        //   '#' owned site (koth):   defend / capture
        //   '$' owned flag (dom):    defend / captureneutral
        //   '%' planted-by (sab):    defend / defuse
        // Freecam/invalid tm takes the second (enemy-view) material.
        const bool ownedTok = (name[0] == '@' || name[0] == '#' || name[0] == '$' || name[0] == '%');
        if (plantedTok || ownedTok || name[0] == '_')
        {
            int atkTeam = 0;
            const char *atk = Dvar_GetString("caster_atk");
            if (atk != nullptr)
            {
                if (atk[0] == 'a' && atk[1] == 'x')
                    atkTeam = 1;
                else if (atk[0] == 'a' && atk[1] == 'l')
                    atkTeam = 2;
            }
            int tm = 0;
            {
                const int snapO = *reinterpret_cast<volatile int *>(cgBase + 0x24);
                if (snapO != 0)
                {
                    const int ciO = *reinterpret_cast<volatile int *>(snapO + 0xec);
                    if (ciO >= 0 && ciO <= 63)
                    {
                        const int infoO = ciO * 0x4e4 + cgBase;
                        if (*reinterpret_cast<volatile int *>(infoO + 0xe8f58) != 0)
                            tm = *reinterpret_cast<volatile int *>(infoO + 0xe8f84);
                    }
                }
            }
            const bool followingAttack = (atkTeam != 0 && tm == atkTeam);
            if (ownedTok)
            {
                const int ownerTeam = name[1] - '0';
                const bool owner = (ownerTeam == 1 || ownerTeam == 2) && (tm == ownerTeam);
                const char *base = "waypoint_defend";
                if (!owner)
                {
                    if (name[0] == '@')
                        base = "waypoint_target";
                    else if (name[0] == '#')
                        base = "waypoint_capture";
                    else if (name[0] == '$')
                        base = "waypoint_captureneutral";
                    else
                        base = "waypoint_defuse";
                }
                if (name[2] != '\0')
                    sprintf_s(composed, sizeof(composed), "%s_%s", base, name + 2);
                else
                    sprintf_s(composed, sizeof(composed), "%s", base);
            }
            else if (plantedTok)
            {
                char lbl[8];
                lbl[0] = '_';
                int li = 0;
                for (; name[1 + li] != '\0' && li < 6; ++li)
                    lbl[1 + li] = name[1 + li];
                lbl[1 + li] = '\0';
                // v49: waypoint_* family (defend_a/_b, defuse_a/_b are
                // precached by sd.gsc; the compass_* twins also are, but
                // one family keeps the map art consistent).
                sprintf_s(composed, sizeof(composed), "%s%s",
                          followingAttack ? "waypoint_defend" : "waypoint_defuse", lbl);
            }
            else
            {
                sprintf_s(composed, sizeof(composed), "%s%s",
                          followingAttack ? "compass_waypoint_target" : "compass_waypoint_defend", name);
            }
            drawName = composed;
        }

        // v41: strict name-verified resolve (v36 sentinel retired -- it
        // never matched, see ResolveMaterialStrict).
        const int mat = ResolveMaterialStrict(drawName);
        if (mat == 0)
            continue;

        float dx = px - wOx, dy = py - wOy;
        float u = (nX * dx - nY * dy) / wW;
        float v = -(nY * dx + nX * dy) / wH;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            continue;

        float qx = ox + u * ow - iconSize * 0.5f, qy = oy + v * oh - iconSize * 0.5f;
        float qw = iconSize, qh = iconSize;
        CompassMatrixXform_fn(matrix, &qx, &qy, &qw, &qh, horzAlign, vertAlign);
        R_DrawStretchPic_fn(qx, qy, qw, qh, 0.0f, 0.0f, 1.0f, 1.0f, white, mat);
    }
}

// v28 geometry fix. v27 arrows drew but landed outside the scMap: the rect
// dvars went through hudelem-independent placement (virtual coords + align
// enum) while the scMap hudelem has its own placement -- two pipelines, two
// rects. Ghidra: FUN_822b36e8 align case 5 is a pure identity (skips every
// transform), and the R_DrawStretchPic hook already receives the scMap blit's
// FINAL screen rect. So we feed that rect straight through with align 5/5 --
// map and arrows then share one transform chain and land inside the scMap
// quad by construction. CompassProject (82323E28) aspect-fits the world
// aspect into the rect + insets the border; that reshape applies identically
// to the native map bg and the arrows, so they stay mutually aligned. The
// compass_native_x/y/size/align dvars are inert as of v28.
static void DrawNativeSpecCompass(float bx, float by, float bw, float bh)
{
    if (compass_native == nullptr || !compass_native->current.enabled)
        return;
    // v27: ValidFollowedPlayer gate REMOVED. NDIAG h=213 f=213 m=0 d=0 while
    // actively following proved the 0x849F3664 followedSlot chain is wrong
    // (hypothesis from a prior session, never validated). The gate guarded
    // nothing: the draw never uses the followed slot -- CompassDrawActors
    // reads the engine-filled snapshot actor array with its own guards.
    if (!CompassMatrixReady())
        return;

    compassRectDef_s rect;
    rect.x = bx;
    rect.y = by;
    rect.w = bw;
    rect.h = bh;
    rect.horzAlign = 5; // identity placement (FUN_822b36e8 case 5)
    rect.vertAlign = 5;

    compassRectDef_s scratch = {0.0f, 0.0f, 0.0f, 0.0f, 0, 0};

    int mode = compass_native_mode->current.integer;

    if (compass_native_bg != nullptr && compass_native_bg->current.enabled)
        DrawNativeSpecMap(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (caster_dots != nullptr && caster_dots->current.enabled)
    {
        DrawCasterDots(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);
    }
    else
    {
        // v54 fix: the original else-body was braceless; the swap
        // expansion needs its own scope.
        const float fs = static_cast<float>(
            (caster_arrow_friendly != nullptr) ? caster_arrow_friendly->current.integer : 32);
        const float ofw = ArrowSizePush(0, fs);
        const float ofh = ArrowSizePush(1, fs);
        CompassDrawActors_fn(0, mode, &scratch, &rect, color);
        ArrowSizePop(0, ofw);
        ArrowSizePop(1, ofh);
    }

    // v34: objective icons under the player blips.
    DrawObjectiveIcons(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);

    // v29: enemy team as red dots, composited over the native arrows.
    DrawEnemyDots(mode, &scratch, &rect, rect.horzAlign, rect.vertAlign);

}

// --- v67: class-menu faded row panels (stage 1) ------------------------------
// Native CHOOSE-TEAM look: each row is a gradient_fadein bar (opaque toward the
// text edge, fading off the other way), the selected row brighter, with a thin
// separator after a configurable row. GSC publishes the live state via dvars;
// geometry is GSC virtual 640x480 units scaled to full by scaleVirtualToFull.
//
// Stage 1 draws from OnCG_DrawActive, which runs AFTER the hud pass, so these
// land ON TOP of the GSC row text -- hence the deliberately low default alpha so
// the text still reads through. Stage 2 moves the draw behind the text by
// piggybacking the menu's background blit in the R_DrawStretchPic detour.
static int s_gradMat = 0;

static void DrawClassMenuPanels()
{
    if (classmenu_panels == nullptr || !classmenu_panels->current.enabled)
        return;

    if (s_gradMat == 0)
    {
        const int g = ResolveMaterialStrict("gradient_fadein");
        s_gradMat = (g != 0) ? g : s_whiteMat; // white fallback so the body still shows
    }
    if (s_whiteMat == 0)
        s_whiteMat = R_RegisterMaterial_fn(4, "white");
    if (s_gradMat == 0 || s_whiteMat == 0)
        return;

    // v68: geometry in per-mille of the real screen (0..1000), NOT 640x480
    // virtual. The GSC menu hud maps x at ~half scaleVirtualToFull[0], so a
    // shared virtual basis put the panels ~2x too wide / too far right. Screen
    // fractions decouple us from the GSC hud scale entirely -- we just dial the
    // panels onto wherever the GSC text lands.
    const float scrW = scrPlaceFullUnsafe.scaleVirtualToFull[0] * 640.0f;
    const float scrH = scrPlaceFullUnsafe.scaleVirtualToFull[1] * 480.0f;

    const float pxR   = ((classmenu_px    != nullptr) ? classmenu_px->current.integer    : 244) / 1000.0f * scrW; // right edge
    const float pw    = ((classmenu_pw    != nullptr) ? classmenu_pw->current.integer    : 175) / 1000.0f * scrW; // width
    const float pyT   = ((classmenu_py    != nullptr) ? classmenu_py->current.integer    : 360) / 1000.0f * scrH; // first row top
    const float pitch = ((classmenu_pitch != nullptr) ? classmenu_pitch->current.integer :  34) / 1000.0f * scrH;
    const float ph    = ((classmenu_ph    != nullptr) ? classmenu_ph->current.integer    :  28) / 1000.0f * scrH;

    int n = (classmenu_n != nullptr) ? classmenu_n->current.integer : 7;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    const int idx = (classmenu_idx != nullptr) ? classmenu_idx->current.integer : -1;
    const int sep = (classmenu_sep != nullptr) ? classmenu_sep->current.integer : 4;
    int solidPct = (classmenu_solid != nullptr) ? classmenu_solid->current.integer : 60;
    if (solidPct < 0) solidPct = 0; if (solidPct > 100) solidPct = 100;
    const bool flip = (classmenu_fade != nullptr) && classmenu_fade->current.enabled;
    const float a0 = ((classmenu_a0 != nullptr) ? classmenu_a0->current.integer : 45) / 100.0f;
    const float a1 = ((classmenu_a1 != nullptr) ? classmenu_a1->current.integer : 80) / 100.0f;

    const float solidW  = pw * (solidPct / 100.0f); // opaque body, right portion
    const float pxL     = pxR - pw;                 // panel left edge
    const float pxBody  = pxR - solidW;             // solid left edge = fade right edge
    // fade strip: transparent at far-left, opaque where it meets the solid body.
    const float s0 = flip ? 0.0f : 1.0f; // far-left sample
    const float s1 = flip ? 1.0f : 0.0f; // meets-the-body sample

    // v80: native row bars cut the TOP-RIGHT corner at the text (right) end and
    // fade out on the LEFT (far end) -- the mirror of the header's far-end cut.
    // The chamfer cap (button_highlight_end, the same donor native uses) sits on
    // the RIGHT so it caps the SOLID part of the bar (no floating tab on the
    // faded end); the body is pulled back by capW so the cap forms the chamfered
    // end, and the fade strip fills the whole left portion. classmenu_capright
    // toggles the side; caps/capt still orient which corner the chamfer cuts.
    const bool  capOn = (classmenu_cap == nullptr) || classmenu_cap->current.enabled;
    if (capOn && s_capMat == 0)
        s_capMat = ResolveMaterialStrict("button_highlight_end");
    const float capW  = (capOn && s_capMat != 0)
        ? (((classmenu_capw != nullptr) ? classmenu_capw->current.integer : 5) / 1000.0f * scrW) : 0.0f;
    const bool  capRight = (classmenu_capright == nullptr) || classmenu_capright->current.enabled;
    const bool  capSFlip = (classmenu_caps == nullptr) || classmenu_caps->current.enabled;
    const bool  capTFlip = (classmenu_capt != nullptr) && classmenu_capt->current.enabled;
    const float cs0 = capSFlip ? 1.0f : 0.0f, cs1 = capSFlip ? 0.0f : 1.0f;
    const float ct0 = capTFlip ? 1.0f : 0.0f, ct1 = capTFlip ? 0.0f : 1.0f;
    // x-layout depends on which end carries the cap.
    float capX, bodyX, bodyW, fadeX, fadeW;
    if (capRight)
    {
        capX  = pxR - capW;                 // cap at the right (text) end
        bodyX = pxBody;
        bodyW = (pxR - capW) - pxBody;       // body stops short for the cap
        fadeX = pxL;                         // fade fills the whole left portion
        fadeW = pxBody - pxL;
    }
    else
    {
        capX  = pxL;                         // legacy: cap at the left end
        bodyX = pxBody;
        bodyW = solidW;
        fadeX = pxL + capW;
        fadeW = pxBody - fadeX;
    }
    if (bodyW < 0.0f) bodyW = 0.0f;

    const float cr = ((classmenu_cr != nullptr) ? classmenu_cr->current.integer : 150) / 255.0f;
    const float cg = ((classmenu_cg != nullptr) ? classmenu_cg->current.integer : 165) / 255.0f;
    const float cb = ((classmenu_cb != nullptr) ? classmenu_cb->current.integer : 175) / 255.0f;
    float col[4] = {cr, cg, cb, 1.0f};
    for (int i = 0; i < n; ++i)
    {
        const float topY = pyT + i * pitch;
        col[3] = (i == idx) ? a1 : a0;
        if (bodyW > 0.5f)
            R_DrawStretchPic_fn(bodyX, topY, bodyW, ph, 0.0f, 0.0f, 1.0f, 1.0f, col, s_whiteMat);
        if (fadeW > 0.5f)
            R_DrawStretchPic_fn(fadeX, topY, fadeW, ph, s0, 0.0f, s1, 1.0f, col, s_gradMat);
        if (capW > 0.5f)
            R_DrawStretchPic_fn(capX, topY, capW, ph, cs0, ct0, cs1, ct1, col, s_capMat);
    }

    if (sep >= 0 && sep < n - 1)
    {
        const float midY = pyT + sep * pitch + ph + (pitch - ph) * 0.5f;
        const float sh = (scrH / 480.0f) * 2.0f; // ~2 virtual px tall
        float scol[4] = {cr, cg, cb, a0 * 0.8f};
        if (bodyW > 0.5f)
            R_DrawStretchPic_fn(bodyX, midY - sh * 0.5f, bodyW, sh, 0.0f, 0.0f, 1.0f, 1.0f, scol, s_whiteMat);
        if (fadeW > 0.5f)
            R_DrawStretchPic_fn(fadeX, midY - sh * 0.5f, fadeW, sh, s0, 0.0f, s1, 1.0f, scol, s_gradMat);
    }
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
    // v65: class-menu cap chamfer V-flip. The GSC highlight/header/back caps
    // use button_highlight_end with the chamfer on the texture's TOP edge;
    // native X360 menus put it on the BOTTOM edge. GSC setshader cannot flip a
    // hud shader (negative dims ignored on the hudelem path -- RETIRED), but
    // every GSC hudelem shader blit reaches THIS function and it carries
    // explicit texcoords. So when GSC says the class menu is open
    // (menu_capflip) and the blit is the cap material, swap the two t values
    // (t0<->t1) to mirror the texture vertically. s0/s1 untouched -> chamfer
    // stays on the RIGHT, only moves top->bottom. Body rects use "white"
    // (different material) and pass through unchanged.
    //
    // v66: only flip the SLIM/TALL caps (header 10x38, selection 8x26). The
    // bottom Back-bar cap is WIDE (76x58) and flipping it inverted the bottom
    // panel; excluding it (h > w*1.5 is false for the wide cap) leaves it in
    // its original orientation. The test is scale-invariant -- the slim caps
    // stay h/w ~3+ and the back cap stays h/w ~0.76 after any aspect scaling,
    // so the 1.5 threshold sits safely in the gap.
    if (!s_inNativeBlit && menu_capflip != nullptr && menu_capflip->current.enabled)
    {
        if (s_capMat == 0)
            s_capMat = ResolveMaterialStrict("button_highlight_end");
        if (s_capMat != 0 && material == s_capMat && h > w * 1.5f)
        {
            R_DrawStretchPic_Detour.GetOriginal<R_DrawStretchPic_fn_t>()(x, y, w, h, s0, t1, s1, t0, color, material);
            // v71: the header cap is a GSC hudelem at sort -2 -- it blits
            // BEFORE the sort-999 body text. Piggyback the class row panels
            // here so they paint behind the text/(A) instead of over them
            // (the OnCG_DrawActive draw is after the hud pass = on top).
            // DrawClassMenuPanels uses the raw R_DrawStretchPic_fn pointer,
            // so it does not re-enter this detour; the guard is belt-and-braces.
            if (classmenu_behind != nullptr && classmenu_behind->current.enabled)
            {
                s_inNativeBlit = true;
                DrawClassMenuPanels();
                s_inNativeBlit = false;
            }
            return;
        }
    }

    if (!s_inNativeBlit && compass_native != nullptr && compass_native->current.enabled)
    {
        const int mapHandle = RegisterCompassMapMaterial();
        if (mapHandle != 0 && material == mapHandle)
        {
            // v35: real UI menu open (pause/options/team/class). CL_KeyEvent
            // (822DD1E8) gates all menu key routing on KEYCATCH_UI = bit 0x10
            // of the catcher word at 0x82435a18 + client*0x24; the engine
            // sets it for every Menus_OpenByName-style menu and clears it on
            // close (plain spectate = 0 -- the v17 lesson). While set, the
            // whole caster compass leaks over the menu, so SWALLOW the scMap
            // blit itself: skip the original call and every composite layer.
            // Frame-perfect, no GSC involvement.
            const uint32_t catchers = *reinterpret_cast<volatile uint32_t *>(0x82435a18u);
            if ((catchers & 0x10u) != 0)
                return;

            R_DrawStretchPic_Detour.GetOriginal<R_DrawStretchPic_fn_t>()(x, y, w, h, s0, t0, s1, t1, color, material);

            s_inNativeBlit = true;
            DrawNativeSpecCompass(x, y, w, h);
            DrawCasterBars();  // v61: HP bars, same render phase as the compass
            s_inNativeBlit = false;
            return;
        }
    }

    R_DrawStretchPic_Detour.GetOriginal<R_DrawStretchPic_fn_t>()(x, y, w, h, s0, t0, s1, t1, color, material);
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

    menu_capflip = Dvar_RegisterBool("menu_capflip", false, 0,
        "GSC: class menu open -> V-flip button_highlight_end caps (native bottom-right chamfer)");

    // v67 class-menu faded row panels. classmenu_panels/idx/n are published by
    // GSC; the rest are layout knobs (GSC virtual 640x480 units) so you can
    // calibrate from a screenshot via a GSC redeploy, no DLL rebuild.
    classmenu_panels = Dvar_RegisterBool("classmenu_panels", false, 0, "GSC: class menu open -> draw faded row panels");
    classmenu_behind = Dvar_RegisterBool("classmenu_behind", true, 0, "Draw class panels behind text (piggyback header-cap blit) vs on-top");
    classmenu_cap  = Dvar_RegisterBool("classmenu_cap", true, 0, "Draw native chamfered left cap on class panels");
    classmenu_capw = Dvar_RegisterInt("classmenu_capw", 5, 0, 100, 0, "Chamfer cap width, per-mille of screen W");
    classmenu_caps = Dvar_RegisterBool("classmenu_caps", true, 0, "Chamfer cap: mirror horizontally (chamfer to the left)");
    classmenu_capt = Dvar_RegisterBool("classmenu_capt", false, 0, "Chamfer cap: mirror vertically (flip chamfer top/bottom)");
    classmenu_capright = Dvar_RegisterBool("classmenu_capright", true, 0, "Chamfer cap on the right (text) end vs the left end");
    classmenu_cr = Dvar_RegisterInt("classmenu_cr", 150, 0, 255, 0, "Panel colour red (0-255)");
    classmenu_cg = Dvar_RegisterInt("classmenu_cg", 165, 0, 255, 0, "Panel colour green (0-255)");
    classmenu_cb = Dvar_RegisterInt("classmenu_cb", 175, 0, 255, 0, "Panel colour blue (0-255)");
    classmenu_idx = Dvar_RegisterInt("classmenu_idx", -1, -1, 16, 0, "GSC: selected class row index");
    classmenu_n = Dvar_RegisterInt("classmenu_n", 7, 1, 16, 0, "GSC: class row count");
    classmenu_sep = Dvar_RegisterInt("classmenu_sep", 4, -1, 16, 0, "Separator after this row index (-1 none); 4 = after Secondary");
    classmenu_px = Dvar_RegisterInt("classmenu_px", 244, 0, 1000, 0, "Panel right edge (per-mille of screen width)");
    classmenu_pw = Dvar_RegisterInt("classmenu_pw", 175, 5, 1000, 0, "Panel width (per-mille of screen width)");
    classmenu_py = Dvar_RegisterInt("classmenu_py", 360, 0, 1000, 0, "First-row panel top (per-mille of screen height)");
    classmenu_pitch = Dvar_RegisterInt("classmenu_pitch", 34, 1, 200, 0, "Row pitch (per-mille of screen height)");
    classmenu_ph = Dvar_RegisterInt("classmenu_ph", 28, 1, 200, 0, "Panel height (per-mille of screen height)");
    classmenu_solid = Dvar_RegisterInt("classmenu_solid", 60, 0, 100, 0, "Solid body width as pct of panel (rest fades left)");
    classmenu_fade = Dvar_RegisterBool("classmenu_fade", false, 0, "Flip the left fade strip if it fades the wrong way");
    classmenu_a0 = Dvar_RegisterInt("classmenu_a0", 45, 0, 100, 0, "Panel alpha pct, unselected rows");
    classmenu_a1 = Dvar_RegisterInt("classmenu_a1", 80, 0, 100, 0, "Panel alpha pct, selected row");

    compass_group0 = Dvar_RegisterBool("compass_group0", false, 0, "Retired: legacy follow-spec HUD group force (no-op)");

    // v24 native spec compass: enabled by GSC (caster_radar) while casting;
    // drawn from the R_DrawStretchPic detour when the GSC scMap blit is seen.
    compass_native = Dvar_RegisterBool("compass_native", false, 0,
        "Draw native engine compass blips over the GSC caster map (set by GSC while casting)");
    compass_native_bg = Dvar_RegisterBool("compass_native_bg", true, 0,
        "Draw the native compass map texture under the blips, aspect-fitted like the arrows (v28 default on)");
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

    caster_obj_size = Dvar_RegisterInt("caster_obj_size", 24, 4, 96, 0,
        "Caster objective icon size (sites, flags, radios, bomb), projected units");

    caster_arrow_friendly = Dvar_RegisterInt("caster_arrow_friendly", 32, 4, 96, 0,
        "Caster map team arrow size (swap-scoped to the caster draw only)");
    caster_arrow_player = Dvar_RegisterInt("caster_arrow_player", 44, 4, 96, 0,
        "Caster map followed-player arrow size (swap-scoped to the caster draw only)");

    caster_font = Dvar_RegisterInt("caster_font", 0, 0, 6, 0,
        "Stat table font: 0 console 1 small 2 normal 3 bold 4 objective 5 big 6 extraBig");

    caster_arrows = Dvar_RegisterBool("caster_arrows", true, 0,
        "Draw caster blips as native-styled rotated arrows (off: plain square dots)");

    // v23 caster radar publisher: DLL publishes the static compass projection
    // basis once per map; GSC uses it for objective markers (and the dots, when
    // they existed).
    caster_publish = Dvar_RegisterBool("caster_publish", true, 0,
        "Publish compass projection basis (caster_wox/woy/wsx/wsy/nx/ny) for the GSC caster radar");

    caster_followed = Dvar_RegisterInt("caster_followed", -1, -1, 63, 0,
        "Followed client slot while spectating, -1 when none (published by the DLL)");

    // v45 stat-column dvars. The string itself is GSC-registered via setdvar;
    // only the positions live here. Defaults calibrated from the NDIAG line
    // in the ~1024x768 text space; tune from GSC with setdvar (no rebuild).
    Dvar_RegisterString("caster_stats", "", DVAR_FLAG_NONE,
        "10 pipe-separated stat row strings (rows 0-4 allies col, 5-9 axis col); empty = off");
    // v47: recalibrated from on-screen measurements (text space is
    // ~1.5 px per unit at 1080p, not the v45 estimate).
    caster_stats_xl = Dvar_RegisterInt("caster_stats_xl", 265, -200, 2000, 0,
        "Stat column x, allies side (text space)");
    caster_stats_xr = Dvar_RegisterInt("caster_stats_xr", 950, -200, 2000, 0,
        "Stat column x, axis side (text space)");
    caster_stats_y = Dvar_RegisterInt("caster_stats_y", 430, 0, 2000, 0,
        "Stat rows base y (text space)");
    caster_stats_dy = Dvar_RegisterInt("caster_stats_dy", 27, 1, 200, 0,
        "Stat row pitch (text space)");

    caster_stats_dx = Dvar_RegisterInt("caster_stats_dx", 45, 5, 400, 0,
        "Gap between the K and D stat sub-columns (text space)");
    caster_stats_hdr = Dvar_RegisterBool("caster_stats_hdr", true, 0,
        "Draw the Kills / Deaths header row above the stat columns");
    Dvar_RegisterString("caster_hp", "", DVAR_FLAG_NONE,
        "v60: 10 pipe fields, int hp 1-100 per visible bar, empty = hidden");
    Dvar_RegisterString("caster_bar_rgb1", "0.25 0.5 0.95", DVAR_FLAG_NONE,
        "v60: allies bar color, r g b floats");
    Dvar_RegisterString("caster_bar_rgb2", "0.9 0.2 0.2", DVAR_FLAG_NONE,
        "v60: axis bar color, r g b floats");
    caster_bar_xl = Dvar_RegisterInt("caster_bar_xl", 40, -200, 2000, 0, "v60 allies bar left edge px");
    caster_bar_xr = Dvar_RegisterInt("caster_bar_xr", 1240, -200, 2000, 0, "v60 axis bar right edge px");
    caster_bar_y = Dvar_RegisterInt("caster_bar_y", 399, 0, 2000, 0, "v60 bar row0 top px");
    caster_bar_dy = Dvar_RegisterInt("caster_bar_dy", 27, 1, 200, 0, "v60 bar row step px");
    caster_bar_w = Dvar_RegisterInt("caster_bar_w", 300, 10, 1000, 0, "v60 full bar width px");
    caster_bar_h = Dvar_RegisterInt("caster_bar_h", 21, 2, 100, 0, "v60 bar height px");
    caster_bar_cap = Dvar_RegisterInt("caster_bar_cap", 14, 0, 100, 0, "v60 cap width px");
    caster_hdr_scale = Dvar_RegisterInt("caster_hdr_scale", 100, 25, 150, 0,
        "Header text scale, percent of the stat row size");

    caster_imginfo = Dvar_RegisterString("caster_imginfo", "", DVAR_FLAG_NONE,
        "Image name to probe on screen (dims/format readout); empty = off");

    // Build marker -- proves this cg.cpp compiled into the running codxe DLL.
    // GSC gates compass_native enablement on this being >= 24.
    Dvar_RegisterInt("compass_hook_v", 80, 0, 100, 0,
        "Codxe compass hook build marker (v66 -- cap flip excludes the wide back-bar cap)");

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

            if (cg_draw_player_info->current.enabled)
            {
                CG_DrawPlayerInfo();
            }

            // v49: publish the followed client slot for GSC.
            if (caster_followed != nullptr)
            {
                int slot = -1;
                const int cgB = static_cast<int>(*reinterpret_cast<volatile uint32_t *>(0x823F28A0u));
                if (cgB != 0)
                {
                    const int snapO = *reinterpret_cast<volatile int *>(cgB + 0x24);
                    if (snapO != 0)
                    {
                        const int ciO = *reinterpret_cast<volatile int *>(snapO + 0xec);
                        if (ciO >= 0 && ciO <= 63)
                            slot = ciO;
                    }
                }
                caster_followed->current.integer = slot;
            }

            // v58: on-screen image-spec readout for the rounded-bar
            // donor hunt. Xenia mounts game:\ read-only (imagedump
            // writes all fail), but the replacement only needs the
            // donor's dims + DXT format -- and the engine knows those.
            // Set caster_imginfo to an image name; specs draw at 10,170
            // until cleared. Same DB lookup image_loader keys on, so a
            // FOUND here means Image_Replace would match that name.
            {
                const char *probeName = Dvar_GetString("caster_imginfo");
                if (probeName != nullptr && probeName[0] != '\0')
                {
                    char info[160];
                    XAssetEntryPoolEntry *ae = DB_FindXAssetEntry(ASSET_TYPE_IMAGE, probeName);
                    if (ae == nullptr)
                    {
                        sprintf_s(info, sizeof(info), "IMG %s: NOT FOUND", probeName);
                    }
                    else
                    {
                        GfxImage *img = ae->entry.asset.header.image;
                        const char *fmt = "null-basemap";
                        int rawFmt = -1;
                        if (img->texture.basemap != nullptr)
                        {
                            rawFmt = static_cast<int>(img->texture.basemap->Format.DataFormat);
                            if (rawFmt == static_cast<int>(GPUTEXTUREFORMAT_DXT1))
                                fmt = "DXT1";
                            else if (rawFmt == static_cast<int>(GPUTEXTUREFORMAT_DXT2_3))
                                fmt = "DXT3";
                            else if (rawFmt == static_cast<int>(GPUTEXTUREFORMAT_DXT4_5))
                                fmt = "DXT5";
                            else if (rawFmt == static_cast<int>(GPUTEXTUREFORMAT_DXN))
                                fmt = "DXN";
                            else
                                fmt = "other";
                        }
                        // v59: mat flag = a MATERIAL with this exact name
                        // strict-resolves (what GSC precacheShader/setShader
                        // needs; the image existing is not enough).
                        const int matOk = (ResolveMaterialStrict(probeName) != 0) ? 1 : 0;
                        sprintf_s(info, sizeof(info), "IMG %s: w=%d h=%d %s(%d) map=%d strm=%d mat=%d",
                                  probeName, static_cast<int>(img->width), static_cast<int>(img->height),
                                  fmt, rawFmt, static_cast<int>(img->mapType),
                                  img->streaming ? 1 : 0, matOk);
                    }
                    static Font_s *probeFont = R_RegisterFont("fonts/consoleFont");
                    static const float probeCol[4] = {0.3f, 1.0f, 1.0f, 1.0f};
                    R_AddCmdDrawText(info, 160, probeFont, 10.f, 170.f, 1.0f, 1.0f, 0.0f, probeCol, 0);
                }
            }

            // v46: material-cache refresh tick, see comment at
            // s_mapMatHandle. 240 frames ~= 4s at 60fps.
            {
                ++s_frameCounter; // v47
                static unsigned int s_matRefreshFrames = 0;
                if (++s_matRefreshFrames >= 240)
                {
                    s_matRefreshFrames = 0;
                    s_mapMatHandle = 0;
                    s_mapMatLastName[0] = '\0';
                    s_whiteMat = 0;
                    s_arrowMat = 0;
                    s_pingMat = 0;
                    s_playerArrowMat = -2;
                    s_statFont = nullptr;   // v51
                    s_statFontIdx = -1;
                    s_barBodyMat = 0;   // v61: bar materials, same staleness
                    s_capMat = 0;       // v65: cap material, same staleness
                    s_gradMat = 0;      // v67: class-menu gradient material
                }
            }

            // v45: caster stat columns (gated inside on caster_stats
            // non-empty + no engine menu open).
            DrawCasterStats();
            // v71: panels now draw BEHIND the text from the header-cap
            // blit (R_DrawStretchPic_Hook). This OnCG path is the on-top
            // fallback, used only when classmenu_behind is off.
            if (classmenu_behind == nullptr || !classmenu_behind->current.enabled)
                DrawClassMenuPanels();
            // v61: DrawCasterBars() MOVED to the R_DrawStretchPic detour.
            // Calling R_DrawStretchPic_fn from this frame tick is the
            // wrong render phase (broke the caster compass on-box, v60).
            // R_AddCmdDrawText (stats above) is command-add and stays.
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
