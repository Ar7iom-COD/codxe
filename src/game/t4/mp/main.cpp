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
    DbgPrint("T4 MP: Plugin loaded (r314 minimal-bw + GSCClientMethods)\n");

    // ========================================================================
    // r314 — GSCClientMethods restored.
    //
    // r313 build (6 modules) compiled clean but BW scripts failed at load with
    // "Server script compile error / unknown function". One of the stripped
    // modules was load-bearing for BW's GSC surface.
    //
    // GSCClientMethods is the most likely culprit: it detours T4's player
    // method dispatcher to register methods like ButtonPressed, SetVelocity,
    // SetStance, etc. BW's scripts likely call one of these (probably
    // ButtonPressed for menu input).
    //
    // Re-adding it. If BW now loads -> confirmed minimum viable set is 7
    // modules. If error persists -> it's a different missing function and
    // we capture the actual name from Xenia console.
    // ========================================================================

    RegisterModule(new Config());
    RegisterModule(new Branding());
    // RegisterModule(new BrushCollision());     // [r313] stripped — dev tool, per-frame hook
    // RegisterModule(new cg());                  // [r301] stripped — ADS angle corruption
    RegisterModule(new GSCClientFields());
    RegisterModule(new GSCClientMethods());      // [r314] RESTORED — was load-bearing for BW
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
