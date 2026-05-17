#include "pch.h"
#include "gsc_functions.h"

namespace t4
{
namespace mp
{

// Forward decl from sv_bots.cpp — registers BW-specific GSC builtins.
extern "C" BuiltinFunction BW_LookupFunction(const char *name);

/**
 * Checks if a 3D point is contained within an axis-aligned bounding box
 */
bool IsPointInBounds(const float mins[3], const float maxs[3], const float point[3])
{
    return (point[0] >= mins[0] && point[0] <= maxs[0]) && (point[1] >= mins[1] && point[1] <= maxs[1]) &&
           (point[2] >= mins[2] && point[2] <= maxs[2]);
}

void GSCrGetPlayerclipBrushesContainingPoint()
{
    float point[3] = {0};
    Scr_GetVector(0, point, SCRIPTINSTANCE_SERVER, -1);

    std::vector<int> brushIndices;
    for (int i = 0; i < cm->numBrushes; ++i)
    {
        auto &brush = cm->brushes[i];
        if (brush.contents & 0x10000 /* CONTENTS_PLAYERCLIP */ && IsPointInBounds(brush.mins, brush.maxs, point))
            brushIndices.push_back(i);
    }

    Scr_MakeArray(SCRIPTINSTANCE_SERVER);
    for (size_t i = 0; i < brushIndices.size(); ++i)
    {
        Scr_AddInt(brushIndices[i], SCRIPTINSTANCE_SERVER);
        Scr_AddArray(SCRIPTINSTANCE_SERVER);
    }
}

static struct
{
    const char *name;
    BuiltinFunction handler;
} gsc_functions[] = {
    {"getplayerclipbrushescontainingpoint", GSCrGetPlayerclipBrushesContainingPoint}, {nullptr, nullptr} // Terminator
};

Detour Scr_GetFunction_Detour;

BuiltinFunction Scr_GetFunction_Hook(const char **pName, int *type)
{
    if (pName != nullptr)
    {
        const auto *func = gsc_functions;
        for (; func->name != nullptr; ++func)
        {
            if (_stricmp(*pName, func->name) == 0)
                return func->handler;
        }

        // BW dispatch — checked after gsc_functions own table, before
        // falling through to the engine. Returns nullptr if not a BW name.
        if (BuiltinFunction bw = BW_LookupFunction(*pName))
        {
            DbgPrint("sv_bots: [HOOK] '%s' -> BW port handler\n", *pName);
            return bw;
        }

        // r321: log only the addtestclient lookup so the fall-through to the
        // native engine builtin is provable, without spamming every lookup.
        if (_stricmp(*pName, "addtestclient") == 0)
            DbgPrint("sv_bots: [HOOK] 'addtestclient' -> fall through to engine\n");
    }
    return Scr_GetFunction_Detour.GetOriginal<decltype(&Scr_GetFunction_Hook)>()(pName, type);
}

GSCFunctions::GSCFunctions()
{
    Scr_GetFunction_Detour = Detour(Scr_GetFunction, Scr_GetFunction_Hook);
    Scr_GetFunction_Detour.Install();
}

GSCFunctions::~GSCFunctions()
{
    Scr_GetFunction_Detour.Remove();
}
} // namespace mp
} // namespace t4
