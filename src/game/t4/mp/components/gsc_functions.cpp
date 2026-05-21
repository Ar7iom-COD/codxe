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
// fs_* file I/O for Bot Warfare waypoint loading.
//
// Why this matters:
//   BW reads waypoint CSVs from <mod>/scriptdata/waypoints/mp_<map>_wp.csv
//   at level init. The deployed bots_adapter_pt4.gsc historically stubbed
//   the fs_* dispatch (false/-1/empty) because codxe T4 didn't expose them.
//   With these handlers registered in gsc_functions[] below, the adapter
//   wires through to real engine calls.
//
// Why bulk fs_readall_lines exists (CRITICAL):
//   Xenia tracks "stackpoints" on every guest function call (PPC function
//   prologue). With enable_host_guest_stack_synchronization = true the pool
//   is 65536 entries. The IW3 BW CSV-read pattern is a tight GSC for-loop
//   calling fs_readline 200+ times back-to-back, which on T4-via-Xenia
//   accumulates stackpoints faster than they recycle and crashes the
//   emulator with "Overflowed stackpoints!" mid-loop. This was confirmed
//   in two production logs on Cliffside and Asylum.
//
//   fs_readall_lines collapses N readline calls into 1 host call by
//   slurping the entire file and pushing all lines as a GSC array in one
//   pass. BW's readWpsFromFile is patched to call it. The crash goes away
//   because the engine only crosses the guest/host boundary once per CSV.
//
//   Each Scr_AddString inside the array build still costs stackpoints, but
//   the cost is bounded by file size (up to ~1000 lines) and happens during
//   a single GSC builtin invocation that returns before the next GSC tick.
//   No GSC for-loop = no per-iteration accumulation.
//
// Implementation notes:
//   - Plain stdlib fopen/fclose/fgets/fprintf. Xenia accepts "game:\" paths.
//   - Config::GetModBasePath() resolves to "game:\_codxe\mods\<active>".
//   - Handle slots 1-indexed (BW expects 0/-1 = "no handle").
//   - Per symbols_bw_ext.h audit (r335), stock T4 symbols.h has the correct
//     TU7 addresses for Scr_AddInt/GetInt/GetString/Error/GetNumParam. Only
//     Scr_AddString and Scr_AddUndefined need the _BW-namespace versions
//     (provided by `using namespace t4::mp::bw;` above).
// ---------------------------------------------------------------------------

#define MAX_SCRIPT_FILEHANDLES 8
#define MAX_LINE_LENGTH        8192

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

    // Normalize forward slashes to backslashes for the Xbox 360 host FS.
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

static inline void StripTrailingNewline(char *buf, int len)
{
    if (len > 0 && buf[len - 1] == '\n') { buf[len - 1] = '\0'; --len; }
    if (len > 0 && buf[len - 1] == '\r') { buf[len - 1] = '\0'; }
}

static void GScr_FS_TestFile()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error("Usage: fs_testfile(<filename>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString(0, SCRIPTINSTANCE_SERVER);
    std::string fullpath = BuildScriptFilePath(filename);

    FILE *f = std::fopen(fullpath.c_str(), "r");
    if (f)
    {
        std::fclose(f);
        Scr_AddInt(1, SCRIPTINSTANCE_SERVER);
    }
    else
    {
        Scr_AddInt(0, SCRIPTINSTANCE_SERVER);
    }
}

static void GScr_FS_FOpen()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error("Usage: fs_fopen(<filename>, <mode>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString(0, SCRIPTINSTANCE_SERVER);
    const char *mode_str = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    const char *fmode;

    if (_stricmp(mode_str, "read") == 0)
        fmode = "rt";
    else if (_stricmp(mode_str, "write") == 0)
        fmode = "wt";
    else if (_stricmp(mode_str, "append") == 0)
        fmode = "at";
    else
    {
        Scr_Error("fs_fopen: invalid mode. Valid modes are: read, write, append",
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
                Scr_AddInt(0, SCRIPTINSTANCE_SERVER);
                return;
            }
            std::strncpy(s_scriptFiles[i].filename, filename,
                         sizeof(s_scriptFiles[i].filename) - 1);
            Scr_AddInt(i + 1, SCRIPTINSTANCE_SERVER);
            return;
        }
    }

    Scr_Error("fs_fopen: exceeded maximum open file handles", SCRIPTINSTANCE_SERVER);
}

static void GScr_FS_FClose()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error("Usage: fs_fclose(<filehandle>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error("fs_fclose: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (slot.fh)
    {
        std::fclose(slot.fh);
        std::memset(&slot, 0, sizeof(ScriptFileHandle_t));
    }
}

// Legacy single-line read. Still wired for any GSC that uses it, but BW's
// readWpsFromFile patch should call fs_readall_lines instead to avoid the
// Xenia stackpoints overflow described at the top of this section.
static void GScr_FS_ReadLine()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error("Usage: fs_readline(<filehandle>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error("fs_readline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error("fs_readline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    char buffer[MAX_LINE_LENGTH];
    if (!std::fgets(buffer, sizeof(buffer), slot.fh))
    {
        Scr_AddUndefined_BW(SCRIPTINSTANCE_SERVER);
        return;
    }

    StripTrailingNewline(buffer, static_cast<int>(std::strlen(buffer)));
    Scr_AddString(buffer, SCRIPTINSTANCE_SERVER);
}

// Bulk read: open the file by path, slurp every line, return as a GSC array
// of strings. Returns `undefined` if the file cannot be opened. Closes the
// file before returning — caller does NOT need to call fs_fclose afterward.
//
// Single GSC builtin invocation. One host-call boundary cross. Per-line cost
// stays inside the call. This is the fix for Xenia's stackpoints overflow.
//
// Signature: fs_readall_lines(<filename>) -> array of string  | undefined
static void GScr_FS_ReadAllLines()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 1)
        Scr_Error("Usage: fs_readall_lines(<filename>)", SCRIPTINSTANCE_SERVER);

    const char *filename = Scr_GetString(0, SCRIPTINSTANCE_SERVER);
    std::string fullpath = BuildScriptFilePath(filename);

    FILE *f = std::fopen(fullpath.c_str(), "rt");
    if (!f)
    {
        Scr_AddUndefined_BW(SCRIPTINSTANCE_SERVER);
        return;
    }

    // Open a fresh array on the GSC stack. Each Scr_AddString followed by
    // Scr_AddArray pushes one element. Pattern mirrors the existing
    // getplayerclipbrushescontainingpoint helper in this file.
    Scr_MakeArray(SCRIPTINSTANCE_SERVER);

    char buffer[MAX_LINE_LENGTH];
    int  lineCount = 0;
    while (std::fgets(buffer, sizeof(buffer), f))
    {
        StripTrailingNewline(buffer, static_cast<int>(std::strlen(buffer)));
        Scr_AddString(buffer, SCRIPTINSTANCE_SERVER);
        Scr_AddArray(SCRIPTINSTANCE_SERVER);
        ++lineCount;
    }

    std::fclose(f);

    DbgPrint("sv_bots: fs_readall_lines loaded %d lines from %s\n", lineCount, filename);
}

static void GScr_FS_WriteLine()
{
    if (Scr_GetNumParam(SCRIPTINSTANCE_SERVER) != 2)
        Scr_Error("Usage: fs_writeline(<filehandle>, <data>)", SCRIPTINSTANCE_SERVER);

    int fh = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
    if (fh < 1 || fh > MAX_SCRIPT_FILEHANDLES)
        Scr_Error("fs_writeline: invalid filehandle", SCRIPTINSTANCE_SERVER);

    ScriptFileHandle_t &slot = s_scriptFiles[fh - 1];
    if (!slot.fh)
        Scr_Error("fs_writeline: filehandle is not open", SCRIPTINSTANCE_SERVER);

    const char *data = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    if (std::fprintf(slot.fh, "%s\n", data) < 0)
    {
        Scr_AddInt(0, SCRIPTINSTANCE_SERVER);
        return;
    }

    Scr_AddInt(1, SCRIPTINSTANCE_SERVER);
}

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
    {"getplayerclipbrushescontainingpoint", GSCrGetPlayerclipBrushesContainingPoint},
    {"fs_testfile",                         GScr_FS_TestFile},
    {"fs_fopen",                            GScr_FS_FOpen},
    {"fs_fclose",                           GScr_FS_FClose},
    {"fs_readline",                         GScr_FS_ReadLine},
    {"fs_readall_lines",                    GScr_FS_ReadAllLines},
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
    std::memset(s_scriptFiles, 0, sizeof(s_scriptFiles));

    Scr_GetFunction_Detour = Detour(Scr_GetFunction, Scr_GetFunction_Hook);
    Scr_GetFunction_Detour.Install();
}

GSCFunctions::~GSCFunctions()
{
    CloseAllScriptFiles();
    Scr_GetFunction_Detour.Remove();
}
} // namespace mp
} // namespace t4
