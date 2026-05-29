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
    DbgPrint("T4 MP: Plugin loaded (r313 minimal-bw-only)\n");

    // ========================================================================
    // r313 — STRIPPED MODULE SET for freeze diagnosis + BW-only operation
    //
    // Confirmed (2026-05-29): stock codxe T4 MP freezes ~3-6 min into any
    // match on Xenia, even with 0 bots, just walking around. Vanilla WaW on
    // the same Xenia finishes matches normally. Process Explorer stack at
    // freeze shows 3 PPC JIT threads parked in:
    //     ntdll!NtWaitForSingleObject
    //     xenia!curl_formfree+0xXXX  (x3 threads)
    //
    // Guest code is blocked in a libcurl/kernel wait that never returns.
    // codxe doesn't link libcurl directly — something it detours/hooks must
    // be calling a guest function that fans out to Xenia's network or XAM
    // emulation, which uses libcurl internally.
    //
    // STRATEGY: drop every non-essential module in one pass.
    //   - If freeze disappears: the answer was in the stripped set, ship it.
    //   - If freeze persists: narrowed to {Config, Branding, GSCClientFields,
    //     GSCLoader, sv_bots, ui}, or codxe Plugin/RegisterModule machinery.
    //
    // KEPT MODULES (6):
    //   Config          — reads codxe.json, selects active mod (required)
    //   Branding        — "CoDxe rXXX" build banner (cosmetic, kept by user req)
    //   GSCClientFields — self.god / self.noclip / self.ufo (kept by user req)
    //   GSCLoader       — loads .gsc from mod folder (required for BW scripts)
    //   sv_bots         — BW bot driver (the whole point of this port)
    //   ui              — splitscreen + StartServer fixes (needed for testing)
    //
    // STRIPPED MODULES (5 plus 2 already-off):
    //   BrushCollision  — noclip_brushes dvar; dev tool; CG_DrawActive per-frame
    //   GSCClientMethods — SetVelocity, SetStance, ButtonPressed; BW doesn't use
    //   GSCFunctions    — only registers getplayerclipbrushescontainingpoint
    //   Map             — .ents override loader; BW doesn't use map ents
    //   TestModule      — empty
    //   cg              — already off since r301 (ADS bug)
    //   ImageLoader     — already off upstream
    //
    // Inline weapon NOP patches also stripped (already off since r300).
    // ========================================================================

    RegisterModule(new Config());
    RegisterModule(new Branding());
    // RegisterModule(new BrushCollision());     // [r313] stripped — dev tool, per-frame hook
    // RegisterModule(new cg());                  // [r301] stripped — ADS angle corruption
    RegisterModule(new GSCClientFields());
    // RegisterModule(new GSCClientMethods());   // [r313] stripped — BW doesn't call these
    // RegisterModule(new GSCFunctions());       // [r313] stripped — only dev function
    RegisterModule(new GSCLoader());
    // RegisterModule(new ImageLoader());         // upstream default
    // RegisterModule(new Map());                // [r313] stripped — BW doesn't use map ents
    RegisterModule(new sv_bots());
    // RegisterModule(new TestModule());         // [r313] stripped — empty module
    RegisterModule(new ui());

    // Inline patches — already disabled since r300, kept off.
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
