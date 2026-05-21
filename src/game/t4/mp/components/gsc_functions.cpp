#include "pch.h"
#include "gsc_functions.h"

#include <cstdio>
#include <cstring>

namespace t4
{
namespace mp
{

using namespace t4::mp::bw;

// Forward decl from sv_bots.cpp — registers BW-specific GSC builtins.
extern "C" BuiltinFunction BW_LookupFunction(const char *name);

// ---------------------------------------------------------------------------
// fs_* file I/O (ported from IW3 codxe gsc_functions.cpp)
//
// Provides waypoint CSV loading for Bot Warfare. The deployed
// bots_adapter_pt4.gsc previously stubbed these out by default
// (returns false/-1/empty) because stock codxe T4 didn't expose them.
// With these handlers wired into gsc_functions[] below, the adapter
// dispatches to the real engine fs_* calls and BW hot-loads CSVs from
// <mod>/scriptdata/waypoints/.
//
// Implementation notes:
//   - Uses stdlib fopen/fclose/fgets/fprintf. Same approach as IW3 codxe.
//     Xenia's host filesystem accepts paths beginning with "game:\".
//   - Path resolution: Config::GetModBasePath() returns "game:\_codxe\mods\<active>".
//     BuildScriptFilePath joins that with the relative path BW provides.
//     Forward slashes are normalized to backslashes for Xbox 360.
//   - Handle slots are 1-indexed (BW expects 0/-1 to mean "no handle").
//   - Scr_* calls use the _BW variants from symbols_bw_ext.h — stock
//     symbols.h has wrong addresses for several of these on TU7.
// ---------------------------------------------------------------------------

#define MAX_SCRIPT_FILEHANDLES 8

struct ScriptFileHandle_t
{
    FILE *fh;
    char  filename[256];
};

static ScriptFileHandle_t s_scriptFiles[MAX_SCRIPT_FILEHANDLES];

static std::string BuildScriptFilePath(const char *filename)
{
    if (!filename)
        return std::string();

    // If already absolute or has the Xenia "game:\" prefix, pass through.
    if ((filename[0] && filename[1] == ':') || std::strncmp(filename, "game:\\", 6) == 0)
        return filename;

    std::string base = Config::GetModBasePath();
    if (base.empty())
        return filename;

    // Normalize forward slashes to backslashes (Xbox 360 path separator).
    std::string rel(filename);
    for (size_t i = 0; i < rel.size(); ++i)
        if (rel[i] == '/')
            rel[i] = '\\';

    return base + "\\" + rel;
}

static void CloseAllScriptFiles()
{
    for (int i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (s_scriptFiles[i].fh)
        {
            std::fclose(s_scriptFiles[i].fh);
            std::memset(&s_scriptFiles[i], 0, sizeof(ScriptFileHandle_t));
        }
    }
}

static void GScr_FS_TestFile()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_testfile(<filename>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    std::string fullpath = BuildScriptFilePath(filename);

    FILE *f = std::fopen(fullpath.c_str(), "r");
    if (f)
    {
        std::fclose(f);
        Scr_AddInt_BW(1, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
    }
}

static void GScr_FS_FOpen()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error_BW("Usage: fs_fopen(<filename>, <mode>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString_BW(0, SCRIPTINSTANCE_SERVER);
    const char *mode_str = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
    const char *fmode;

    if (_stricmp(mode_str, "read") == 0)
        fmode = "rt";
    else if (_stricmp(mode_str, "write") == 0)
        fmode = "wt";
    else if (_stricmp(mode_str, "append") == 0)
        fmode = "at";
    else
    {
        Scr_Error_BW("fs_fopen: invalid mode. Valid modes are: read, write, append",
                     SCRIPTINSTANCE_SERVER);
        return;
    }

    std::string fullpath = BuildScriptFilePath(filename);

    // Create parent directories for write/append modes.
    if (fmode[0] == 'w' || fmode[0] == 'a')
    {
        char dirpath[256];
        std::strncpy(dirpath, fullpath.c_str(), sizeof(dirpath) - 1);
        dirpath[sizeof(dirpath) - 1] = '\0';
        char *last_slash = std::strrchr(dirpath, '\\');
        if (last_slash)
        {
            *last_slash = '\0';
            filesystem::create_nested_dirs(dirpath);
        }
    }

    for (int i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (!s_scriptFiles[i].fh)
        {
            s_scriptFiles[i].fh = std::fopen(fullpath.c_str(), fmode);
            if (!s_scriptFiles[i].fh)
            {
                // Failed open: return 0 so GSC can detect it.
                Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
                return;
            }
            std::strncpy(s_scriptFiles[i].filename, filename,
                         sizeof(s_scriptFiles[i].filename) - 1);
            Scr_AddInt_BW(i + 1, SCRIPTINSTANCE_SERVER);
            return;
        }
    }

    Scr_Error_BW("fs_fopen: exceeded maximum open file handles", SCRIPTINSTANCE_SERVER);
}

static void GScr_FS_FClose()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_fclose(<filehandle>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_fclose: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (slot.fh)
    {
        std::fclose(slot.fh);
        std::memset(&slot, 0, sizeof(ScriptFileHandle_t));
    }
}

static void GScr_FS_ReadLine()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error_BW("Usage: fs_readline(<filehandle>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_readline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error_BW("fs_readline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    char buffer[8192];
    if (!std::fgets(buffer, sizeof(buffer), slot.fh))
    {
        // EOF or error: GSC sees `undefined` so it can break loops.
        Scr_AddUndefined_BW(SCRIPTINSTANCE_SERVER);
        return;
    }

    // Strip trailing \r\n / \n the way IW3 does.
    int len = static_cast<int>(std::strlen(buffer));
    if (len > 0 && buffer[len - 1] == '\n')
    {
        buffer[len - 1] = '\0';
        --len;
    }
    if (len > 0 && buffer[len - 1] == '\r')
    {
        buffer[len - 1] = '\0';
    }

    Scr_AddString(buffer, SCRIPTINSTANCE_SERVER);
}

static void GScr_FS_WriteLine()
{
    if (Scr_GetNumParam_BW(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error_BW("Usage: fs_writeline(<filehandle>, <data>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt_BW(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error_BW("fs_writeline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error_BW("fs_writeline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    const char *data = Scr_GetString_BW(1, SCRIPTINSTANCE_SERVER);
    if (std::fprintf(slot.fh, "%s\n", data) < 0)
    {
        Scr_AddInt_BW(0, SCRIPTINSTANCE_SERVER);
        return;
    }

    Scr_AddInt_BW(1, SCRIPTINSTANCE_SERVER);
}

// ---------------------------------------------------------------------------
// Existing T4 builtin: getplayerclipbrushescontainingpoint
// (Untouched; used by codjumper and other GSC mods.)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Function dispatch table
// ---------------------------------------------------------------------------

static struct
{
    const char     *name;
    BuiltinFunction handler;
} gsc_functions[] = {
    {"getplayerclipbrushescontainingpoint", GSCrGetPlayerclipBrushesContainingPoint},
    {"fs_testfile",                         GScr_FS_TestFile},
    {"fs_fopen",                            GScr_FS_FOpen},
    {"fs_fclose",                           GScr_FS_FClose},
    {"fs_readline",                         GScr_FS_ReadLine},
    {"fs_writeline",                        GScr_FS_WriteLine},
    {nullptr, nullptr} // Terminator
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
    // Reset filehandle slots on module bring-up. Without this they'd carry
    // over any FILE* from a previous run if the binary state ever survived
    // a level transition (it doesn't, but be explicit).
    std::memset(s_scriptFiles, 0, sizeof(s_scriptFiles));

    Scr_GetFunction_Detour = Detour(Scr_GetFunction, Scr_GetFunction_Hook);
    Scr_GetFunction_Detour.Install();
}

GSCFunctions::~GSCFunctions()
{
    // Close any still-open files so handles aren't leaked on shutdown.
    CloseAllScriptFiles();
    Scr_GetFunction_Detour.Remove();
}
} // namespace mp
} // namespace t4
