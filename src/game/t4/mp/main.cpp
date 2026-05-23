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
    DbgPrint("T4 MP: Plugin loaded (r301 cg-module-disabled-for-ads-diag)\n");
    RegisterModule(new Config());
    RegisterModule(new Branding());
    RegisterModule(new BrushCollision());

    // [r301] cg module DISABLED for ADS-no-damage diagnostic.
    // The cg module installs two detours on BG_CalculateWeaponPosition_IdleAngles
    // and BG_CalculateView_IdleAngles. Both are "BG_" (BothGame) functions which
    // run in both cgame (client) and game (server) contexts and affect view
    // angle computation -- which is what bullet traces use as origin direction.
    //
    // Even though the hooks are designed to be pass-through when bg_bobIdle=true
    // (default), codxe's Detour class relocates the function prologue. If the
    // original prologue contains non-relocatable instructions (PC-relative or
    // similar), the GetOriginal() trampoline executes a corrupted version of
    // the function silently. Symptom would be: bullet trace fires in wrong
    // direction, ADS misses (tight aim, no spread to compensate), hipfire ARs
    // hit (large spread compensates), snipers miss even hipfire (no spread).
    //
    // This matches the bug profile exactly.
    //
    // If ADS works with this disabled: confirmed it's one of the BG_Calculate
    // detours. We can re-enable cg with one detour at a time to bisect.
    //
    // RegisterModule(new cg());

    // ORDER MATTERS: GSCClientMethods and GSCFunctions install detours on
    // Player_GetMethod / Scr_GetFunction. sv_bots exposes its lookups via
    // BW_LookupMethod / BW_LookupFunction which the existing dispatchers
    // call before falling through. sv_bots must register AFTER them.
    RegisterModule(new GSCClientFields());
    RegisterModule(new GSCClientMethods());
    RegisterModule(new GSCFunctions());
    RegisterModule(new GSCLoader());
    // RegisterModule(new ImageLoader());
    RegisterModule(new Map());
    RegisterModule(new sv_bots());
    RegisterModule(new TestModule());
    RegisterModule(new ui());
    // Patches  -- [r300] DISABLED for ADS-no-damage diagnostic.
    // [r301] confirmed not the cause - bug persisted with these off.
    // Keep disabled for now until BG_Calculate theory tested.
    // sub_8220D2D0
    // Patches NO_KNOCKBACK flag check, allows knockback regardless of flags
    // *(volatile uint32_t *)0x8220D2E8 = 0x60000000; // NOP replaces bnelr
    // Weapon_RocketLauncher_Fire
    // *(volatile uint32_t *)0x8225F98C = 0x60000000;
    // *(volatile uint32_t *)0x8225F990 = 0x60000000;
}
T4_MP_Plugin::~T4_MP_Plugin()
{
    DbgPrint("T4 MP: Plugin unloaded\n");
}
} // namespace mp
} // namespace t4
