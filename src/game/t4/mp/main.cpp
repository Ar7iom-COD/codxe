#include "pch.h"
#include "main.h"
#include "components/branding.h"
#include "components/brush_collision.h"
#include "components/cg.h"
#include "components/gsc_client_fields.h"
#include "components/gsc_client_methods.h"
#include "components/gsc_functions.h"
#include "components/gsc_loader.h"
#include "components/image_loader.h"
#include "components/map.h"
#include "components/sv_bots.h"
#include "components/test_module.h"
#include "components/ui.h"
namespace t4
{
namespace mp
{
T4_MP_Plugin::T4_MP_Plugin()
{
    DbgPrint("T4 MP: Plugin loaded (r321 ratelimit-3to1 post-diag)\n");

    // ========================================================================
    // r316 — BrushCollision removed, everything else as r309/r315 baseline.
    //
    // Targeted freeze test. BrushCollision is the only module that hooks a
    // per-frame in-match path (CG_DrawActive), and its hook body calls
    // R_CheckDvarModified every single frame just to support the dev-only
    // noclip_brushes dvar. Strongest candidate for the libcurl wait
    // accumulator behind the 3-6 minute freeze.
    //
    // ACTIVE (10 modules):
    //   Config, Branding, GSCClientFields, GSCClientMethods, GSCFunctions,
    //   GSCLoader, Map, sv_bots, TestModule, ui
    //
    // DISABLED:
    //   BrushCollision   [r316] removed — freeze suspect (per-frame hook,
    //                    dev-only noclip_brushes dvar)
    //   cg               [r301] ADS angle corruption bug
    //   ImageLoader      upstream default
    //   3x weapon NOPs   [r300] kept off
    //
    // EXPECTED OUTCOMES on freeze test (Castle, 0 bots, 8 min):
    //   A) No freeze     -> BrushCollision was the cause. Done. Document
    //                       and ship. noclip_brushes dvar dead but unused.
    //   B) Same freeze   -> BrushCollision was not the cause. Restore it,
    //                       try Branding next (UI_DrawBuildNumber per-frame).
    //   C) Different     -> partial cause; capture new procexp stack.
    // ========================================================================

    RegisterModule(new Config());
    RegisterModule(new Branding());
    // RegisterModule(new BrushCollision());    // [r316] removed — freeze suspect
    // RegisterModule(new cg());                 // [r301] disabled — ADS bug
    RegisterModule(new GSCClientFields());
    RegisterModule(new GSCClientMethods());
    RegisterModule(new GSCFunctions());
    RegisterModule(new GSCLoader());
    // RegisterModule(new ImageLoader());        // upstream default
    RegisterModule(new Map());
    RegisterModule(new sv_bots());
    RegisterModule(new TestModule());
    RegisterModule(new ui());

    // Inline patches — disabled since r300. Leave off.
    // *(volatile uint32_t *)0x8220D2E8 = 0x60000000; // NO_KNOCKBACK NOP
    // *(volatile uint32_t *)0x8225F98C = 0x60000000; // Weapon_RocketLauncher_Fire NOP
    // *(volatile uint32_t *)0x8225F990 = 0x60000000; // Weapon_RocketLauncher_Fire NOP
}
T4_MP_Plugin::~T4_MP_Plugin()
{
    DbgPrint("T4 MP: Plugin unloaded\n");
}
} // namespace mp
} // namespace t4
