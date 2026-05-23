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
    DbgPrint("T4 MP: Plugin loaded (r300 patches-disabled-for-ads-diag)\n");
    RegisterModule(new Config());
    RegisterModule(new Branding());
    RegisterModule(new BrushCollision());
    RegisterModule(new cg());
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
    // Patches  -- [r300] DISABLED for ADS-no-damage diagnostic. Re-enable
    // one at a time to bisect if ADS works with these off.
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
