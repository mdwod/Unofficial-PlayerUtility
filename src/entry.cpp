// Unofficial PlayerUtility
// A Raidcore Nexus add-on for Guild Wars 2.
//
// What it does: listens to the Unofficial Extras squad events (relayed by Nexus'
// ArcDPS integration), shows everybody in your party/squad in a window and looks
// each account up on killproof.me and GW2 Wingman. Thresholds and per-account
// notes help with organizing groups.
//
// Requirements at runtime: Nexus + ArcDPS + Unofficial Extras installed.
// Build: x64 DLL, based on RaidcoreGG/Nexus-Template-cpp (use the Nexus API and
// ImGui versions that the template pins, they must match the host).

#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "nexus/Nexus.h"

static AddonDefinition_t AddonDef{};
static AddonAPI_t* APIDefs = nullptr;

// ---------------------------------------------------------------------------
// Unofficial Extras payload (mirrors the public MIT-licensed UE API header;
// double-check against arcdps_unofficial_extras_api.h if fields look wrong).
// ---------------------------------------------------------------------------
enum class UserRole : uint8_t
{
    SquadLeader = 0,
    Lieutenant = 1,
    Member = 2,
    Invited = 3,
    Applied = 4,
    None = 5, // sent when a user leaves -> remove from list
    Invalid = 6
};

struct UeUserInfo
{
    const char* AccountName; // includes leading ':'; only valid during the callback
    uint32_t    JoinTimeLow;
    uint32_t    JoinTimeHigh;
    UserRole    Role;
    uint8_t     Subgroup;
    bool        ReadyStatus;
    uint8_t     _unused1;
    uint32_t    _unused2;
};

struct UeSquadUpdate
{
    const UeUserInfo* Users;
    uint64_t          Count;
};

// ---------------------------------------------------------------------------
// Our own copy of the data (events can arrive on another thread than rendering)
// ---------------------------------------------------------------------------
struct Member
{
    std::string Account;
    UserRole    Role = UserRole::None;
    uint8_t     Subgroup = 0;
    bool        InGroup = true; // false = only shown because it is you (not in a group)
    bool        IsLinked = false; // true = a linked account, shown below the account that is in the group
    int         LinkedCount = 0;  // number of linked accounts that can be expanded below this player
};

static std::mutex          g_mutex;
static std::vector<Member> g_members;
// Version of the release (0.3.1). Used for the Nexus add-on list and for the User-Agent of the web requests.
static constexpr int VERSION_MAJOR = 0;
static constexpr int VERSION_MINOR = 3;
static constexpr int VERSION_BUILD = 1;
static constexpr int VERSION_REVISION = 0;

static bool                g_visible = false; // hidden until the keybind is pressed

// ---------------------------------------------------------------------------
// Settings: column width and text alignment (0 = left, 1 = center, 2 = right)
// ---------------------------------------------------------------------------
namespace fs = std::filesystem;

static constexpr int COL_COUNT = 7;
static constexpr int COL_ACCOUNT = 0;
static constexpr int COL_GROUP = 1;
static constexpr int COL_LI = 2;
static constexpr int COL_UFE = 3;
static constexpr int COL_DATE = 4; // "Note Date"
static constexpr int COL_NOTES = 5; // "Account Note"
static constexpr int NOTE_MAX_BYTES = 1000; // longest account note (in bytes; letters with accents or emoji take 2 - 4 bytes)
static constexpr int COL_KPID = 6; // "Kp.me", the killproof.me code (click to open the profile)
static const char* COL_NAMES[COL_COUNT] = { "Account Name", "Group", "LI", "UFE", "Note Date", "Account Note", "Kp.me" };

// "custom = false" means: use whatever Nexus' theme says
struct ColorSetting { bool custom = false; float c[4] = { 0.20f, 0.20f, 0.30f, 1.00f }; };
struct FloatSetting { bool custom = false; float v = 1.0f; };

static float g_width[COL_COUNT] = { 150.0f, 50.0f, 60.0f, 60.0f, 130.0f, 300.0f, 80.0f };
static int   g_align[COL_COUNT] = { 0, 1, 0, 0, 0, 0, 0 };
static int   g_order[COL_COUNT] = { COL_ACCOUNT, COL_KPID, COL_GROUP, COL_LI, COL_UFE, COL_DATE, COL_NOTES }; // table position -> column id

static ColorSetting g_headerBg = { true,  { 0.0f, 0.0f, 0.0f, 0.0f } };                    // header row background
static ColorSetting g_tableBorder = { true,  { 0.397256f, 0.397256f, 0.39726f, 0.356164f } }; // lines inside/around the table
static ColorSetting g_windowBorder = { false, { 0.243114f, 0.243121f, 0.484018f, 1.0f } };     // border around the whole window
static ColorSetting g_noteBg = { true,  { 0.0f, 0.0f, 0.0f, 0.0f } };                    // background of the note cell
static ColorSetting g_noteText = { false, { 1.0f, 1.0f, 1.0f, 1.0f } }; // text color in the note field
static ColorSetting g_hintColor = { false, { 0.6f, 0.6f, 0.6f, 1.0f } };                // placeholder text (search box and note fields)
static ColorSetting g_clickHlColor = { true, { 1.0f, 1.0f, 1.0f, 22.0f / 255.0f } };      // highlight of the clicked row and column (#FFFFFF16)
static ColorSetting g_searchBg = { false, { 0.10f, 0.10f, 0.12f, 1.0f } };               // background of the search box
static float        g_highlight[4] = { 1.0f, 0.82f, 0.0f, 0.45f };   // color behind found search text
static bool         g_copyToLinked = true;         // a new comment is also written to empty notes of linked accounts
static bool         g_linkedExpanded = false;        // linked accounts start expanded (the arrow toggles each player)
static int          g_maxRows = 10;               // window grows with the players up to this many rows
static bool         g_headerBold = false;            // bold header text
static bool         g_tooltips = true;             // show tooltips
static bool         g_debug = false;            // show the Debug tab (the log itself is always kept); saved
static bool         g_clickHighlight = true;         // click a cell: its row and column are highlighted
static bool         g_dataTooltips = true;           // show the value tooltips (LI, UFE, raids, strikes) - own switch
static bool         g_quickAccess = true;            // icon in the Nexus Quick Access bar
static bool         g_tooltipBold = true;            // bold first line (boss / strike name, "Combined Value") in tooltips
static FloatSetting g_headerHeight = { true, 22.0f };
static FloatSetting g_rowHeight = { true, 22.0f };   // height of each player row
static FloatSetting g_windowBorderSize = { false, 6.0f };

// Raid tab
static constexpr int RAID_WING_COUNT = 14; // W1 to W8, W8 Cm, then W3 Cm to W7 Cm (the last five only have data from Wingman)
static constexpr int STRIKE_COUNT = 10; // strike missions (Strikes tab)
static bool  g_strikeExpanded[STRIKE_COUNT] = {}; // strike shows Nm and Cm columns (triangle in the header)
static int   g_strikeCollapsed = 0;               // collapsed strike columns show: 0 = Cm value, 1 = Nm (normal) value
static bool  g_raidExpanded[RAID_WING_COUNT] = {}; // wing shows all its bosses (triangle in the header); not stored, all wings start collapsed
static float g_wingWidth = 40.0f;                // width of a collapsed wing column
static float g_bossWidth = 44.0f;                // width of a boss column of an expanded wing
static int   g_dataSource = 0;                     // 0 = Kp.me, 1 = Wingman (always Kp.me when the game starts, not saved)
static bool  g_showIcons = true;                   // icons instead of the boss / strike shortcuts in the headers
static float g_iconSize = 24.0f;                  // icon size in the headers (px)
static bool  g_showTriangles = true;               // triangles in the wing / strike headers (false = narrower columns)
static int   g_bossAlign = 1;                    // alignment of the numbers (0 left, 1 center, 2 right)
static int   g_cofferValue = 3;                    // how many tokens one coffer counts as (0..5)
static int   g_activeTab = 0;                    // 0 General, 1 Raid, 2 Raid Cm, 3 Strikes, 4 Strikes Cm, 5 Comments, 6 Debug (tab drawn last frame)
static float g_tableBottomReserve = 0.0f;           // height kept free under the table for a fixed bar (Comments tab: the Add button)
static float g_extraHeight = 0.0f;                 // height of a horizontal scrollbar, added to the window height

// Threshold highlight (dropdown + number field next to the search box)
enum { THR_KP = 0, THR_LOGS = 1, THR_LI = 2, THR_UFE = 3, THR_COUNT = 4 };
static int   g_thrKind = 0;                    // THR_KP (Kp.me numbers of the raid / strike tabs), THR_LOGS (Wingman logs there), THR_LI / THR_UFE (general tab)
static int   g_thrValue[THR_COUNT] = { -1, -1, -1, -1 }; // per kind; -1 = empty = off
static char  g_thrText[THR_COUNT][8] = {};         // what is typed in the field, per kind
static int   g_thrStyle = 0;                    // 0 cell background, 1 number text, 2 entire row
static float g_thrCellColor[4] = { 1.0f, 0.15f, 0.15f, 0.45f };
static float g_thrTextColor[4] = { 1.0f, 0.30f, 0.30f, 1.00f };
static float g_thrRowColor[4] = { 1.0f, 0.15f, 0.15f, 0.20f };

static void SyncThresholdText()
{
    for (int i = 0; i < THR_COUNT; ++i)
    {
        if (g_thrValue[i] >= 0) snprintf(g_thrText[i], sizeof(g_thrText[i]), "%d", g_thrValue[i]);
        else                    g_thrText[i][0] = '\0';
    }
}

// Values of the right-click menu of the threshold field (set in the options, Colors tab)
static std::vector<int> g_thrPresets = { 25, 50, 100, 250, 10000, 25000 };
static char             g_thrPresetText[160] = "25, 50, 100, 250, 10000, 25000";

// "25, 50 100;250" -> sorted list without duplicates (at most 12 values, 0 - 999999)
static std::vector<int> ParsePresets(const char* text)
{
    std::vector<int> out;
    long long cur = -1;
    for (const char* c = text;; ++c)
    {
        if (*c >= '0' && *c <= '9')
        {
            cur = (cur < 0 ? 0 : cur) * 10 + (*c - '0');
            if (cur > 999999) cur = 999999;
        }
        else
        {
            if (cur >= 0) out.push_back((int)cur);
            cur = -1;
            if (!*c) break;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > 12) out.resize(12);
    return out;
}

static void SyncPresetText()
{
    std::string t;
    for (size_t i = 0; i < g_thrPresets.size(); ++i)
    {
        if (i) t += ", ";
        t += std::to_string(g_thrPresets[i]);
    }
    snprintf(g_thrPresetText, sizeof(g_thrPresetText), "%s", t.c_str());
}

static int   g_tableGen = 0;      // changing this makes ImGui rebuild the table
static fs::path g_settingsPath;

// ---------------------------------------------------------------------------
// Debug log: always on. The last entries are kept in memory (shown in the Debug tab, if enabled) and every
// entry is also written to debug_log.txt in the add-on folder. The file is deleted when the add-on starts and
// never grows beyond about 200 KB. Account names are logged (joins, leaves, lookups), Kp.me IDs are not;
// the first line of the file reminds you to check it before sharing.
// ---------------------------------------------------------------------------
static constexpr const char* SRC_NEXUS = "Nexus";
static constexpr const char* SRC_EXTRAS = "Unofficial Extras";
static constexpr const char* SRC_KP = "killproof.me";
static constexpr const char* SRC_WINGMAN = "Wingman";
static constexpr const char* SRC_ADDON = "Unofficial PlayerUtility";
static constexpr const char* DST_ADDON = "Unofficial PlayerUtility";
static constexpr const char* DST_SETTINGS = "Settings.txt";
static constexpr const char* DST_NOTES = "Notes.txt";
static constexpr const char* DST_NONE = "-";

struct LogEntry { std::string time, source, text, dest, account; }; // account: the account the entry is about ("" if none)
static std::mutex           g_logMutex;
static std::deque<LogEntry> g_logEntries;
static fs::path             g_logPath;
static std::uintmax_t       g_logBytes = 0;
static constexpr size_t         LOG_MAX_ENTRIES = 500;
static constexpr std::uintmax_t LOG_MAX_BYTES = 200 * 1024;

// first line of debug_log.txt (a comment; the log lines below are tab-separated)
static const char* const LOG_HEADER = "# This log contains account names. Check it and remove them before you share this file.";

// starts the file with the remark. Call with g_logMutex held and g_logPath set.
static void WriteLogHeader()
{
    std::ofstream out(g_logPath, std::ios::binary | std::ios::trunc);
    out << LOG_HEADER << '\n';
    g_logBytes = std::strlen(LOG_HEADER) + 1;
}

// the file got too big: keep the remark and only the newer half of the log lines. Call with g_logMutex held.
static void TrimLogFile()
{
    std::vector<std::string> lines;
    {
        std::ifstream in(g_logPath, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    if (!lines.empty() && lines[0][0] == '#') lines.erase(lines.begin()); // the remark is written again below
    const size_t from = lines.size() / 2;
    WriteLogHeader();
    std::ofstream out(g_logPath, std::ios::binary | std::ios::app);
    for (size_t i = from; i < lines.size(); ++i)
    {
        out << lines[i] << '\n';
        g_logBytes += lines[i].size() + 1;
    }
}

// Call right before a DebugLog line that is about one account; the Debug tab uses it for the right-click menu.
static thread_local std::string t_logAccount;
static void LogAccount(const std::string& account) { t_logAccount = account; }

static void DebugLog(const char* source, const char* dest, const char* fmt, ...)
{
    const std::string account = t_logAccount;
    t_logAccount.clear();
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    for (char* c = text; *c; ++c)
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';

    char stamp[16] = "--:--:--";
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    if (localtime_s(&tmv, &now) == 0) std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logEntries.push_back({ stamp, source, text, dest, account });
    while (g_logEntries.size() > LOG_MAX_ENTRIES) g_logEntries.pop_front();

    if (g_logPath.empty()) return;
    const std::string line = std::string(stamp) + '\t' + source + '\t' + text + '\t' + dest + '\n';
    {
        std::ofstream out(g_logPath, std::ios::binary | std::ios::app);
        if (!out) return;
        out << line;
    }
    g_logBytes += line.size();
    if (g_logBytes > LOG_MAX_BYTES) TrimLogFile();
}

// Called when the add-on starts: the old file is removed and a fresh one begins
static void StartDebugLog(const fs::path& dir)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath = dir / "debug_log.txt";
    std::error_code ec;
    fs::remove(g_logPath, ec);
    g_logBytes = 0;
    g_logEntries.clear();
    WriteLogHeader();
}

static void ClearDebugLog()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logEntries.clear();
    g_logBytes = 0;
    if (!g_logPath.empty()) WriteLogHeader();
}


// --- defaults, one function per options tab (also used for new users) ---
static void ResetLayoutSettings()
{
    g_headerBold = false;
    g_tooltipBold = true;
    g_headerHeight = { true,  22.0f };
    g_rowHeight = { true,  22.0f };
    g_windowBorderSize = { false, 6.0f };
}

static void ResetColumnSettings()
{
    // columns: Account, Group, LI, UFE, Note Date, Account Note, Kp.me
    const float w[COL_COUNT] = { 150.0f, 45.0f, 60.0f, 60.0f, 130.0f, 220.0f, 45.0f };
    const int   a[COL_COUNT] = { 0, 1, 0, 0, 0, 0, 0 };
    const int   o[COL_COUNT] = { COL_ACCOUNT, COL_KPID, COL_GROUP, COL_LI, COL_UFE, COL_DATE, COL_NOTES };
    for (int i = 0; i < COL_COUNT; ++i) { g_width[i] = w[i]; g_align[i] = a[i]; g_order[i] = o[i]; }

    g_wingWidth = 30.0f;
    g_bossWidth = 30.0f;
    g_bossAlign = 0;
    g_showTriangles = false;
}

static void ResetColorSettings()
{
    g_headerBg = { true,  { 0.999991f, 0.99999f, 1.0f, 0.273973f } };
    g_tableBorder = { true,  { 0.51141f, 0.51141f, 0.511415f, 0.356164f } };
    g_windowBorder = { false, { 0.243114f, 0.243121f, 0.484018f, 1.0f } };
    g_noteBg = { true,  { 0.0f, 0.0f, 0.0f, 0.0f } };
    g_noteText = { false, { 1.0f, 1.0f, 1.0f, 1.0f } };
    g_hintColor = { true,  { 1.0f, 0.99999f, 0.99999f, 0.365297f } };
    g_searchBg = { true,  { 0.237441f, 0.237441f, 0.237443f, 0.196347f } };
    g_clickHlColor = { true,  { 1.0f, 1.0f, 1.0f, 22.0f / 255.0f } };
    g_highlight[0] = 1.0f; g_highlight[1] = 0.82f; g_highlight[2] = 0.0f; g_highlight[3] = 0.45f;

    g_thrPresets = { 25, 50, 100, 250, 10000, 25000 };
    SyncPresetText();
    g_thrStyle = 2;
    const float c0[4] = { 1.0f, 0.15f, 0.15f, 0.45f }, c1[4] = { 1.0f, 0.30f, 0.30f, 1.00f }, c2[4] = { 1.0f, 0.15f, 0.15f, 0.20f };
    for (int i = 0; i < 4; ++i) { g_thrCellColor[i] = c0[i]; g_thrTextColor[i] = c1[i]; g_thrRowColor[i] = c2[i]; }
}

static void ResetGeneralSettings()
{
    g_maxRows = 15;
    g_tooltips = true;
    g_dataTooltips = true;
    g_clickHighlight = true;
    g_quickAccess = true;
    g_showIcons = true;
    g_iconSize = 30.0f;
    g_linkedExpanded = false;
    g_copyToLinked = true;
}

static void ResetAdvancedSettings()
{
    g_debug = false;
}

static void ResetDataSettings()
{
    g_cofferValue = 3;
}

static void ResetSettings()
{
    ResetGeneralSettings();
    ResetAdvancedSettings();
    ResetLayoutSettings();
    ResetColumnSettings();
    ResetColorSettings();
    ResetDataSettings();

    for (int i = 0; i < RAID_WING_COUNT; ++i) g_raidExpanded[i] = false;
    for (int i = 0; i < STRIKE_COUNT; ++i) g_strikeExpanded[i] = false;
    g_strikeCollapsed = 0;
    g_dataSource = 0;

    g_thrKind = 0;
    for (int i = 0; i < THR_COUNT; ++i) g_thrValue[i] = -1;
    SyncThresholdText();

    ++g_tableGen;
}

static void WriteColor(std::ostream& o, const char* key, const ColorSetting& s)
{
    o << key << ' ' << (s.custom ? 1 : 0) << ' '
        << s.c[0] << ' ' << s.c[1] << ' ' << s.c[2] << ' ' << s.c[3] << '\n';
}
static void WriteFloat(std::ostream& o, const char* key, const FloatSetting& s)
{
    o << key << ' ' << (s.custom ? 1 : 0) << ' ' << s.v << '\n';
}
static void ReadColor(std::istream& in, ColorSetting& s)
{
    int on = 0;
    in >> on >> s.c[0] >> s.c[1] >> s.c[2] >> s.c[3];
    s.custom = (on != 0);
}
static void ReadFloat(std::istream& in, FloatSetting& s)
{
    int on = 0;
    in >> on >> s.v;
    s.custom = (on != 0);
}

// "key value value ..." lines of a settings text, key -> everything after the key
static std::map<std::string, std::string> SettingsLines(const std::string& text)
{
    std::map<std::string, std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) lines[line] = "";
        else                         lines[line.substr(0, sp)] = line.substr(sp + 1);
    }
    return lines;
}

static std::string g_savedSettingsText; // what the settings file contains (as loaded or last written)

static void SaveSettings()
{
    std::ostringstream out;

    out << "widths";
    for (int i = 0; i < COL_COUNT; ++i) out << ' ' << g_width[i];
    out << '\n' << "aligns";
    for (int i = 0; i < COL_COUNT; ++i) out << ' ' << g_align[i];
    out << '\n' << "order";
    for (int i = 0; i < COL_COUNT; ++i) out << ' ' << g_order[i];
    out << '\n';

    WriteColor(out, "headerBg", g_headerBg);
    WriteColor(out, "tableBorder", g_tableBorder);
    WriteColor(out, "windowBorder", g_windowBorder);
    WriteColor(out, "noteBg", g_noteBg);
    WriteColor(out, "noteText", g_noteText);
    WriteColor(out, "hintColor", g_hintColor);
    WriteColor(out, "searchBg", g_searchBg);
    WriteColor(out, "clickHlColor", g_clickHlColor);
    out << "headerBold " << (g_headerBold ? 1 : 0) << '\n';
    out << "tooltips " << (g_tooltips ? 1 : 0) << '\n';
    out << "dataTooltips " << (g_dataTooltips ? 1 : 0) << '\n';
    out << "debugTab " << (g_debug ? 1 : 0) << '\n';
    out << "clickHighlight " << (g_clickHighlight ? 1 : 0) << '\n';
    out << "tooltipBold " << (g_tooltipBold ? 1 : 0) << '\n';
    out << "quickAccess " << (g_quickAccess ? 1 : 0) << '\n';
    out << "maxRows " << g_maxRows << '\n';
    out << "linkedExpanded " << (g_linkedExpanded ? 1 : 0) << '\n';
    out << "copyToLinked " << (g_copyToLinked ? 1 : 0) << '\n';
    out << "highlight " << g_highlight[0] << ' ' << g_highlight[1] << ' ' << g_highlight[2] << ' ' << g_highlight[3] << '\n';
    WriteFloat(out, "headerHeight", g_headerHeight);
    WriteFloat(out, "rowHeight", g_rowHeight);
    WriteFloat(out, "windowBorderSize", g_windowBorderSize);

    out << "wingWidth " << g_wingWidth << '\n';
    out << "bossWidth " << g_bossWidth << '\n';
    out << "bossAlign " << g_bossAlign << '\n';
    out << "showTriangles " << (g_showTriangles ? 1 : 0) << '\n';
    out << "showIcons " << (g_showIcons ? 1 : 0) << '\n';
    out << "iconSize " << g_iconSize << '\n';
    out << "cofferValue " << g_cofferValue << '\n';

    out << "thrStyle " << g_thrStyle << '\n';
    out << "thrCell " << g_thrCellColor[0] << ' ' << g_thrCellColor[1] << ' ' << g_thrCellColor[2] << ' ' << g_thrCellColor[3] << '\n';
    out << "thrText " << g_thrTextColor[0] << ' ' << g_thrTextColor[1] << ' ' << g_thrTextColor[2] << ' ' << g_thrTextColor[3] << '\n';
    out << "thrRow " << g_thrRowColor[0] << ' ' << g_thrRowColor[1] << ' ' << g_thrRowColor[2] << ' ' << g_thrRowColor[3] << '\n';
    out << "thrPresets";
    for (int v : g_thrPresets) out << ' ' << v;
    out << '\n';

    const std::string text = out.str();
    if (text == g_savedSettingsText) return; // nothing changed: no write, no log line

    {
        std::ofstream file(g_settingsPath, std::ios::trunc);
        if (!file) { DebugLog(SRC_ADDON, DST_SETTINGS, "Settings Could Not Be Saved (File Not Writable)"); return; }
        file << text;
    }

    // log what changed (the settings hold no account names)
    if (g_savedSettingsText.empty())
        DebugLog(SRC_ADDON, DST_SETTINGS, "Settings File Created");
    else
    {
        const std::map<std::string, std::string> before = SettingsLines(g_savedSettingsText), after = SettingsLines(text);
        for (const auto& kv : after)
        {
            auto old = before.find(kv.first);
            if (old != before.end() && old->second == kv.second) continue;
            DebugLog(SRC_ADDON, DST_SETTINGS, "Setting Saved: %s = %s", kv.first.c_str(), kv.second.substr(0, 150).c_str());
        }
    }
    g_savedSettingsText = text;
}

static bool OrderIsValid(const int* o)
{
    bool seen[COL_COUNT] = {};
    for (int i = 0; i < COL_COUNT; ++i)
    {
        if (o[i] < 0 || o[i] >= COL_COUNT || seen[o[i]]) return false;
        seen[o[i]] = true;
    }
    return true;
}

static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void LoadSettings()
{
    std::ifstream in(g_settingsPath);
    if (!in) { DebugLog(SRC_ADDON, DST_SETTINGS, "No Settings File Found, Defaults Used"); return; }
    {
        std::ostringstream whole;
        whole << in.rdbuf();
        g_savedSettingsText = whole.str();
        in.clear();
        in.seekg(0);
    }

    std::string key;
    while (in >> key)
    {
        if (key == "widths" || key == "aligns")
        {
            // read the rest of the line; older files have fewer values than we have columns
            std::string line;
            std::getline(in, line);
            std::istringstream ls(line);
            for (int i = 0; i < COL_COUNT; ++i)
            {
                if (key == "widths") { if (!(ls >> g_width[i])) break; }
                else { if (!(ls >> g_align[i])) break; }
            }
        }
        else if (key == "order")
        {
            std::string line;
            std::getline(in, line);
            std::istringstream ls(line);
            int tmp[COL_COUNT];
            int n = 0;
            while (n < COL_COUNT && (ls >> tmp[n])) ++n;
            if (n == COL_COUNT && OrderIsValid(tmp))
                for (int i = 0; i < COL_COUNT; ++i) g_order[i] = tmp[i];
        }
        else if (key == "headerBg")          ReadColor(in, g_headerBg);
        else if (key == "tableBorder")       ReadColor(in, g_tableBorder);
        else if (key == "windowBorder")      ReadColor(in, g_windowBorder);
        else if (key == "noteBg")            ReadColor(in, g_noteBg);
        else if (key == "noteText")          ReadColor(in, g_noteText);
        else if (key == "hintColor" || key == "searchHint") ReadColor(in, g_hintColor); // searchHint = older name
        else if (key == "searchBg")          ReadColor(in, g_searchBg);
        else if (key == "clickHlColor")      ReadColor(in, g_clickHlColor);
        else if (key == "headerBold") { int v = 0; in >> v; g_headerBold = (v != 0); }
        else if (key == "tooltips") { int v = 1; in >> v; g_tooltips = (v != 0); }
        else if (key == "dataTooltips") { int v = 1; in >> v; g_dataTooltips = (v != 0); }
        else if (key == "debugTab") { int v = 0; in >> v; g_debug = (v != 0); }
        else if (key == "clickHighlight") { int v = 1; in >> v; g_clickHighlight = (v != 0); }
        else if (key == "tooltipBold") { int v = 1; in >> v; g_tooltipBold = (v != 0); }
        else if (key == "quickAccess") { int v = 1; in >> v; g_quickAccess = (v != 0); }
        else if (key == "maxRows") { in >> g_maxRows; }
        else if (key == "linkedExpanded") { int v = 0; in >> v; g_linkedExpanded = (v != 0); }
        else if (key == "copyToLinked") { int v = 1; in >> v; g_copyToLinked = (v != 0); }
        else if (key == "highlight") { for (int i = 0; i < 4; ++i) in >> g_highlight[i]; }
        else if (key == "headerHeight")      ReadFloat(in, g_headerHeight);
        else if (key == "rowHeight")         ReadFloat(in, g_rowHeight);
        else if (key == "windowBorderSize")  ReadFloat(in, g_windowBorderSize);
        else if (key == "raidExpanded") { std::string ignored; std::getline(in, ignored); } // older files: the wing state is no longer stored
        else if (key == "strikeCollapsed") { int ignored = 0; in >> ignored; } // older files: removed option
        else if (key == "strikeOpen")
        {
            std::string line;
            std::getline(in, line);
            std::istringstream ls(line);
            for (int i = 0; i < STRIKE_COUNT; ++i)
            {
                int v = 0;
                if (!(ls >> v)) break;
                g_strikeExpanded[i] = (v != 0);
            }
        }
        else if (key == "wingWidth") { in >> g_wingWidth; }
        else if (key == "bossWidth") { in >> g_bossWidth; }
        else if (key == "raidWidths") { float oldWing = 0.0f; in >> oldWing >> g_bossWidth; } // older file: wing width was for "Wing 1", ignored
        else if (key == "bossAlign") { in >> g_bossAlign; }
        else if (key == "showTriangles") { int v = 1; in >> v; g_showTriangles = (v != 0); }
        else if (key == "showIcons") { int v = 1; in >> v; g_showIcons = (v != 0); }
        else if (key == "dataSource") { int ignored = 0; in >> ignored; } // older files: the choice is no longer saved
        else if (key == "iconSize") { in >> g_iconSize; }
        else if (key == "cofferValue") { in >> g_cofferValue; }
        else if (key == "thrKind4" || key == "thrKind3" || key == "thrKind2" ||
            key == "thrValues4" || key == "thrValues3" || key == "thrValues2")
        {
            std::string ignored; std::getline(in, ignored);
        } // older files: the threshold is no longer stored, it starts empty
        else if (key == "thrStyle") { in >> g_thrStyle; }
        else if (key == "thrCell") { for (int i = 0; i < 4; ++i) in >> g_thrCellColor[i]; }
        else if (key == "thrText") { for (int i = 0; i < 4; ++i) in >> g_thrTextColor[i]; }
        else if (key == "thrRow") { for (int i = 0; i < 4; ++i) in >> g_thrRowColor[i]; }
        else if (key == "thrPresets")
        {
            std::string line;
            std::getline(in, line);
            g_thrPresets = ParsePresets(line.c_str()); // an empty line = the user removed all values
            SyncPresetText();
        }
        else in.ignore(1 << 16, '\n'); // unknown / old-format line
    }

    for (int i = 0; i < COL_COUNT; ++i)
    {
        g_width[i] = ClampF(g_width[i], 20.0f, 600.0f);
        g_align[i] = g_align[i] < 0 ? 0 : (g_align[i] > 2 ? 2 : g_align[i]);
    }
    g_headerHeight.v = ClampF(g_headerHeight.v, 10.0f, 100.0f);
    g_rowHeight.v = ClampF(g_rowHeight.v, 10.0f, 100.0f);
    if (g_maxRows < 1)  g_maxRows = 1;
    if (g_maxRows > 50) g_maxRows = 50;
    g_windowBorderSize.v = ClampF(g_windowBorderSize.v, 0.0f, 10.0f);
    g_wingWidth = ClampF(g_wingWidth, 20.0f, 300.0f);
    g_iconSize = ClampF(g_iconSize, 16.0f, 48.0f);
    if (g_dataSource < 0) g_dataSource = 0;
    if (g_dataSource > 1) g_dataSource = 1;
    g_bossWidth = ClampF(g_bossWidth, 30.0f, 300.0f);
    if (g_strikeCollapsed < 0) g_strikeCollapsed = 0;
    if (g_strikeCollapsed > 1) g_strikeCollapsed = 1;
    if (g_bossAlign < 0) g_bossAlign = 0;
    if (g_bossAlign > 2) g_bossAlign = 2;
    if (g_cofferValue < 0) g_cofferValue = 0;
    if (g_cofferValue > 5) g_cofferValue = 5;
    if (g_thrKind < 0) g_thrKind = 0;
    if (g_thrKind >= THR_COUNT) g_thrKind = THR_COUNT - 1;
    if (g_thrStyle < 0) g_thrStyle = 0;
    if (g_thrStyle > 2) g_thrStyle = 2;
    DebugLog(SRC_ADDON, DST_SETTINGS, "Settings Loaded");
    for (int i = 0; i < THR_COUNT; ++i)
    {
        if (g_thrValue[i] < -1)   g_thrValue[i] = -1;
        if (g_thrValue[i] > 999999) g_thrValue[i] = 999999;
    }
    for (int i = 0; i < 4; ++i)
    {
        g_thrCellColor[i] = ClampF(g_thrCellColor[i], 0.0f, 1.0f);
        g_thrTextColor[i] = ClampF(g_thrTextColor[i], 0.0f, 1.0f);
        g_thrRowColor[i] = ClampF(g_thrRowColor[i], 0.0f, 1.0f);
    }
    SyncThresholdText();
}
// ---------------------------------------------------------------------------
// Player notes: account name -> note text + time the note was added
// Saved to player_notes.txt, one line per note: "account<TAB>unix time<TAB>note"
// (older files without the time, "account<TAB>note", are still read)
// ---------------------------------------------------------------------------
struct NoteEntry
{
    std::string Text;
    std::time_t Time = 0; // 0 = unknown
};

// Account names are compared without regard to case: "name.1234" and "Name.1234" are the same note.
// The key keeps the spelling it was first saved with.
struct AccountLess
{
    bool operator()(const std::string& a, const std::string& b) const
    {
        const size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i)
        {
            const int x = std::tolower((unsigned char)a[i]);
            const int y = std::tolower((unsigned char)b[i]);
            if (x != y) return x < y;
        }
        return a.size() < b.size();
    }
};

static std::map<std::string, NoteEntry, AccountLess> g_notes;
static fs::path                                      g_notesPath;

static void SaveNotes()
{
    std::ofstream out(g_notesPath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    int stored = 0;
    for (const auto& kv : g_notes)
    {
        if (kv.second.Text.empty()) continue; // don't store empty notes
        out << kv.first << '\t' << (long long)kv.second.Time << '\t' << kv.second.Text << '\n';
        ++stored;
    }
    DebugLog(SRC_ADDON, DST_NOTES, "Notes Saved (%d Notes)", stored);
}

static void LoadNotes()
{
    std::ifstream in(g_notesPath, std::ios::binary);
    if (!in) return;

    int merged = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos || tab1 == 0) continue;

        NoteEntry e;
        size_t tab2 = line.find('\t', tab1 + 1);
        if (tab2 != std::string::npos)
        {
            e.Time = (std::time_t)std::atoll(line.substr(tab1 + 1, tab2 - tab1 - 1).c_str());
            e.Text = line.substr(tab2 + 1);
        }
        else
        {
            e.Text = line.substr(tab1 + 1); // old format, no time
        }
        // the same account in two spellings: the newer note wins
        const std::string key = line.substr(0, tab1);
        auto it = g_notes.find(key);
        if (it != g_notes.end())
        {
            ++merged;
            if (it->second.Time >= e.Time) continue;
            g_notes.erase(it);
        }
        g_notes[key] = e;
    }
    if (merged > 0) DebugLog(SRC_ADDON, DST_NOTES, "Notes Loaded (%d Notes, %d Duplicate Spellings Merged)", (int)g_notes.size(), merged);
    else            DebugLog(SRC_ADDON, DST_NOTES, "Notes Loaded (%d Notes)", (int)g_notes.size());
}

// ---------------------------------------------------------------------------
// Import from the "Player List" addon (player_list.toml):
//   [[Players]]
//   comment = "..."
//   name = "account.1234"
// ---------------------------------------------------------------------------
static void AppendUtf8(std::string& out, unsigned long cp)
{
    if (cp < 0x80)         out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6));  out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// Reads one TOML string starting at s[i] (a quote). On success "i" is behind the closing quote.
static bool ParseTomlString(const std::string& s, size_t& i, std::string& out)
{
    out.clear();
    if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return false;
    const char q = s[i];
    const bool multi = s.compare(i, 3, std::string(3, q)) == 0;
    i += multi ? 3 : 1;
    if (multi && i < s.size() && s[i] == '\r') ++i;
    if (multi && i < s.size() && s[i] == '\n') ++i; // a newline right after the opening quotes is dropped

    while (i < s.size())
    {
        const char c = s[i];
        if (multi ? s.compare(i, 3, std::string(3, q)) == 0 : c == q)
        {
            i += multi ? 3 : 1;
            return true;
        }
        if (!multi && (c == '\n' || c == '\r')) return false; // unterminated single-line string
        if (q == '"' && c == '\\')
        {
            if (++i >= s.size()) return false;
            const char e = s[i++];
            switch (e)
            {
            case 'b': out += '\b'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'f': out += '\f'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case 'u':
            case 'U':
            {
                const int len = e == 'u' ? 4 : 8;
                if (i + len > s.size()) return false;
                unsigned long cp = std::strtoul(s.substr(i, len).c_str(), nullptr, 16);
                i += len;
                AppendUtf8(out, cp);
                break;
            }
            default: // line-ending backslash in multi-line strings: skip the whitespace after it
                if (multi && (e == '\n' || e == '\r' || e == ' ' || e == '\t'))
                {
                    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
                }
                else out += e;
                break;
            }
            continue;
        }
        out += c;
        ++i;
    }
    return false;
}

// UTF-8 text <-> file path (also works for names with special characters)
static fs::path PathFromUtf8(const char* text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return fs::path();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &w[0], n);
    return fs::path(w);
}
static std::string PathToUtf8(const fs::path& path)
{
    const std::wstring w = path.wstring();
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

struct PlayerListImport
{
    bool found = false;
    int  imported = 0;
    int  keptOwn = 0;   // account already had a note
    int  invalid = 0;   // no valid account name
    int  noComment = 0; // empty comment
};

// "Name.1234": a name, a dot and four digits
static bool IsAccountName(const std::string& n)
{
    if (n.size() < 6 || n[n.size() - 5] != '.') return false;
    for (size_t k = n.size() - 4; k < n.size(); ++k)
        if (n[k] < '0' || n[k] > '9') return false;
    return true;
}

static PlayerListImport ImportPlayerList(const fs::path& file)
{
    PlayerListImport r;
    std::ifstream in(file, std::ios::binary);
    if (!in) return r;
    r.found = true;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3); // UTF-8 byte order mark

    std::vector<std::pair<std::string, std::string>> players; // name, comment
    bool inPlayers = false, haveName = false, haveComment = false;
    std::string name, comment;
    auto flush = [&]()
        {
            if (inPlayers && (haveName || haveComment)) players.push_back({ name, comment });
            name.clear(); comment.clear(); haveName = haveComment = false;
        };

    size_t i = 0;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        size_t a = i;
        while (a < eol && (text[a] == ' ' || text[a] == '\t')) ++a;

        if (a < eol && text[a] == '[')
        {
            flush();
            inPlayers = text.compare(a, 11, "[[Players]]") == 0;
            i = eol + 1;
            continue;
        }
        if (inPlayers)
        {
            const bool isName = text.compare(a, 4, "name") == 0;
            const bool isComment = text.compare(a, 7, "comment") == 0;
            if (isName || isComment)
            {
                size_t eq = text.find('=', a);
                if (eq != std::string::npos && eq < eol)
                {
                    size_t v = eq + 1;
                    while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
                    std::string value;
                    size_t pos = v;
                    if (ParseTomlString(text, pos, value))
                    {
                        if (isName) { name = value; haveName = true; }
                        else { comment = value; haveComment = true; }
                        eol = text.find('\n', pos); // a multi-line string may have moved us on
                        if (eol == std::string::npos) eol = text.size();
                    }
                }
            }
        }
        i = eol + 1;
    }
    flush();

    for (auto& pl : players)
    {
        std::string n = pl.first, c = pl.second;
        while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.erase(0, 1);
        while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
        if (!IsAccountName(n)) { ++r.invalid; continue; }

        for (char& ch : c) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        while (!c.empty() && c.front() == ' ') c.erase(0, 1);
        while (!c.empty() && c.back() == ' ') c.pop_back();
        if (c.empty()) { ++r.noComment; continue; }
        if (c.size() > (size_t)NOTE_MAX_BYTES) // the note field holds this many bytes; do not cut a multi-byte character in half
        {
            size_t cut = NOTE_MAX_BYTES;
            while (cut > 0 && ((unsigned char)c[cut] & 0xC0) == 0x80) --cut;
            c.resize(cut);
        }

        auto it = g_notes.find(n);
        if (it != g_notes.end() && !it->second.Text.empty()) { ++r.keptOwn; continue; }
        NoteEntry& e = (it != g_notes.end()) ? it->second : g_notes[n];
        e.Text = c;
        e.Time = std::time(nullptr);
        ++r.imported;
    }
    return r;
}

static std::string g_activeNote;       // account whose note field is being edited right now
static std::string g_keepAccount;      // same, from the previous frame (this account is never filtered out)

// "Add Note" / "Edit Note" from the Debug tab: jump to the Comments tab and show the account's row
static std::string g_noteReqAccount;           // the account (empty = no request)
static bool        g_noteReqAdd = false;       // true: put the cursor into its note field
static bool        g_noteScrollPending = false;
static bool        g_noteFocusPending = false;
static bool        g_noteSeenActive = false;   // "Add Note": the note field has been in use (from then on it keeps the row itself)
static bool        g_gotoComments = false;     // switch to the Comments tab
static int         g_noteReqFrames = 0;        // the request ends after this many frames at the latest
static int         g_noteFlash = 0;            // frames left of the highlight on the row

static double g_noteLastEdit = 0.0; // time of the last key in a note box
static bool g_noteDirtyCopy = false; // the edited note should also go to the linked accounts
static std::string g_noteDirty; // account whose note was edited and not yet saved/copied

static bool SameAccount(const std::string& a, const std::string& b)
{
    return !AccountLess()(a, b) && !AccountLess()(b, a);
}

// The row of this account must not be hidden (being typed in, or just requested to get a note)
static bool IsKeptAccount(const std::string& account)
{
    if (account == g_keepAccount) return true;
    return g_noteReqAdd && !g_noteSeenActive && g_noteReqFrames > 0 && !g_noteReqAccount.empty() && SameAccount(account, g_noteReqAccount);
}

static char        g_search[128] = "";  // text in the search box
static std::string g_searchLower;       // lower-case copy, updated every frame

static void RequestNoteEdit(const std::string& account, bool add)
{
    g_search[0] = '\0'; // so the row is not hidden by a search
    g_searchLower.clear();
    if (add) g_notes[account]; // an empty entry: the Comments tab shows a row for it
    g_noteReqAccount = account;
    g_noteReqAdd = add;
    g_noteFocusPending = add;
    g_noteSeenActive = false;
    g_noteScrollPending = true;
    g_noteReqFrames = 300;
    g_noteFlash = 0;
    g_gotoComments = true;
}

// Measured while drawing the table, used to place the search box and to size the window
static float g_notesColumnScreenX = -1.0f; // left edge of the Account Note column (screen x)
static float g_tableTopLocalY = -1.0f; // top of the table, measured from the window's top
static float g_measuredHeader = -1.0f; // height of the header row
static float g_measuredPitch = -1.0f; // distance from one player row to the next
static int   g_lastRowCount = 0;     // rows shown in the table last frame
static std::string         g_self;       // own account name (without ':'), set by Nexus event
static std::atomic<bool>   g_selfKnown{ false }; // same information, readable without the lock

// ---------------------------------------------------------------------------
// Tiny JSON reader (just enough for the killproof.me answer, no extra library needed)
// ---------------------------------------------------------------------------
struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type                    type = Type::Null;
    double                  number = 0.0;
    bool                    boolean = false;
    std::string             text;
    std::vector<JsonValue>  items;   // array elements
    std::vector<std::string> keys;   // object keys ...
    std::vector<JsonValue>  values;  // ... and their values (same order)

    bool IsArray()  const { return type == Type::Array; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }

    const JsonValue* Get(const char* key) const
    {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &values[i];
        return nullptr;
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    bool Parse(JsonValue& out)
    {
        SkipSpace();
        if (!ParseValue(out, 0)) return false;
        SkipSpace();
        return p == end;
    }

private:
    const char* p;
    const char* end;

    void SkipSpace()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool Literal(const char* word)
    {
        size_t n = std::strlen(word);
        if ((size_t)(end - p) >= n && std::strncmp(p, word, n) == 0) { p += n; return true; }
        return false;
    }

    static void AppendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) { out += (char)cp; }
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6));  out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    bool Hex4(unsigned& value)
    {
        if (end - p < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            char c = *p++;
            value <<= 4;
            if (c >= '0' && c <= '9')      value |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end)
        {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }

            if (p >= end) return false;
            char e = *p++;
            switch (e)
            {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
            {
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                {
                    p += 2;
                    unsigned low = 0;
                    if (!Hex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false; // string was not closed
    }

    bool ParseNumber(JsonValue& v)
    {
        char* e = nullptr;
        double d = std::strtod(p, &e);
        if (e == p || e > end) return false;
        p = e;
        v.type = JsonValue::Type::Number;
        v.number = d;
        return true;
    }

    bool ParseArray(JsonValue& v, int depth)
    {
        ++p; // [
        v.type = JsonValue::Type::Array;
        SkipSpace();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;)
        {
            v.items.emplace_back();
            SkipSpace();
            if (!ParseValue(v.items.back(), depth + 1)) return false;
            SkipSpace();
            if (p >= end) return false;
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return false;
        }
    }

    bool ParseObject(JsonValue& v, int depth)
    {
        ++p; // {
        v.type = JsonValue::Type::Object;
        SkipSpace();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;)
        {
            SkipSpace();
            std::string key;
            if (!ParseString(key)) return false;
            SkipSpace();
            if (p >= end || *p != ':') return false;
            ++p;
            SkipSpace();
            v.keys.push_back(key);
            v.values.emplace_back();
            if (!ParseValue(v.values.back(), depth + 1)) return false;
            SkipSpace();
            if (p >= end) return false;
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return false;
        }
    }

    bool ParseValue(JsonValue& v, int depth)
    {
        if (depth > 64 || p >= end) return false; // too deep = broken or hostile data
        switch (*p)
        {
        case '{': return ParseObject(v, depth);
        case '[': return ParseArray(v, depth);
        case '"': v.type = JsonValue::Type::String; return ParseString(v.text);
        case 't': v.type = JsonValue::Type::Bool; v.boolean = true;  return Literal("true");
        case 'f': v.type = JsonValue::Type::Bool; v.boolean = false; return Literal("false");
        case 'n': v.type = JsonValue::Type::Null; return Literal("null");
        default:  return ParseNumber(v);
        }
    }
};

// pulls LI and UFE out of a killproof.me answer; returns status via out params
enum class KpStatus { NotLoaded, Loading, Loaded, NoProfile, Private, Error };

static bool ReadItemId(const JsonValue& item, long long& id)
{
    const JsonValue* v = item.Get("id");
    if (!v) return false;
    if (v->IsNumber()) { id = (long long)v->number; return true; }
    if (v->IsString()) { char* e = nullptr; id = std::strtoll(v->text.c_str(), &e, 10); return e != v->text.c_str(); }
    return false;
}

// item ids used by killproof.me (same ids as the game's items)
static constexpr long long ITEM_LEGENDARY_INSIGHT = 77302;
static constexpr long long ITEM_LEGENDARY_DIVINATION = 88485;
static constexpr long long ITEM_UNSTABLE_FRACTAL = 94020;

// ---- Raid bosses (ids from the arcdps killproof.me plugin) ----
static constexpr int BOSS_COUNT = 41;
// token2 / coffer2: a second item id that counts for the same boss (the amounts are added up)
struct RaidBoss { const char* name; const char* abbr; long long token; long long token2; long long coffer1; long long coffer2; };
static const RaidBoss RAID_BOSSES[BOSS_COUNT] = {
    { "Vale Guardian",       "VG",    77705,  0, 91203, 0 },
    { "Gorseval",            "Gors",  77751,  0, 91215, 0 },
    { "Sabetha",             "Sab",   77728,  0, 91147, 0 },
    { "Slothasor",           "Sloth", 77706,  0, 91160, 0 },
    { "Matthias",            "Matt",  77679,  0, 91252, 0 },
    { "Escort",              "Esc",   78873,  0, 91262, 0 },
    { "Keep Construct",      "KC",    78902,  0, 91187, 0 },
    { "Xera",                "Xera",  78942,  0, 91182, 0 },
    { "Cairn",               "Cairn", 80623,  0, 91186, 0 },
    { "Mursaat Overseer",    "MO",    80269,  0, 91191, 0 },
    { "Samarog",             "Sam",   80087,  0, 91267, 0 },
    { "Deimos",              "Dei",   80542,  0, 91233, 0 },
    { "Soulless Horror",     "SH",    85993,  0, 91211, 0 },
    { "River of Souls",      "River", 85785,  0, 91244, 0 },
    { "Statues of Grenth",   "Stat",  85800,  0, 91138, 0 },
    { "Dhuum",               "Dhuum", 85633,  0, 91220, 0 },
    { "Conjured Amalgamate", "CA",    88543,  0, 91157, 0 },
    { "Twin Largos",         "Twins", 88860,  0, 91166, 0 },
    { "Qadim",               "Qadim", 88645,  0, 91237, 0 },
    { "Cardinal Adina",      "Adina", 91246,  0, 91200, 0 },
    { "Cardinal Sabir",      "Sabir", 91270,  0, 91241, 0 },
    { "Qadim the Peerless",  "QtP",   91175,  0, 91260, 0 },
    { "Greer",               "Greer", 104047, 0, 103783, 104306 },
    { "Decima",              "Dec",   103754, 0, 103926, 104410 },
    { "Ura",                 "Ura",   103996, 0, 103946, 104439 },
    // W8 Cm: the "Magnificent Coffer" items; like the killproof.me plugin they count 1:1 (two ids per boss)
    { "Greer Cm",            "GrCm",  104399, 107171, 0, 0 },
    { "Decima Cm",           "DecCm", 104246, 106934, 0, 0 },
    { "Ura Cm",              "UraCm", 104355, 106938, 0, 0 },
    // Cm of the other wings: killproof.me has no numbers for them, only Wingman (ids at WM_RAID_ID)
    { "Keep Construct Cm",       "KC Cm",    0, 0, 0, 0 },
    { "Cairn Cm",                "CairnCm",  0, 0, 0, 0 },
    { "Mursaat Overseer Cm",     "MO Cm",    0, 0, 0, 0 },
    { "Samarog Cm",              "SamCm",    0, 0, 0, 0 },
    { "Deimos Cm",               "DeiCm",    0, 0, 0, 0 },
    { "Soulless Horror Cm",      "SH Cm",    0, 0, 0, 0 },
    { "Dhuum Cm",                "DhuumCm",  0, 0, 0, 0 },
    { "Conjured Amalgamate Cm",  "CA Cm",    0, 0, 0, 0 },
    { "Twin Largos Cm",          "TwinsCm",  0, 0, 0, 0 },
    { "Qadim Cm",                "QadimCm",  0, 0, 0, 0 },
    { "Cardinal Adina Cm",       "AdinaCm",  0, 0, 0, 0 },
    { "Cardinal Sabir Cm",       "SabirCm",  0, 0, 0, 0 },
    { "Qadim the Peerless Cm",   "QtP Cm",   0, 0, 0, 0 },
};
struct RaidWing { int first; int count; }; // the last boss of the wing is first + count - 1
static const RaidWing RAID_WINGS[RAID_WING_COUNT] = {
    { 0, 3 }, { 3, 2 }, { 5, 3 }, { 8, 4 }, { 12, 4 }, { 16, 3 }, { 19, 3 }, { 22, 3 }, { 25, 3 },
    { 28, 1 }, { 29, 4 }, { 33, 2 }, { 35, 3 }, { 38, 3 } // W3 Cm, W4 Cm, W5 Cm, W6 Cm, W7 Cm
};

// Strike missions. Each has a normal and a challenge mode (Cm) proof, two item ids each that are added up.
// Item ids from the killproof.me plugin. A single id per proof is set with "=", two ids are added with "+=".
struct StrikeDef { const char* name; const char* abbr; long long n1, n2; long long c1, c2; };
static const StrikeDef STRIKES[STRIKE_COUNT] = {
    { "Aetherblade Hideout (Mai Trin)",  "AH",   95638,  107067, 97269,  106935 },
    { "Xunlai Jade Junkyard (Ankka)",    "XJJ",  95982,  106954, 96638,  106999 },
    { "Kaineng Overlook (Minister Li)",  "KO",   97451,  107026, 96419,  107028 },
    { "Harvest Temple (Dragonvoid)",     "HT",   97132,  106956, 95986,  106910 },
    { "Old Lion's Court",                "OLC",  99165,  106951, 99204,  107132 },
    { "Cosmic Observatory (Dagda)",      "CO",   100068, 107087, 101172, 106940 },
    { "Temple of Febe (Cerus)",          "ToF",  100858, 107114, 101542, 107065 },
    { "Kela, Seneschal of Waves",        "Kela", 107033, 0,      107608, 0      },
    { "Nexus of Eternity (Vloxx)",       "NoE",  110231, 0,      110245, 0      },
    { "Boneskinner (Ritual Vial)",       "BS",   93781,  0,      0,      0      }, // last column
};
static bool StrikeHasCm(int i) { return STRIKES[i].c1 != 0; }

// ---- raid boss icons: 48x48 PNG pictures (boss portraits), embedded as base64 so no files or web requests are needed ----
static constexpr int ICON_COUNT = 25;
static const char* const ICON_B64[ICON_COUNT] = {
    "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", // Vale_Guardian
    "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", // Gorseval
    "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAABgFBMVEX78tv86Mv93sL43sL41LPxy63uw6fruJzX3r/ZxaHispbVr5C6w6t8xLissJd6sKXTpIrQmnyzoX2WnXuyjnK3f22RjXWTe2Vmo6pTnbB9jWhSj513f3FzfFRefnRFfZOealODZVBwcF9vY0teb11aZlpfXkpQXURrVEBYU0BQU0tNUjhfSzhMSj5fPypKQTI6Z4MxVVxBTEAiTV5BQzRAPjQtQlwVQVc+OTI2OCs6MSgxMCwcNmAVNEEtMScYMy8nJ0orKCcsKSIoJCMyJxcnJBoiJRsiIBwiHiohHRogF0AeGSQlHhIlGA4eHBUeFxIWKEQMKD0SH0kSHTQSIyUTHSAbGhkRGh8aFjQZFSEZFxMZFBIVFSQVFA4OFiYFFiEZEDYXECwTEC4VDDETCygUEBwUEQ0SDCMUDQ4NECsMCywMDR4OCh4PEA4PDBMJDRELChYRCCsPCCAKBioMBx4NCBoKCBoJBRoOBwwKBxAJBg4IBg8HAwsFBQ8EAwoCAQYUYrLjAAAH4klEQVR42i2VaVfqSBeFIyIzyCQQRgMyJTIJyIyQgOnwIlPsLFYwDBKJKGm5XBC9iPz1t1zdtVa+1VN7n31OVaDvz4Ug7A5/lpvl09PidTjpCxt+v63EPY54/LqzvbkZjXabBdi0WH5+/vmAvver1Wb/vd/tl5vFckkL39u/rhKOU3cigbodsOftpjT6Xqy329Vq/7X7/gPtlvxqu12vVu8bYf4+Gu4TtlO7O1i7A6vWLNvgUul+9D7dbjfr/e5zv4Q+N6vVYrdZ86v1ek7TLzbtRQ4X22yr2azFYjXu1PF2fz9c8Zvtdvv19WcJ7RYCvzhsASC8Dumt4zSIlTmWbVE4Xq/lY66wIz66odfr9+3m8+t7BwBB4Df77Yrn5x16XvQHLCmcFVlmNmjX09gtpnTZSiOanq/fwf7v/Y/CYvO+3PC/eLozfHQEjOE0KTIMN2uTzVk+kHIp/fBbtyvwq93+e7f/hJYgnMXr4vF12KG3cbfRGKPY2WA2oBgmm8bL9pTJYku83b+vhO3+awssbRYbwAD/3cy84nYpws367OOZZRgCn3HlgNNudwUcwBMIdve1fl9CAkh/P+m/DTu9qQdVen3hQJV9Y8hqAUUjeMtu9zrrjnh3uAf9EngAvL9P+GW/Q5c602u/yWTxmsotFPX6s9yhHkGDKV3OjyfgOf36/ovn+fUGAvXy36+dTq/Xiwd0dq89d2mUQhCkUPhmBdSp8wbRwjNcpIevL1NeWK2hxWq9OYy6xfH0GvX5YgFfXio5gn7WsZlCUZMdK5Q/PPE5PVo8ToRPYGnCfy5pUEAvkfLFZq6WUXr8L3B0YiUjmMlfLTwnYLo7FCY8vwCWtusfR+NpMREIl/MpXPYDAORIciy7TKOBi2q2XYK7AHgQpvw79Lnb7/ev4/Hv60Q4n47lQhKpRPKDHB9LJUY8kvKThRYBZ7r3Dw/96eoVWn7+AIuXaSKN5VKRulWuPjkGp0sAKFXnc5EgRVB1rSdz8/D0MF0vIDAYr6NR/3cxfhfDUoG6Wa23+iwqmUymtoasSLWcopg6Z4IzpeHT0wQAj/xjl+42fnsSH3jQFWip1RYrcomoZarzEIJcpqkCI+KtsrZLP0wmk4UATfhHmqYbvx1XH/WgHa3r28/hMHJmkBrOQ6FwLEsS3KCM39k8nX6/0Z80IEF4pEeTxhS+n1FBu7cm0yNmo1GlP9KDZUZiHDEYYO1QBO7O+42JAIDVnB4+NHpadtBK6Zx5lVRlUEmk8iOVXCLTW7MiwbQCXMDnyDw2+o+rCcT/XExhWtSK7XpdocHVMtDpI5lKolJJjqTm+oxgKH8Os8c980ZjOp1CvECDenpF+JktlzWKvEEuA6nKVVK96uRYZmVmVYa6wAImFKb5SmM6hvpAgKZ7RYfYKmAKTdoqB62WyOUSlV4mlSOEWKgyTszqcsElocJvH6E+DxTue0lYbOHYkSmCGFSyY6nhTH92JpfKQm2ynCYv0AiGwde/GsK0AU3mnVKplEnaWApPKXTpkMEgP5GqzpHQmUx2gswi5Qt/2k/iTcd1owGShSbT/wCGyGE6O1o1GkDd0qMT2QkAzIeg98LpjVQHdQBUfvow+TWe39+UEtqPajZozwVYq14vk4EC9HqVSqbKt3VOizcS4Z5/gEm/Dwnrx/mwVEpqvwi07LrN1SOgD2dnZwbw6ZVKfTNyhToD6RwTr1QqfQDwLy/AUzeuPQy8YraFFyggoQKZAgm1+sTcpEgym6ay1USjUmlMgAI/LiUzXYftwJpw8EISVNtg0BvOzoGIQW9uciTLUs8UdwNqbkz+BTrJDA07DmVdhMNFgvjOm0HZR8cncjOCtNP4YMC2fOW4p1OpCGCWFj9AsquNf5U1wcPtPwRTpepm2QkYJ3PoDDmYwBNIln052FMEjkBKm9W4k0kWtYVsy4TObllSpArscwj0AUGs5rxXc6gSZAEtw/HkT6x9aMWPu8lk3DbzfthRlmwRb1SVFAdWnRUAoZgSjGuLaF21bZ5isTIB0zpZ9TKZIuxp2sVAiiMZgqXApRcvlZDSdXkXDhNi+raaSzWvtAAYC08Qz3dKxaK2VPeKqdysxTLigCEYBkcslnAtZjGlD+XWIB3E7uJwsdjY/AONx5lMEnbP/OmvNEtxTOF/hw+CYRkrnq0jl6Yrl6sucuVyMHiX8BR74DEej4tJGB5EcLZAiOxHVvQ7DyTxJ4+wTE6Zvrj1GbG7Gkk2MawNAPob+vUS13pEkpuRpEgOuBl1pbMciEE+3460kUssZnY1o1GWaGGu25t4r7OHfsW1ib9ZbparcgwDFEwarw49zFy+prVZi2I1a/QuijTBP8yYExPX9BICl9MD34gc1xJnYlp0KxSQ8/SvmtEYUptDH9EoEqpFo2CgDi5XGvP8XkK2YtFhuxJRZz50HsprCk4I0rjdYaXPalSHorXQ83ModPdRp8RI2K6z3Rwgt83m1uhytwpX1GyO+hUWp0LhdqK1KGLR1KJqjcYZMSN3z1w17avrFP4DVHNpgjqFibuCclbZeRRVKC5MllxMCRmR8MFsDTuPpZYwEuWy1UC1rjk9QGal05vSaNx/W3R5ldHrTzkV/iur8tJyZD74zJQGvTXGCt4QyDCCf1lO3yC18SKfvtI53YTOG7W7E6koqnEaKTTilSLWCKrRYEbcf1ElCAZvt72no/8DUyMxKruR5SQAAAAASUVORK5CYII=", // Sabetha
    "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAABgFBMVEXx8dXi26rj0aPN0KbXxIrHwJbRs3nFsYCt1o+vwpa7soSqsI+B1H9gz25zu2lKtlrGo2m3oG2qo3mmmWq9jFyoj1qekmych16UlnSTiFdvmWR9hmJBpU0/l0lCikg2hj67eEKfdUOPfVePc1GjaDaOaT6lWy6OWSyAek9/c09+bEp8bDh8ZEV8YzN6WzJ6VCw/fUI5eD1tZkZPbD8vdzYybjYyZjUpYy5tWzdjWzllUzdmUilRVjgrWy8oWSwjUiaMRyJwSSRgTCxiQyJVSjhUSyJTQy1UQh+ONRpxMRplOxplKxVUOh9UMxRTKhJTHghJQCRFPCJHNh4+NxxFMxpGLxc7MRg8LRdCKxRAJxE3KhU2JQ8/Hw41IQ44Ggo8DgYkSCYcRR8ZOhwWMBcvJxATKRUOKBINJBAuIQ8uHg0iHg0PIA8NIQ4KIgwNHQ4KHAwsGAojFwkrEgcjEgcPGAsZEAYHFwoHEAYlCwMaCQITCgMTBAEGCgUHBwMGAgEBAQHdpXkGAAAH8UlEQVR42h1UZ1fyTBRcugoioSO9i5H6CCIISAu9BekGpYN0UKSpf/1d3ntyTvJh5s7eO5MFv7/Hn9nxp9V6f/D5/C6b3Gaz2e0upd3ls8O3q/X+3m63euPe5/H7++/vB4x7x+PPsfPeCvj8PpfdqbDbbE67Ug7RTvj4YKd3X3vy/Tn+/vlpz36By9eet1v+AMT7fXaFTC6Xye/lV1cypVyuVCj8vnYLKnZaPn/g76/z/QeczkDbF/C7/C6nzya7OtXl5SWLxbpkXUKaTKawK2w2qN5uz+GRjsDvdLpg74DLplDYTwTWGSwKLBoFfrDYl0qlzOZs9ToBp7P3cwQtvxNO6fS5YCe5DPY+o1IBIJEpNAaFzEB4UOpK/mB7n7edToXr+APeXXA8BezvVJ7gVCoioFMZdDqZRCLTKBydkXZ+eaW89/k7NoXcNfsDLr/ztBC53C6/ujyj0lU6nc5oMKqpJAAAQifTERI4Yytc/veAy7eBQyt8LZfNrjjNyqJwVOpkKv4cjz8ncmYRAgCZDKgMQOKxZXC7ne/PI1RwOu0nMByQylGp4gmdgMNBGBxdsBnWkgAHAAEgc/hnJ8ZkDBUCD7Yr9hmNweBI1CoOR8C4oPxfArW11tRyKTyqgEZSCwC79d6ZzaECPM3lGaJF0WJQZ1AxKDSuVqThQtY5U7/Npy8suVe3w4OQzlrj3ngDo2GTsRgm0200auUnEjQKGs5kE6lU3Min0SiRw0ib1jOQGE7TUV3TDgzTEcjZAlTD1ZjN+mQWobib2dcgnOTtLZcKavXEG4fEp5PJzLsUna20+zfQOLbEIuQKTOFk8i2GZlaL19fEcyJuiL9lc0ljMB7n6PQXGjEtYWY5W+PZ8QjuQiahUBjMJSyNZq35Gg4nEok3A1VlNMbjb8+G4Ha//2r+OoQ46/3nG/4HIO2+0QjRbFJk2RK1UjiIr7bbnIquij8bDAmVgZzLalfr1S1fdGafz3rfR+AN39xIkimtNrZdER5stVo1E/FULsWncwwGvoqeeKbqskYG4FMU8/FscwQvXrfImkvo9enXVbW5KiWNBoNBx6cBRG15PlEMfMMzmc+hM66ns8H4CCoht6bUTFm1RaJUxUuZpFHFoJEARR3UPedyKYOKb0jo+HRdUKjYzDuzb1Cp37m3WatFH9s9RaujVNLKg6YJg5msIZFbZeMwizq9XqCuiRVQ4fsb1BtufJvV69XhQ0zsuNMLoMlMbTqzenvdvqYSOpXam6nh1mRDej+dbSChUsS6u7RarbY2F+Lzc9o55ZzrrdaayWASX7xmMrVayWptPorz5w+bzWx+IpS727BGzTcat1GxVMzkmjKZWDNh0AU9JhPhtaJqqy6TBw5mazKedz5BvViu/5QseoFAFz7UwkFrplnxLLLBYDL5RGF2UTUaDiazVqb0PDDobTYD0C0W8oe1xWpEEH1uC11YhS3rQ0b9ms04wFM3Gn2pVjOogCtmBTaD2XwOPj4K0d2hpDYaEZ4xl8vtVs1mJhbWbkdfUtr+o9solWISiRvhXrank/nmE3wMu57hfqVHgiiDp35+Tm53a+JOwK/tf6WPaQ0PYSBqddDqYMkeBrPpYAOWw2Khu9+n9UhYy9XrjcZwqXZYexBtNe3W06lkMpkKPZdI2Q+BznwAh14u8vhwt197zUgmfGf2WvQer+frtyLiuM0cKpVEIlHpZq+UeX2vbM/n8xkYLov5l+X+txT2apNJ753F7L3hifN/v16u0GIWIHQ6XZDMC1ksim06nsxmgCiWC/XhYrcLW1GBVnt7e4uiEhpNWD68aLhCkd5szWq1GI3EvA5M25PJBDTKhWixWyIOuxez4IKncYS8Jh73giaJESMv6tEIYhpmOH1rupa3Jp3BYAIq+Xqh0I1V1vvfZUQsFms8tSrKu4DN0VqmQVSjXlPIG9JcyZWdPsSPwdew3i0W8Zd1Y/21XxCVCv4Y9XpRoUTIi91IRKFGt1vHazR49c2n/Skk7PfLer5cWS6HRG24XuNYFEVvULeZS7qgcUXCC15xtxrFGCx5K9DrT/sdMNr/7Lsfu/VysShg1cqw4JDAQm9MogseeiO5ia4IHK9yWfd+Z2c86E8Asdj/fg0Xy3p9UQwV8sWPj+7jrUSImty4++42NNqmQxaijLDuO+1eb97rgN3+8PtV362L+Tqez2MYBn0sF6PRWOjR435ZvBrVwSYe5Z3ddwa9Xm8wBo1KfbQc7tYf9VHU7fE4Hh0eLI1XX/IFjGiEwslstmnSFG6Z8k6/32/BtUa97nC9sVqsd/tlSCoWSm8dDofJ5MDxRTpdaq6Gh6VQg+FcWWvQH5yigXm8oeFotSaW61UtEvGYpCaxVGqK4LgH2mxxO0I73LPFeNcPvf5gMv8Ew2oJ7nQ1ggYSME9PkYjbJH3KRyPDRzeqN2H1fOMQ1kQY7Id2fzqDWVrvdnui3mws6ziGvWTyjw7HE2RFuoX8UzQawj4wbFnShFCusj2FaZ2D7XqxWEITlustUa5U8lgIe/yXLxQiDqnj3z+po+i5C22bJZTMvh/05oM+ICrQ5uHysB6eqlwuRDAMoh1SqVjMFT991LV3dxjh8ZIvZK1+H0YjjVfKo6/lcl2BHmCFQgGLPDrgbQNLCuERIVck8tQ9QQ6VrehPJjNQLTXKH1/14ZJo4MUixDtMp+Zipvi9CHcG37duvOC26jj0a9dkMP4P7Ucz9tJt0jcAAAAASUVORK5CYII=", // Slothasor
    "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", // Matthias
    "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", // Escort
    "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", // Keep_Construct
    "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAABgFBMVEX///7///f//+r//9j2/vb9+tzo9+nx58Lf4tHB+vfE8+2z+PWk+PXC7OTC4di359yh597k2bfm0KjM1MLQyrLUv5TFv53UtY3Bs5Ky08eyyL+b1ciZyr6uvrOusKGPvbqMsLDHpYTQlnyxpYmwmG+aoZmEoJ2hk32Lk5bHh2mkhmWPiXmLgnKnd1mjalWIe2yHa1t6opx2k4h4iYBuiIl3f4x4fG5rf3hmd3JzcHx0b2N0ZmhzZVNpbWlbbmlnZF9XZF5vWlVjXFhcWmNcV0hzRj5hSD9aT0haRTxKWFdMUFNQSUNBSUdNQz1KPjhBPz60HR2eHByLHR2MFBRtLityGhdxERJhDxBPOTE/ODVJMCs8MS1JIiFQFBRQCAo+CQkoQzgvOTczNTIdNSk0Ly8xKyopLSsaLCYwJSQnJScyHhwnHx4fJCIQJBMeHBsSGxQvERIjEA8WFhUZDg4uBQYkBQYaBgYYAQEMEg0KCwcMBQcDBwQNAQEGAQECAgIAAAAVGqsVAAAHmklEQVR42kWV+V8xURvG52kjRRFCihpb9i0xBmHKYBhLGEtNPJYG2csS+dffM73P531v8/HT9Z37us45c27o+/t7+f09HEwGFIVjfu6fnZ39/R2xzaQ3w/pexGwzmU0Gw50vCipBtNrQN1uL4XDQr+B4mAv9YYmdK/OV3iZGelGTzWy6u/P5gkAfihLNNrRc/uo77X41ifzT7+9w01ETfJZ8j+rNhrvf8vl8oRjZrEKL5ffi46Pf7vdJ/wHQ/xK7+0I7CkfSjEerNRgMah1L+aKJdCoN/er/dvpM27cHQdAvsMuWLFUmGYNWq71SKNU6nZoFsCQLAP3fv506cQD91h/QgMPhCOEU2WxcXV0CvUqtVuvY3LF0FVouPjqdzt+3duIP9K/D7i5HKBTB6eqAAPLLy0uVUqlmLQWecAwChj4+Om9v9Tr3vw1Ah32OUArbIl6vQ/GvAOANeDA8EoWGwwVr6e2towXyvQOuUMjlCqWmBzMWdzsUZ0AtlytUjnuvC6GwQAJaAEus/q0e4IplUiksFQo5UqneLDGVE6RO/guo1DqX04OhgRAGLYcdNnWnXo+YYI1MLOYIRRKRWK8R7oWCAa9ccQkApVLnRFDEFQh5oOHHx7ADVrVOmmVSmUQi4YiuNRqjRsOFroh7hYIrl5+cnCjVTpdTd3fvQqHJYtjvD/tkPabRXJ9LrjVGFMVS1ZRRKlAlHGo5j88XCARgZVVqt8PpiUCLCTMYtutEIma02lGqMppOflarLm5HdPdfy5XikMcXnAmAKaVarXIFXNBkMFgwJElGI3itRr+UcqVyOPz4aLnFyUp3ur7c4/Hlv8CF6kLldDqhwWTItMqpdCxE5QqFXDZbTOivby035/boEzVYe/YOBArgSa68uDi9UDtAhwnTbJaTsVj0OZvNAuDFJztnAYs/kmqs0wCQCwT8o1NQAHBAk16baZBkMhb6BTLZmk0iurm9vbn1P1Qb8+nZHhv66Oj49ORUCZJDg3673SizQC6TyeQyuYr15hwAt1b8Pd343Bj2eAI5n88/OWFzXKigXrvRBpawsB8AuVymiFtuz89vbm6sT9VEbzwvHxzwWT2fDzbkFABMs9FuNGKo35fNZArZzEsY2AEtrI/Pwfjn6GutBi0EJydsD97pxQXENKoNMhnxI/58JlsAEewscGuxPNbeyRk9mpMHYO94fB542OTAEvCDhRBPOJ/NFzLFZ7uFTXxreaQSjW5tNF4rDvm8Q/DjHR4BAgKJY1gkhDgf85l8PvNas1osLHJjxx8GvTT1uQkeCviHgAB1DIBGncRCkQjiemaB7IgCL7daLewqTSarqn00b/FEojNWfnh0fHwMgEQECQRCSCGfLebzLVRzDkpjDad6y8nCWBmNx0rNtVH9P4AgiAck4EFCAHjJF8HR6lGP9nM01ZssvjF02h3PHR6L1XX0fyDxcH/v8iCFXO4lX5r0arWXUuHainfHq6mmtRp/TXXYE2pTHv8D6m9E8N7lcnmesywwYLq1UgEAVLe36mqWq681GUynqbDBcQFWiQXqca9T53C6/KBFtgQsjV5yJaP1ududlu3bn830YTpo9ioeN6G6AFsBEXHvvcvpcDgNGF3MlKYgJF16tVoeK91xmtpufhr1+nY5qKK6NqFS8XlQ0Av8uAwGrG2XUXSuNB2Pxy+vj8Zn6n2Mv2+3yykR327WEwqpTx1uNQ8C/p0uV+j9k/6C91uzAj39GtOvNTMOvla8t11tpo76Zr2Z9GL1gYOIKljAheCfNF0qrmWixXI8Gn/N6JntiaK66d7PZulwr9abNTMok3Evk9RCbpfX4zHCtVIpX1xLTMDCGBDzFFrDK3h1O1EbJvMvet7uL6I+kmnZIHDJ3lMvUjGVL2XprgjbLhcgxXqAdHGKqrYv79b07HW2IdqbSKrNMBQEJkt3VprBO8/FUvGT0rS2P8uv8WiVrFQq1N2Zf1akgZ5xD7fp9HuzVYUCPrz2+ErPbfqXl9fXdRVhtqnkarQmot2nKwE5K5SK9Jx017fbBtZrgZGlR+aUEAQclymapmeDdGgSgbdbTEWg3LPWKzhhrzWvu77ZbBf31V6rBaHjKiyRcjhcrT9Fz+ZP+uoA3rdhXNXdpalbAuf3M2aIrzbr9eYnGGCaDGQDYpFIKBSLL7U+/P1TJoM54t1dodr3PisVSvS7SdvebsBObLYTB9FrQrvCawkYUBIprDebkcj8eVeKYTKO0Lv+LBVfa6bLEMO0iHic6Cy3cUczAcnA5yISw7BGbwzHUpHoEwclfbCIc/81m9VMYlOjSRKJYDzxEFS9xd2qMnR9fq3hiGAbFgvj/tiI8XMQ0qSRiEzTsl7rYyZkFFyLRIJsEkHCHQVXpURiPOcIwT8axm2x5fqTI8Mwu1Fmivmi/Uk9EfUnPVpfyJ+IBrdBd1QFSW7PRSIRrNfor9Cn6s94phEKZdJrC0fHDJg2GfX5kSux1GTzp93B/oXyFLIYwXgCM8oG+9/Drc2YllhQjrgyRRXuVoO94iJglAnFsClSddQdR0eQEYZlehS1Jx8r4XBzPadEVFcvGm9XmJOsgjs6ljTBMqlYCpvLcW/jRP4f56o793qYl2YAAAAASUVORK5CYII=", // Xera
    "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", // Cairn
    "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", // Mursaat_Overseer
    "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", // Samarog
    "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", // Deimos
    "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", // Soulless_Horror
    "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAABdmlDQ1BJQ0MgUHJvZmlsZQAAeJx1kTlLA1EUhb9Eg+IWRAsVixRRLBIQBbGUCNpEiySCW5NMZhIhyzCTIMFWsLEQLEQbt8J/oK1gqyAIiiBiae3WiIz3JYGIJG94cz/Oe+dy5wy4wxktazdPQTZXsCKzId/i0rKv5RUP/XTRS3dcs8256EyMhuvrHpeqd0HVq/G9uqs9qdsauFqFJzTTKgjLNITXC6bibeFeLR1PCh8JBywZUPha6YkKvyhOVfhDsRWLTINb9fSl/nDiD2tpKys8IuzPZopadR71JR16biEqdUD2IDYRZgnhI0GRNTIUCErNSWb1faNl3zx58WjyNilhiSNFWrwBUYvSVZdqiK7Lk6Gkcv+fp22Mj1W6d4TA8+w470PQsgs/O47zfew4PyfQ9ASXuZo/LzlNfoq+U9P8h+DdhPOrmpbYg4st6Hs041a8LDXJdhsGvJ1B5xL03ELbSiWr6jmnDxDbkF90A/sHMCz3vau/AVhoCh3wuSoAAAGAUExURf////3//vX9/en9+ev29tvy9NTr68zg5Lvk6LLZ4LXT26HS3KTJ0KPCz5LH0o2/zpe4woi0x3mzxnuss4GmuXGmuXScsXOTsWilvmOesmGYqFqWpmKQslONqGKOj1SMl2KGtVaFnV59o1R5mE6EoU2DjU55p0x3fUaDnUZ+j0J6lUF4gEJzm0NygkJzdz1xd0RpsEFrhDpoiThpdzdbtDldkzZheTNYeUBpaDRobDxdYzJbYy1iZStbYi1WYSRWWSxOuS9OlCtRaStMaiREpyA8oyZFbyE+eC1PUypHTyFLTCFFSCU/Rx9ARxk+RRo6Qxk0mhMsmRw1dBksaB42RxU1PhQxPRYrPhkyMBQxMBYsLxYqLBYpJxEvMA4rLhApKhAkhREiTBMmLBAhLxUkJRUhIg8lJAwhIxUhHxUfHhQhHwkiHRUeHBUdGxYcGQgdHAkYZgoYJQMLZQYNJRYbGBYaFwsbGQUQGQcTEwQNDgIICQIFCQEBBgACAAEAAAAAANvBY1wAAAfrSURBVHjaJZT5d/FcG4WPeSZ42qaGiLeCUBo1NSiq5lAzranUVFS9qq+pQ/71L57v/uGsrJV9Ze879zkH/Hesz/e32aw9T3PFZNZJOhFqOW4Nh6/T2fTt/f3983Ox+Ks6LuD/+vfpsN1u8wGQ3Do1WoPFkhk/t9vD2XT6tlh8vk+njIgphgHHh/f32fD5cY4AMVeddWo02IlChaQnyzHDjIbD0Wz6On3/PFoxDkf5YtZuP85vWAKIf0NhGoP25ESLSJSWzGT5/NxsMU6vo/Z0OnxjAoL3xdvba7vVXmb5LLGA/5hnHLTwiSrQQuRSdfq5s1wyTq3WU3v4NHydLcDrbMbox/MbPhtAECf/YMG05zAMnwbonx0iFQigU0u6O98ux8vlZth+BUyvo+UyreZyuTxEzH7sYM6QgQFUhZ9vmt7diDkcFosvU1tygUDgafkGNpv5owVSq7ksHhZB+IExFsKv0ucqFUXT+8M3ve333f/w2RyOgMeGpEgOBCwIn4vUERHkDDkLg+DTVb61madh2F4Ybw6Tx6/1ut+v3F85nU5smxargQYSiS3ZiCq9p7CrGh4s/NBN/EAH4dDThKbX1Ub/b1X65Urje7v/BqSACyFBi3NuJ+32dODxsNs1Xn6y1Dm52QWxYKZaqZQZeSoRvVv9fMcoIGVzoEJQfmMxXN3iV4dWQEPumhheUzlqdq3DqM989SvlSioajXpSP1++CBCz+NmOGgqpDfaQ/VYlvKlpgvD5OHvuoILulw1pqO2qlXI5VU7dr+iDYwMggEUiag0D3Npxt0RYyGEGbZYM4YZs9kqBjZ1wpp5iynu3pRtykQMULE5LRC1zqrGQ5tQuCSzVIeqRxCP2q0cq71Q4SNhYsHnLXm+5alRY0k3QnTfT1KkA0xoMXDWJzBm/UIgMRexZSoON23DwSl8IFxp+XThjqS0nzKRbrfGzVIpplAaxmIwYVCSmtdupAk5SpAp7NmIFfaT+hGEXtcmm0xy+grdZu/PElxrEUpuSxQAGMhvJFzYFqz1N4Rp3WhM2BLu1h/Gk3mwNp9MpmI5anTEXikglVgnXHXMl15feFNUi8dAs/2Q33ZquDGT9od7pdlvt0eJ9Aabt9tMekUQgkU3Kdjuuk38k5f4tiZ9n9s1dWo/jV1iw2613HPikPVy8/wemw+HT7lGLSJW4jIUbkn4ekUph7qvzwuFxXzPhVnek+fBPvqOCB63R4vPz2MOYsrQR5FSjBOrT8pkkGl05DXZT7mf/U7PZTOEm7VLVOjBcfWEifYJZu7lFIBrCIKWUL1emJJfF8vet1o6nN4Xdk81my3/vVcbugwq+m42mCwYYtrYywe/pDSKR8kSKsu4u2v/JG+xWGNbu0jabdfTllhi7dR2cZC6et6PD85IN0eqbG44A8DVm1BPt/1KYVhvT6wtZq03f2mvkmU7dBycnLeaaAsPWPMNCGOCRDXExAwErooPfoFbrovO4m7LacPpFfl6v1Xwm77j1Oj3+1rmMe3N0YPO4IYMZFka3v/kTOLyvk+6w1er/qUqwQr3jM6HVbvNtwUTqStlzWvnHK+CyECYSl+hvI/B5/jtMkpTVlKHTcgszuZgJHWxfRm9g2OyKBXva8kcnl3PDJFHkoeX1gxan9mE3E8n68qWCw7WHhxiMDvYvo9FfQEx/+c7ulBJ2JkIkOHBlvTq3Vgcx0k2ZfPRGfs4A9RisYoDhCLSeMyyI/leB9lARK0cRPbGk9NFX6ap1X8idNlG0URKK1Rv7ukq1/noZTUF74gQI7RWae6iQt08rSgQ4+0jJdfl9xuXI6AcHhcrtW62/qiKUAYYM0EEARKPCO8aBu9mZix4gS3h1+gG9CrvSpgMldGO+7Wr7B5j7X8emc04EcNdmtrd3ouAFtp5EXMARKYj4+msddIRz9Jkk5nZv6RhbSngnL8wBsiAIS7DuCc8+zEq5ZR+NlgipiCOKlunBNV6jD+KTUKY62Oo4LhSdNF9GAMHUIED3ZIoeoVLkMnfFaLFEiHhool+24VU6x/GttoNGxurrm70vx6Y1CMTPfTMde4oK5a7g3yajiSLKk/SKhN5U3Qpkq/vgYDDINyKov9v6C8iQA93zsGQlVFJ4uQ+v4tFEnLHoFZPJdQA4Mvf31cZqm/UxwDGS2gIFdulSCQWeHnNG/LpqPxrtyXkcZa/s74tlMUfsSAzqbtQ/6HSGQO1UbwLKeDHOvvu4q5SZd8lrf5FQyYCwlCDkciLmcMVirurqXpdcd+tDoAlK98xGOs6r1yuWM/57/4XfrC/2zhiiF4cJn8Plw12ZRkPXXzcYAMkdAaW5qAS8O8J8f+2/Nl5fmu96PRQISr1e3MWUL3PfzOmS/Wq1CSw54ZdFITsxJ1CWMu5JeMxGl993WY1G/1WyRGipVPRe+pMu/yCmL1disQxI54Q7zYlUKScSLPSjlIgTFxfGa9umUoknhGw2XDLHPT4/7mr44ErS54uBVk52sOAitpAoSQRxnrlEGC90psvJN10uohyu0OMpFZN+/33uXJfyX1/HwO4UWsWMfJYQ/UABinqKnhPIbLXtfukMUVKyuUIVkUjEPf6q0czobTGQhqBq8oLNFsElD0tcKsWjuhPGYfd7kKGlBMzh81iKeCKqX7l03strmw/cQOKLvgZwJUS8J+IfvxY/PTFdf9EUYIA4KlHwhIlS3F89/cd/7bP5/weC2zG0LgXwfQAAAABJRU5ErkJggg==", // River_Of_Souls
    "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", // Broken_King
    "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", // Dhuum
    "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", // Conjured_Amalgamate
    "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", // Kenutt
    "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", // Qadim
    "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", // Adina
    "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", // Sabir
    "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAABgFBMVEX59cj784fw3dfv4I7i2rLi1YDc0orMz5vevObXsN3UxnTPuWnGu5O3s5nBtmi+rmTMptGzpH+/plivol3El8WykK2xmVixj0GimXWjmFmjjI2hi0qYk3GYiFiOhmF1kJ+nd66VdZqafVeVdEmCdYqEdl6GeUWBcEGUZ5F9Z2uFZjx2ZzuMWJOETIp+XEN6UlZmcXRoZltfYGlqXjZoVV9pVzBRVGZbUzRkSWJeSj5oTBxcSiZQS0w7SV9QSi9KSDJyQHVePGVWQDxYQBlHPWNFPjVKPyZGOR9lMGdWLF9PMTtPJElBM05BMRdDIEtAIzAvO10zMFk5MiEtLyYyKD8zJxMnJzIbJzgzHT0nHzYxIA4nHhghHj0YHToiHh0bHhs8GD4xFzYpFyouEiwzCjUiFzQjFh8iESMhCyIcFyscFCcdEygbFhQcECYbERUbCR8VEicLEjAWFg8TEAwQDCkQDAsMCCgKBxAJBQIFBQQFAScEAQ8EAgQAAAUGAQAAAACFIh0QAAAHyElEQVR42iWV+1/5fhTHRyO33JdGbmWYUcTSEAsjhdw/NZdcQvq4FIlS7V//vn2+57Fftsd5vl/nvHcuUHQXPoAJbrEr0OnshUK1jko1Cv0tgsoVtREm0LmOQocXv+vPxWO95I+WotCS3YVl0AEX5al1ustCPcpT7KOnp3saVEiN5JItgGR/F4+Pj6VgqU5WoQWbM8GyXVVUKtHpHIW5QWDUXCj5SiMh8ccEapcuhImyrdpjORatVk9KHLReswlSAAthhUC+r4st7FLDPnWoOjL4d4JGEOZxmG9r/XlMk2HmBr/7mULrXy4XJhWw/FAuUKuJMRtWG6iTI9JvIFICjU6XQBD28U+QTLBxa/d7OAQKa0AkXDuCA4lArUtXU3coeeQiSJbVSBy6UALJzcskGWaryB03/B9YcvVQKGRAhTyJzs8IXQkCvOZQOwjoIKR35NKJUKLKKPOr7nA4AQD3y3ELVygXMgh4EnlEzgsljl2sRoiqXVISU7LpdC7BMnxmdd/t9rogh9YntyVINuESCCRUjJdI8NSLI5VErdbl9Np5Ol1lc/wA1+l2h70JxH2a5gDgxkTJEQpJedJUrEQIw+yaEEj2icNTBgiwa7Gem/Z6vW63C3FzXoz75j65epo4DoV0PPWmJCXs8rlEonYdxeelXKK6jiO/y2nvmWJmHYhb8nzcNihusQ8kwgaeehok5FGSh5LHwSWTToUXY5xaToGCKfIXAPOdQ46b6yPjkiTevzk5Peahm2qQVUiNOP7QarVK0WoJ11tv20+UMXMPgCgEtbhPH1+IGombt/zRyQnPTwRJnpEMZCvd+aIUjwdwPPPUfkKxqy0w3oG2MS33UWO80shkrijUKHQQmvBNlna7adqJ43o8fkFfBBQUnn2CuIUQll1ZLBatSCQWi0V8sQ0vEun5POsWi/4Z+GqxWJ20yl5yX7VBDjAM6e89FkSrVIrFShHflryQpsblrFskslicTq3T47mm6bxHlUo5M23om5PBMJzluNW9UktfAynbGaz2B7NJ2m1xepxW/MJ9m8nkr2VEKmbJ3EIrDoMkArjMrTgTTJkwlekENFuQpJiI3oZj2EFce5HPZOIKUzxGWTIZAMQgoRTejc0fy5gMwjBV2ZGy14lSWW+Kw7AJC4jjmQuj3EhRJdxym4E+v5dCWAKiUh3EDqBdX+TQcTS2V/11n+qGCsggh1Vpk6s1jhhFza1WAMxmnAqChUIhvBtGYUeQ0ciPQ2GSdDjsUdNvShhU6jENwVarcWYs9lxnoEl3w0CQRCeFYYeDzIE+VigUEg1qdBwED+r1qAPx++1pNpdOjW1i2nMNzYYTbg8CE2NXZXQQ6WgU3VfsCHdQvzwcJUpUKpz2YfYcAKoLkZP20NDsebhx8I7V0uPh3Y0j7rJHFQSqMKSNGpaKINl51mux2XIgpPG12OOhgcLz82oApom0N5vl85RBTTpSVXtq7k+ncDFeL5vNYkSLL1iW0yppN52Bnp+fZ5uUS+2aPd9lb6kyobajhFSdkyX8fCtnPs+aRSKlmFms63wr7QY5gEHQmdyppXdPt7cZ00G6ihoVcV8wxg9josUnLrJ6lXyR2LZe74lBLYIctkDnS8N7yl/RuEQgSYexfUKLHO4Rewi3WONKrxcR8xGO4XusFjd9BYBepzO5EbTzV57Dw0CCzBn9Pr4IJ8i9yPe6XFoUL71nCLLU25wWAGQB0Mnn209HmQztcSLaUzZhvDAdYVg8gM3XY2/z6+2j4KGLXpFSa7EELhBo2AVAPr/1d1tAhZtyOI7dnJ7G49R6brtsvr19vBWTScSKgK7ASyJo2Gnn86B+AWC1aLVi0YlGit1SKgoPfOrP/lwWCs1mo5H0FrwWrSXAaKFZDwCZ6+tr2mpFlBZEaVVL7QYCTWHUmfLro9gARLExStLJJH3VLWuBQq+dyVzToK8QpafQODfbyZSBALvmUHz+9dZ8bzY/GrN+snifTLprWQR67nTat+B8CLaCbit8vZnTXM5gZ+ulPeUXyKD5sWk2VpNkEiRiWQWsUK8DMqDpaysMy5JNcClNMIhkJm7N6b2bP+D9JWJ73TSSjWK/mOQCAajTBn+A9ng8u6CJ9vYOVDKBwK4qMaU6lX17rTFMZDeyee9/rBoPldfHAAO1/wecOAOWIwzt7AgVKBqNgQajfD5frBTbO/t5f1+tVpViY1oJ1LbAbebi3BZhymaFQq5QaDQaI2qPUsCZYSr43vnP6v3jAwDZyriSnQIg325fmc9jLe4R04BOQzUoQOwmXyxW9uH62s9q9f76+vLyOhq3KrUBAHrnZjPiq/7WzL6Iz4ii28doPDT5fBgWWfUrjYf+6AXYdFyrdAfQYHAuNnu9j/P7c7PXfB7xmVAUVclkErkJZ1bA/+EBAFtiOqhUhgMoojTXWpdLbp0EQt7L7Fvj5oapRHxMa7z62cy29vrP/2UyqAymA8h81qplW7/zchLUS7/YaDTf39+/vjZbW40eXoD/62Qy2QIPYIsOoA3XimSXi3mrkCzMRo1Go7+1xmjrNHp5AGdPXv7+s25nuN1xq1o22xqMl8C/P5n0/wGjv/2/s8nk4fVnNRqNADDqdgEAtigAWgxTmw7m88tisT8CQHF7fqMIYgO38zIFFwo8H7pb63QeOt3/AFAANCWpF+t1AAAAAElFTkSuQmCC", // Qadim_The_Peerless
    "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", // Greer
    "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", // Decima
    "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", // Ura
};
// icon of every raid boss (index like RAID_BOSSES); -1 = no icon, the text is shown
static const int RAID_BOSS_ICON[BOSS_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 22, 23, 24,
                                          6, 8, 9, 10, 11, 12, 15, 16, 17, 18, 19, 20, 21 };

static bool Base64Decode(const char* s, std::vector<unsigned char>& out)
{
    static signed char table[256];
    static bool ready = false;
    if (!ready)
    {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(unsigned char)chars[i]] = (signed char)i;
        ready = true;
    }
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (; *s; ++s)
    {
        if (*s == '=') break;
        const int v = table[(unsigned char)*s];
        if (v < 0) return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((unsigned char)((acc >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// The icons are loaded by Nexus in the background when the add-on starts (PreloadIcons). Nexus calls IconReceived
// when a texture is ready; GetIconTexture then just hands it out. A header shows its text until its icon is there.
static std::vector<unsigned char> g_iconBytes[ICON_COUNT]; // Nexus may keep reading the memory, so it stays alive
static std::atomic<void*>         g_iconResource[ICON_COUNT];
static bool                       g_iconRequested = false;

static void IconIdentifier(int icon, char* id, size_t size) { snprintf(id, size, "UPU_ICON_%d", icon); }

static void IconReceived(const char* id, Texture_t* tex)
{
    int icon = -1;
    if (!id || strncmp(id, "UPU_ICON_", 9) != 0) return;
    icon = atoi(id + 9);
    if (icon < 0 || icon >= ICON_COUNT) return;
    if (tex && tex->Resource) g_iconResource[icon].store(tex->Resource);
}

static void PreloadIcons()
{
    if (!APIDefs || g_iconRequested) return;
    g_iconRequested = true;
    for (int i = 0; i < ICON_COUNT; ++i)
    {
        if (g_iconBytes[i].empty() && !Base64Decode(ICON_B64[i], g_iconBytes[i])) continue;
        char id[32];
        IconIdentifier(i, id, sizeof(id));
        if (APIDefs->Textures_LoadFromMemory)
            APIDefs->Textures_LoadFromMemory(id, g_iconBytes[i].data(), (uint64_t)g_iconBytes[i].size(), IconReceived);
    }
}


// --- Quick Access icon (person in a magnifier); color of the Nexus icon (#f6f6ee), see-through at rest, opaque while hovered ---
static const char* const QA_NORMAL_B64 = "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";
static const char* const QA_HOVER_B64 = "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";
static const char* const QA_ID = "QA_UNOFFICIALPLAYERUTILITY";
static const char* const QA_TEX = "UPU_QA_ICON";
static const char* const QA_TEX_HOVER = "UPU_QA_ICON_HOVER";
static std::vector<unsigned char> g_qaBytes[2]; // Nexus may keep reading the memory, so it stays alive
static bool                       g_qaTexturesRequested = false;
static bool                       g_qaShown = false;

static void QaReceived(const char*, Texture_t*) {}

static void ApplyQuickAccess()
{
    if (!APIDefs || !APIDefs->QuickAccess_Add || !APIDefs->QuickAccess_Remove) return;
    if (g_quickAccess && !g_qaShown)
    {
        if (!g_qaTexturesRequested && APIDefs->Textures_LoadFromMemory)
        {
            if (Base64Decode(QA_NORMAL_B64, g_qaBytes[0]) && Base64Decode(QA_HOVER_B64, g_qaBytes[1]))
            {
                APIDefs->Textures_LoadFromMemory(QA_TEX, g_qaBytes[0].data(), (uint64_t)g_qaBytes[0].size(), QaReceived);
                APIDefs->Textures_LoadFromMemory(QA_TEX_HOVER, g_qaBytes[1].data(), (uint64_t)g_qaBytes[1].size(), QaReceived);
                g_qaTexturesRequested = true;
            }
        }
        APIDefs->QuickAccess_Add(QA_ID, QA_TEX, QA_TEX_HOVER, "KB_UNOFFICIALPLAYERUTILITY_TOGGLE", "Unofficial PlayerUtility");
        g_qaShown = true;
        DebugLog(SRC_ADDON, DST_ADDON, "Quick Access Icon Added");
    }
    else if (!g_quickAccess && g_qaShown)
    {
        APIDefs->QuickAccess_Remove(QA_ID);
        g_qaShown = false;
        DebugLog(SRC_ADDON, DST_ADDON, "Quick Access Icon Removed");
    }
}

// Texture of an icon; false while it is not ready (the caller then shows the text instead).
static bool GetIconTexture(int icon, ImTextureID& out)
{
    if (icon < 0 || icon >= ICON_COUNT) return false;
    void* res = g_iconResource[icon].load();
    if (!res && APIDefs)
    {
        char id[32];
        IconIdentifier(icon, id, sizeof(id));
        // already known to Nexus (e.g. loaded by an earlier version of the add-on)? cheap lookup
        if (APIDefs->Textures_Get)
        {
            Texture_t* t = APIDefs->Textures_Get(id);
            if (t && t->Resource) { g_iconResource[icon].store(t->Resource); res = t->Resource; }
        }
        // still nothing after 5 seconds: ask once more, this time waiting for the result
        static double lastTry[ICON_COUNT] = {};
        const double now = ImGui::GetTime();
        if (!res && APIDefs->Textures_GetOrCreateFromMemory && !g_iconBytes[icon].empty() && now - lastTry[icon] > 5.0)
        {
            lastTry[icon] = now;
            Texture_t* t = APIDefs->Textures_GetOrCreateFromMemory(id, g_iconBytes[icon].data(), (uint64_t)g_iconBytes[icon].size());
            if (t && t->Resource) { g_iconResource[icon].store(t->Resource); res = t->Resource; }
        }
    }
    if (!res) return false;
    out = (ImTextureID)res;
    return true;
}

static int RaidIconFor(int boss) { return g_showIcons ? RAID_BOSS_ICON[boss] : -1; }


// Raw amounts per boss, so the "tokens per coffer" setting works without a new lookup
struct KpBosses
{
    int tokens[BOSS_COUNT] = {};
    int coffers[BOSS_COUNT] = {};
    int strikeN[STRIKE_COUNT] = {}; // strike proofs, normal mode
    int strikeCm[STRIKE_COUNT] = {}; // strike proofs, challenge mode
};

static int BossIndexByToken(long long id)
{
    if (id == 0) return -1;
    for (int i = 0; i < BOSS_COUNT; ++i)
        if (RAID_BOSSES[i].token == id || RAID_BOSSES[i].token2 == id) return i;
    return -1;
}
static int BossIndexByCoffer(long long id)
{
    if (id == 0) return -1;
    for (int i = 0; i < BOSS_COUNT; ++i)
        if (RAID_BOSSES[i].coffer1 == id || RAID_BOSSES[i].coffer2 == id) return i;
    return -1;
}
static int ClampAmount(double v)
{
    if (!(v > 0.0)) return 0; // also catches NaN
    if (v > 100000000.0) return 100000000;
    return (int)v;
}
static void AddOrSet(int& slot, bool add, int amount)
{
    if (add)
    {
        const long long sum = (long long)slot + amount;
        slot = sum > 100000000LL ? 100000000 : (int)sum;
    }
    else slot = amount;
}
static void ApplyStrikeItem(KpBosses& b, long long id, int amount)
{
    if (id == 0) return;
    for (int i = 0; i < STRIKE_COUNT; ++i)
    {
        const StrikeDef& d = STRIKES[i];
        if (d.n1 == id || d.n2 == id) AddOrSet(b.strikeN[i], d.n2 != 0, amount);
        if (d.c1 != 0 && (d.c1 == id || d.c2 == id)) AddOrSet(b.strikeCm[i], d.c2 != 0, amount);
    }
}
// tokens plus coffers, one coffer counts as g_cofferValue tokens
static long long BossValue(const KpBosses& b, int boss)
{
    return (long long)b.tokens[boss] + (long long)b.coffers[boss] * g_cofferValue;
}

// LI and UFE of a linked account, taken from the same answer
struct KpLinked
{
    std::string name;
    std::string kpid;                          // the KillProof id of this account
    KpStatus    status = KpStatus::Loaded; // Loaded, or Private when the account shares nothing
    int         li = 0;
    int         ufe = 0;
    KpBosses    bosses;
};

struct KpData
{
    KpStatus    status = KpStatus::NotLoaded;
    int         li = 0;   // Legendary Insight (including Legendary Divination)
    int         ufe = 0;   // Unstable Fractal Essence
    std::string error;        // only set when status == Error
    std::string kpid;         // the KillProof id (code of the profile page)
    std::vector<KpLinked> linked; // the player's other accounts (not this one)
    KpBosses    bosses;           // raid boss tokens and coffers
};

// Reads LI and UFE from one account object (its "tokens" and "killproofs" lists).
// Returns false when the account shares no data (both lists are null).
static bool ReadAccountItems(const JsonValue& obj, int& li, int& ufe, KpBosses& bosses)
{
    const JsonValue* tokens = obj.Get("tokens");
    const JsonValue* kps = obj.Get("killproofs");
    const bool haveTokens = tokens && tokens->IsArray();
    const bool haveKps = kps && kps->IsArray();
    if (!haveTokens && !haveKps) return false;

    li = 0;
    ufe = 0;
    bosses = KpBosses();
    for (const JsonValue* list : { tokens, kps })
    {
        if (!list || !list->IsArray()) continue;
        for (const JsonValue& item : list->items)
        {
            long long id = 0;
            const JsonValue* amount = item.Get("amount");
            if (!ReadItemId(item, id) || !amount || !amount->IsNumber()) continue;

            if (id == ITEM_LEGENDARY_INSIGHT || id == ITEM_LEGENDARY_DIVINATION) li += (int)amount->number;
            else if (id == ITEM_UNSTABLE_FRACTAL)                                ufe += (int)amount->number;

            const int b = BossIndexByToken(id);
            if (b >= 0)
            {
                const int amt = ClampAmount(amount->number);
                if (RAID_BOSSES[b].token2 != 0) // two ids for the same boss: add them up
                {
                    long long sum = (long long)bosses.tokens[b] + amt;
                    bosses.tokens[b] = sum > 100000000LL ? 100000000 : (int)sum;
                }
                else bosses.tokens[b] = amt;
            }
            ApplyStrikeItem(bosses, id, ClampAmount(amount->number));
        }
    }

    // coffers (same list shape); bosses with two coffer ids add them up
    const JsonValue* coffers = obj.Get("coffers");
    if (coffers && coffers->IsArray())
    {
        for (const JsonValue& item : coffers->items)
        {
            long long id = 0;
            const JsonValue* amount = item.Get("amount");
            if (!ReadItemId(item, id) || !amount || !amount->IsNumber()) continue;
            const int b = BossIndexByCoffer(id);
            if (b < 0) continue;
            int amt = ClampAmount(amount->number);
            if (RAID_BOSSES[b].coffer2 != 0)
            {
                long long sum = (long long)bosses.coffers[b] + amt;
                bosses.coffers[b] = sum > 100000000LL ? 100000000 : (int)sum;
            }
            else bosses.coffers[b] = amt;
        }
    }
    return true;
}

// Reads a whole killproof.me answer into "out" (LI, UFE and the linked accounts)
static KpStatus ParseKpAnswer(const std::string& body, KpData& out)
{
    JsonValue root;
    if (!JsonParser(body).Parse(root) || root.type != JsonValue::Type::Object)
    {
        out.error = "unreadable answer";
        return KpStatus::Error;
    }

    if (!ReadAccountItems(root, out.li, out.ufe, out.bosses)) return KpStatus::Private; // no data shared

    if (const JsonValue* k = root.Get("kpid"))
        if (k->IsString()) out.kpid = k->text;

    out.linked.clear();
    std::string mainName;
    if (const JsonValue* n = root.Get("account_name"))
        if (n->IsString()) mainName = n->text;

    const JsonValue* linked = root.Get("linked");
    if (linked && linked->IsArray())
    {
        for (const JsonValue& item : linked->items)
        {
            const JsonValue* name = item.Get("account_name");
            if (!name || !name->IsString() || name->text.empty() || name->text == mainName) continue;

            KpLinked l;
            l.name = name->text;
            if (const JsonValue* k = item.Get("kpid"))
                if (k->IsString()) l.kpid = k->text;
            if (!ReadAccountItems(item, l.li, l.ufe, l.bosses)) l.status = KpStatus::Private;
            out.linked.push_back(l);
        }
    }
    return KpStatus::Loaded;
}


// ---------------------------------------------------------------------------
// Wingman (gw2wingman.nevermindcreations.de): number of successful logs per boss.
// /api/kp?account=Name.1234 answers with {"kp": {"<boss id>": {"<profession>": n, "total": n}}}.
// The normal mode has the positive boss id, the challenge mode the same id with a minus sign.
// ---------------------------------------------------------------------------
static const long long WM_RAID_ID[BOSS_COUNT] = {
    15438, 15429, 15375,          // VG, Gors, Sab
    16123, 16115,                 // Sloth, Matt
    16253, 16235, 16246,          // Escort ("Siege the Stronghold"), KC, Xera
    17194, 17172, 17188, 17154,   // Cairn, MO, Sam, Dei
    19767, 19828, 19691, 19450,   // SH, River, Statues (Broken King), Dhuum
    43974, 21105, 20934,          // CA, Twin Largos, Qadim
    22006, 21964, 22000,          // Adina, Sabir, QtP
    26725, 26774, 26712,          // Greer, Decima, Ura
    -26725, -26774, -26712,       // Greer Cm, Decima Cm, Ura Cm
    -16235, -17194, -17172, -17188, -17154,   // KC, Cairn, MO, Sam, Deimos Cm
    -19767, -19450,                           // SH, Dhuum Cm
    -43974, -21105, -20934,                   // CA, Twin Largos, Qadim Cm
    -22006, -21964, -22000                    // Adina, Sabir, QtP Cm
};
static const long long WM_STRIKE_ID[STRIKE_COUNT] = {
    24033, 23957, 24485, 24375, 25413, 25705, 25989, 27124, 28106, 22521 // AH, XJJ, KO, HT, OLC, CO, ToF, Kela, NoE, BS
};

// Reads a Wingman answer. The number of logs of a boss goes into "tokens" (coffers stay 0).
static KpStatus ParseWmAnswer(const std::string& body, KpData& out)
{
    JsonValue root;
    if (!JsonParser(body).Parse(root) || root.type != JsonValue::Type::Object)
    {
        out.error = "unreadable answer";
        return KpStatus::Error;
    }
    const JsonValue* kp = root.Get("kp");
    if (!kp || kp->type != JsonValue::Type::Object)
    {
        out.error = "no data in the answer";
        return KpStatus::Error;
    }

    out.bosses = KpBosses();
    for (size_t i = 0; i < kp->keys.size(); ++i)
    {
        char* endp = nullptr;
        const long long id = std::strtoll(kp->keys[i].c_str(), &endp, 10);
        if (id == 0 || !endp || *endp != '\0') continue;           // also skips entries like "--25989"
        const JsonValue* total = kp->values[i].Get("total");
        if (!total || !total->IsNumber()) continue;
        const int n = ClampAmount(total->number);

        for (int b = 0; b < BOSS_COUNT; ++b)
            if (WM_RAID_ID[b] == id) out.bosses.tokens[b] = n;
        for (int k = 0; k < STRIKE_COUNT; ++k)
        {
            if (WM_STRIKE_ID[k] == id)                         out.bosses.strikeN[k] = n;
            if (StrikeHasCm(k) && -WM_STRIKE_ID[k] == id)      out.bosses.strikeCm[k] = n;
        }
    }
    return KpStatus::Loaded;
}

// ---------------------------------------------------------------------------
// killproof.me lookup
// One lookup per account, done by a background thread. Lookups are only started
// while the group has KP_MAX_GROUP players or fewer.
// ---------------------------------------------------------------------------
static constexpr size_t KP_MAX_GROUP = 10;

// An entry in g_kp means "this account was already looked up (or is being looked up)".
// It stays for the whole session, so every account is asked for only once.
static std::map<std::string, KpData> g_kp;
struct KpJob { std::string account; bool wingman; std::string key; };
static std::map<std::string, KpData> g_wm;       // Wingman answers (same rules: one lookup per account and session)
static std::deque<KpJob>             g_kpQueue;  // lookups waiting (killproof.me and Wingman share one thread)
static std::atomic<bool>             g_wmWanted(false); // the window is shown and the source is Wingman
static std::mutex                    g_kpMutex;  // protects g_kp, g_wm, g_kpQueue and g_kpStop
static std::condition_variable       g_kpCv;
static bool                          g_kpStop = false;
static std::thread                   g_kpThread;

static std::string UrlEncode(const std::string& text)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// closes a WinHTTP handle automatically
struct WinHttpHandle
{
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
};

// Plain HTTPS GET with Windows' own WinHTTP. Returns false and fills "error" if the
// request could not be made; otherwise statusCode and body are set.
static bool HttpGet(const wchar_t* host, const std::wstring& path, int& statusCode,
    std::string& body, std::string& error)
{
    static constexpr size_t MAX_BODY = 2 * 1024 * 1024; // refuse absurdly large answers

    WinHttpHandle session, connection, request;

    wchar_t userAgent[64];
    swprintf(userAgent, 64, L"UnofficialPlayerUtility/%d.%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD, VERSION_REVISION);
    session.h = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")"; return false; }

    // resolve, connect, send, receive (milliseconds): a stuck server cannot block us for long
    WinHttpSetTimeouts(session.h, 4000, 4000, 4000, 6000);

    connection.h = WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection.h) { error = "connect failed (" + std::to_string(GetLastError()) + ")"; return false; }

    request.h = WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.h) { error = "request failed (" + std::to_string(GetLastError()) + ")"; return false; }

    const wchar_t* headers = L"Accept: application/json\r\n";
    if (!WinHttpSendRequest(request.h, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        error = "send failed (" + std::to_string(GetLastError()) + ")"; return false;
    }

    if (!WinHttpReceiveResponse(request.h, nullptr))
    {
        error = "no answer (" + std::to_string(GetLastError()) + ")"; return false;
    }

    DWORD code = 0;
    DWORD codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX))
    {
        error = "no status code (" + std::to_string(GetLastError()) + ")"; return false;
    }
    statusCode = (int)code;

    body.clear();
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available))
        {
            error = "read failed (" + std::to_string(GetLastError()) + ")"; return false;
        }
        if (available == 0) break;
        if (body.size() + available > MAX_BODY) { error = "answer too large"; return false; }

        size_t old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, &body[old], available, &read))
        {
            error = "read failed (" + std::to_string(GetLastError()) + ")"; return false;
        }
        body.resize(old + read);
        if (read == 0) break;
    }
    return true;
}

// One lookup (runs in the background thread, no locks held while waiting for the network)
static KpData FetchKp(const std::string& account, int* httpCode = nullptr)
{
    KpData d;

    const std::string encoded = UrlEncode(account);
    const std::wstring path = L"/api/kp/" + std::wstring(encoded.begin(), encoded.end()) + L"?lang=en";

    int code = 0;
    std::string body, error;
    const bool ok = HttpGet(L"killproof.me", path, code, body, error);
    if (httpCode) *httpCode = code;
    if (!ok)
    {
        d.status = KpStatus::Error;
        d.error = error;
        return d;
    }

    if (code == 404) { d.status = KpStatus::NoProfile; return d; } // no account or private
    if (code != 200)
    {
        d.status = KpStatus::Error;
        d.error = "HTTP " + std::to_string(code);
        return d;
    }

    d.status = ParseKpAnswer(body, d);
    return d;
}

static KpData FetchWm(const std::string& account, int* httpCode = nullptr)
{
    KpData d;
    const std::string encoded = UrlEncode(account);
    const std::wstring path = L"/api/kp?account=" + std::wstring(encoded.begin(), encoded.end());

    int code = 0;
    std::string body, error;
    const bool ok = HttpGet(L"gw2wingman.nevermindcreations.de", path, code, body, error);
    if (httpCode) *httpCode = code;
    if (!ok) { d.status = KpStatus::Error; d.error = error; return d; }
    if (code == 404) { d.status = KpStatus::Error; d.error = "no Wingman data (HTTP 404)"; return d; }
    if (code != 200) { d.status = KpStatus::Error; d.error = "HTTP " + std::to_string(code); return d; }
    d.status = ParseWmAnswer(body, d);
    return d;
}

// Remembers the data of the linked accounts that came with an answer, so none of them
// ever needs a lookup of its own. Call with g_kpMutex held.
static void StoreLinkedAccounts(const std::string& account, const KpData& result)
{
    if (result.status != KpStatus::Loaded || result.linked.empty()) return;

    // everybody who belongs together: this account and its linked accounts
    std::vector<KpLinked> family;
    KpLinked first;
    first.name = account;
    first.kpid = result.kpid;
    first.li = result.li;
    first.ufe = result.ufe;
    first.bosses = result.bosses;
    family.push_back(first);
    for (const KpLinked& l : result.linked) family.push_back(l);

    for (const KpLinked& l : result.linked)
    {
        if (g_kp.count(l.name)) continue; // already known (or being looked up right now)

        KpData d;
        d.status = l.status;
        d.kpid = l.kpid;
        d.li = l.li;
        d.ufe = l.ufe;
        d.bosses = l.bosses;
        for (const KpLinked& f : family)
            if (f.name != l.name) d.linked.push_back(f);
        g_kp[l.name] = d;
    }
}

static const char* KpStatusName(KpStatus status);

static void RequestWm(const std::string& account);

static void KpWorker()
{
    for (;;)
    {
        KpJob job;
        {
            std::unique_lock<std::mutex> lock(g_kpMutex);
            g_kpCv.wait(lock, [] { return g_kpStop || !g_kpQueue.empty(); });
            if (g_kpStop) return;
            job = g_kpQueue.front();
            g_kpQueue.pop_front();
        }
        const char* site = job.wingman ? SRC_WINGMAN : SRC_KP;

        int httpCode = 0;
        const auto began = std::chrono::steady_clock::now();
        const KpData result = job.wingman ? FetchWm(job.account, &httpCode) : FetchKp(job.account, &httpCode);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        std::string statusLabel = KpStatusName(result.status); // "no profile" -> "No Profile"
        for (size_t k = 0; k < statusLabel.size(); ++k)
            if (k == 0 || statusLabel[k - 1] == ' ') statusLabel[k] = (char)std::toupper((unsigned char)statusLabel[k]);
        if (httpCode > 0)
        {
            LogAccount(job.account);
            DebugLog(site, DST_ADDON, "Account Lookup For %s Received: %s, HTTP%d, %lldms", job.account.c_str(), statusLabel.c_str(), httpCode, ms);
        }
        else
        {
            LogAccount(job.account);
            DebugLog(site, DST_ADDON, "Account Lookup For %s Received: %s (No Answer, %s), %lldms", job.account.c_str(), statusLabel.c_str(), result.error.c_str(), ms);
        }
        if (!job.wingman && result.status == KpStatus::Loaded && !result.linked.empty())
        {
            LogAccount(job.account);
            DebugLog(SRC_KP, DST_ADDON, "Account Lookup For %s: %d Linked Account(s) Found", job.account.c_str(), (int)result.linked.size());
        }

        std::vector<std::string> followUp; // linked accounts that now need a Wingman lookup
        {
            std::unique_lock<std::mutex> lock(g_kpMutex);
            if (job.wingman) g_wm[job.account] = result;
            else
            {
                g_kp[job.account] = result;
                StoreLinkedAccounts(job.account, result);
                if (g_wmWanted && result.status == KpStatus::Loaded)
                    for (const KpLinked& l : result.linked) followUp.push_back(l.name);
            }
        }
        for (const std::string& name : followUp) RequestWm(name);

        // short pause between two requests; wakes up early when the add-on is unloaded
        std::unique_lock<std::mutex> lock(g_kpMutex);
        if (g_kpCv.wait_for(lock, std::chrono::milliseconds(300), [] { return g_kpStop; })) return;
    }
}

static void StartKpWorker()
{
    {
        std::lock_guard<std::mutex> lock(g_kpMutex);
        g_kpStop = false;
    }
    g_kpThread = std::thread(KpWorker);
}

static void StopKpWorker()
{
    {
        std::lock_guard<std::mutex> lock(g_kpMutex);
        g_kpStop = true;
    }
    g_kpCv.notify_all();
    if (g_kpThread.joinable()) g_kpThread.join(); // at most one request has to finish (a few seconds)

    std::lock_guard<std::mutex> lock(g_kpMutex);
    g_kpQueue.clear();
    g_kp.clear();
    g_wm.clear();
}

// Asks for an account, but only if it was never asked for before
static void RequestKp(const std::string& account)
{
    if (account.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_kpMutex);
        if (g_kp.count(account)) return; // one lookup per player
        g_kp[account].status = KpStatus::Loading;
        g_kpQueue.push_back({ account, false, "K:" + account });
    }
    LogAccount(account);
    DebugLog(SRC_ADDON, SRC_KP, "Account Lookup For %s Initiated", account.c_str());
    g_kpCv.notify_one();
}

// The same for Wingman (only when the source is Wingman and the window is shown)
static void RequestWm(const std::string& account)
{
    if (account.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_kpMutex);
        if (g_wm.count(account)) return; // one lookup per account and session
        g_wm[account].status = KpStatus::Loading;
        g_kpQueue.push_back({ account, true, "W:" + account });
    }
    LogAccount(account);
    DebugLog(SRC_ADDON, SRC_WINGMAN, "Account Lookup For %s Initiated", account.c_str());
    g_kpCv.notify_one();
}

// Looks up everybody in the group (and yourself) that has not been looked up yet,
// but only while the group has KP_MAX_GROUP players or fewer. Call with g_mutex held.
static void RequestWmForGroup();

static void RequestKpForGroup()
{
    if (!g_visible) return; // requests are only sent while the window is shown
    if (g_members.size() > KP_MAX_GROUP)
    {
        static size_t lastSkipped = 0;
        if (lastSkipped != g_members.size())
            DebugLog(SRC_EXTRAS, DST_ADDON, "Group Has %d Players, Lookups Skipped (Limit %d)", (int)g_members.size(), (int)KP_MAX_GROUP);
        lastSkipped = g_members.size();
        return;
    }

    for (const Member& m : g_members) RequestKp(m.Account);
    RequestKp(g_self); // yourself, also when you are not in a group
    if (g_dataSource == 1) RequestWmForGroup();
}

static KpData GetKp(const std::string& account)
{
    std::lock_guard<std::mutex> lock(g_kpMutex);
    auto it = g_kp.find(account);
    return it != g_kp.end() ? it->second : KpData();
}

// Wingman lookups for everybody in the group, yourself and the linked accounts killproof.me already told us about.
// (The linked accounts that are still unknown follow when their killproof.me answer arrives.) Call with g_mutex held.
static void RequestWmForGroup()
{
    if (!g_visible || g_dataSource != 1) return;
    if (g_members.size() > KP_MAX_GROUP) return;
    auto ask = [](const std::string& account)
        {
            RequestWm(account);
            for (const KpLinked& l : GetKp(account).linked) RequestWm(l.name);
        };
    for (const Member& m : g_members) ask(m.Account);
    ask(g_self);
}

// The numbers shown on the raid and strike tabs: the killproof.me data, but with the boss and strike numbers of
// the selected source. LI, UFE, the Kp.me ID and the linked accounts always stay from killproof.me.
static KpData GetKpView(const std::string& account)
{
    KpData kp = GetKp(account);
    if (g_dataSource != 1) return kp;

    std::lock_guard<std::mutex> lock(g_kpMutex);
    auto wmOf = [&](const std::string& name, KpStatus& status, KpBosses& bosses, std::string* error)
        {
            auto it = g_wm.find(name);
            if (it == g_wm.end()) { status = KpStatus::NotLoaded; bosses = KpBosses(); return; }
            status = it->second.status;
            bosses = it->second.bosses;
            if (error) *error = it->second.error;
        };
    wmOf(account, kp.status, kp.bosses, &kp.error);
    for (KpLinked& l : kp.linked) wmOf(l.name, l.status, l.bosses, nullptr);
    return kp;
}

// This account's numbers plus those of all its linked accounts that share data
static void KpCombined(const KpData& d, int& li, int& ufe)
{
    li = d.li;
    ufe = d.ufe;
    for (const KpLinked& l : d.linked)
    {
        if (l.status != KpStatus::Loaded) continue;
        li += l.li;
        ufe += l.ufe;
    }
}

// Value of one boss for this account plus all its linked accounts that share data
static long long KpCombinedBoss(const KpData& d, int boss)
{
    long long v = BossValue(d.bosses, boss);
    for (const KpLinked& l : d.linked)
        if (l.status == KpStatus::Loaded) v += BossValue(l.bosses, boss);
    return v;
}

// What a strike cell shows. kind 0 = collapsed (the Cm or the Nm value, see the option; strikes without Cm
// show their only value), 1 = Nm, 2 = Cm.
static long long StrikeRaw(const KpBosses& b, int i, int kind)
{
    if (kind == 1) return b.strikeN[i];
    if (kind == 2) return b.strikeCm[i];
    return (StrikeHasCm(i) && g_strikeCollapsed == 0) ? b.strikeCm[i] : b.strikeN[i];
}
static long long KpCombinedStrike(const KpData& d, int i, int kind)
{
    long long v = StrikeRaw(d.bosses, i, kind);
    for (const KpLinked& l : d.linked)
        if (l.status == KpStatus::Loaded) v += StrikeRaw(l.bosses, i, kind);
    return v;
}

// A KillProof id is a short code of letters and digits. The text comes from a web answer,
// so it is checked before it is used to build an address.
static bool IsSafeKpCode(const std::string& code)
{
    if (code.empty() || code.size() > 32) return false;
    for (unsigned char c : code)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// Opens https://killproof.me/proof/<code> in the user's default browser
static void OpenKillproofProfile(const std::string& code)
{
    if (!IsSafeKpCode(code)) return;
    const std::wstring url = L"https://killproof.me/proof/" + std::wstring(code.begin(), code.end());
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static const char* KpStatusName(KpStatus status)
{
    switch (status)
    {
    case KpStatus::Loading:   return "loading";
    case KpStatus::Loaded:    return "loaded";
    case KpStatus::NoProfile: return "no profile";
    case KpStatus::Private:   return "private";
    case KpStatus::Error:     return "error";
    default:                  return "not looked up";
    }
}

// Text for the tooltip of an LI / UFE cell that has no number (empty = nothing to explain)
static std::string KpStatusText(const KpData& d)
{
    switch (d.status)
    {
    case KpStatus::Loaded:    return "";
    case KpStatus::Loading:   return g_dataSource == 1 ? "Loading from Wingman ..." : "Loading from killproof.me ...";
    case KpStatus::NoProfile: return "No killproof.me profile for this account";
    case KpStatus::Private:   return "The killproof.me profile is private";
    case KpStatus::Error:     return "Lookup failed: " + d.error;
    default:                  return "Not looked up (more than 10 players in the group)";
    }
}

// Text tooltip that respects the "Show Tooltips" option
static void PuTooltip(const char* fmt, ...)
{
    if (!g_tooltips) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

// Same for the tooltips of the values in the LI, UFE, Raid and Strikes cells ("Show Data Tooltips")
static void PuDataTooltip(const char* fmt, ...)
{
    if (!g_dataTooltips) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

// First line of a tooltip (boss / strike name, "Combined Value"). There is no bold font in Nexus:
// bold text is drawn twice, 1 pixel apart (like the header text).
static void TooltipTitle(const char* text)
{
    if (!g_tooltipBold) { ImGui::TextUnformatted(text); return; }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImU32  col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, col, text);
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y), col, text);
    ImGui::Dummy(ImVec2(ImGui::CalcTextSize(text).x + 1.0f, ImGui::GetTextLineHeight()));
}

// A tooltip that only holds such a title (boss / strike header cells; part of the data tooltips)
static void TitleTooltip(const char* text)
{
    if (!g_dataTooltips) return;
    ImGui::BeginTooltip();
    TooltipTitle(text);
    ImGui::EndTooltip();
}

// Tooltip with a title and one line per account; the values are right-aligned in one column
static void AlignedValueTooltip(const char* title, const std::vector<std::pair<std::string, std::string>>& lines)
{
    float nameW = 0.0f, valW = 0.0f;
    for (const auto& ln : lines)
    {
        const float nw = ImGui::CalcTextSize(ln.first.c_str()).x;
        const float vw = ImGui::CalcTextSize(ln.second.c_str()).x;
        if (nw > nameW) nameW = nw;
        if (vw > valW)  valW = vw;
    }
    if (!g_dataTooltips) return;
    ImGui::BeginTooltip();
    TooltipTitle(title);
    for (const auto& ln : lines)
    {
        ImGui::TextUnformatted(ln.first.c_str());
        ImGui::SameLine(nameW + 16.0f + valW - ImGui::CalcTextSize(ln.second.c_str()).x);
        ImGui::TextUnformatted(ln.second.c_str());
    }
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------
static void OnSquadUpdate(void* args)
{
    if (!args) return;
    const UeSquadUpdate* update = static_cast<const UeSquadUpdate*>(args);

    std::lock_guard<std::mutex> lock(g_mutex);

    for (uint64_t i = 0; i < update->Count; ++i)
    {
        const UeUserInfo& u = update->Users[i];

        if (!u.AccountName || !u.AccountName[0]) continue;

        std::string name = u.AccountName; // copy: pointer is not valid after the callback

        // Unofficial Extras sends names with a leading ':' (":Name.1234"), remove it
        if (!name.empty() && name[0] == ':') name.erase(0, 1);
        if (name.empty()) continue;

        // find existing entry
        size_t idx = g_members.size();
        for (size_t j = 0; j < g_members.size(); ++j)
        {
            if (g_members[j].Account == name) { idx = j; break; }
        }

        if (u.Role == UserRole::None || u.Role == UserRole::Invalid)
        {
            // We left the group ourselves -> everybody is gone
            if (!g_self.empty() && name == g_self)
            {
                g_members.clear();
                DebugLog(SRC_EXTRAS, DST_ADDON, "You Left the Squad");
                continue;
            }

            // someone else left the party/squad
            if (idx < g_members.size())
            {
                g_members.erase(g_members.begin() + idx);
                LogAccount(name);
                DebugLog(SRC_EXTRAS, DST_ADDON, "Player Left Squad: %s", name.c_str());
            }
            continue;
        }

        if (idx < g_members.size())
        {
            // existing player: update group info, keep any loaded LI/UFE
            g_members[idx].Role = u.Role;
            g_members[idx].Subgroup = u.Subgroup;
        }
        else
        {
            Member m;
            m.Account = name;
            m.Role = u.Role;
            m.Subgroup = u.Subgroup;
            g_members.push_back(m);
            LogAccount(name);
            DebugLog(SRC_EXTRAS, DST_ADDON, "Player Joined Squad: %s", name.c_str());
        }
    }

    RequestKpForGroup(); // look up new players on killproof.me
}

// Nexus raises this with our own account name
static void OnAccountName(void* args)
{
    if (!args) return;
    std::string name = static_cast<const char*>(args);
    if (!name.empty() && name[0] == ':') name.erase(0, 1);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_self = name;
    g_selfKnown = !name.empty();
    LogAccount(name);
    DebugLog(SRC_NEXUS, DST_ADDON, "Own Account Name Received: %s", name.c_str());
    RequestKpForGroup(); // look yourself up as soon as the name is known
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static std::string ToLower(std::string text); // defined further down

// Draws a colored rectangle behind every place where the search text occurs in "text".
// "pos" is the screen position where the text starts.
static void DrawSearchHighlight(const char* text, ImVec2 pos, float lineHeight)
{
    if (g_searchLower.empty() || !text[0]) return;

    const std::string lower = ToLower(text);
    const ImU32 color = ImGui::GetColorU32(ImVec4(g_highlight[0], g_highlight[1], g_highlight[2], g_highlight[3]));
    ImDrawList* draw = ImGui::GetWindowDrawList();

    size_t at = 0;
    while ((at = lower.find(g_searchLower, at)) != std::string::npos)
    {
        float x0 = pos.x + ImGui::CalcTextSize(text, text + at).x;
        float x1 = pos.x + ImGui::CalcTextSize(text, text + at + g_searchLower.size()).x;
        draw->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + lineHeight), color);
        at += g_searchLower.size();
    }
}

// Draws text left / centered / right inside the current table cell
static void AlignedText(const char* text, int align, bool bold = false, bool highlight = false)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float width = ImGui::CalcTextSize(text).x;

    float offset = 0.0f;
    if (align == 1)      offset = (avail - width) * 0.5f;
    else if (align == 2) offset = avail - width;

    if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

    if (highlight) // color behind the text that matches the search
        DrawSearchHighlight(text, ImGui::GetCursorScreenPos(), ImGui::GetTextLineHeight());

    if (bold)
    {
        // There is no bold font in Nexus, so the text is drawn twice, 1 pixel apart
        ImVec2      pos = ImGui::GetCursorScreenPos();
        ImU32       col = ImGui::GetColorU32(ImGuiCol_Text);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(pos, col, text);
        draw->AddText(ImVec2(pos.x + 1.0f, pos.y), col, text);
        ImGui::Dummy(ImVec2(width + 1.0f, ImGui::GetTextLineHeight()));
    }
    else
    {
        ImGui::TextUnformatted(text);
    }
}

// Picture in a table cell, aligned like text (0 left, 1 center, 2 right)
static void AlignedImage(ImTextureID tex, float size, int align)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    float offset = 0.0f;
    if (align == 1)      offset = (avail - size) * 0.5f;
    else if (align == 2) offset = avail - size;
    if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    ImGui::Image(tex, ImVec2(size, size));
}

static ImVec4 ToVec4(const ColorSetting& s) { return ImVec4(s.c[0], s.c[1], s.c[2], s.c[3]); }

// Pushes the background colors of an input box (normal, hovered, active).
// Returns how many colors were pushed, so the caller can pop them again.
static int PushInputBackground(const ColorSetting& s)
{
    ImVec4 bg = ToVec4(s);
    ImVec4 lighter(ClampF(bg.x + 0.08f, 0.0f, 1.0f), ClampF(bg.y + 0.08f, 0.0f, 1.0f),
        ClampF(bg.z + 0.08f, 0.0f, 1.0f), bg.w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, lighter);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, lighter);
    return 3;
}

// Moves the cursor down so content of the given height sits in the middle of a row
// that is rowHeight pixels tall (0 = automatic row height, nothing to do)
static void CenterVertically(float rowHeight, float contentHeight)
{
    if (rowHeight <= 0.0f) return;
    float offset = (rowHeight - contentHeight) * 0.5f - ImGui::GetStyle().CellPadding.y;
    if (offset > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
}

// Text that was copied automatically to a linked account's note. While the player keeps typing,
// the copy follows the text. A note that the user wrote himself is never touched.
static std::map<std::string, std::string, AccountLess> g_autoCopied;

// Writes a comment to the notes of all linked accounts of "account", but only to those
// that have no note yet (or only hold the copy made while typing). An existing note is never overwritten.
static void CopyNoteToLinked(const std::string& account, const NoteEntry& source)
{
    if (source.Text.empty()) return; // deleting the main note leaves the linked accounts as they are
    const KpData kp = GetKp(account);
    for (const KpLinked& l : kp.linked)
    {
        NoteEntry& target = g_notes[l.name];
        auto ac = g_autoCopied.find(l.name);
        const bool ownCopy = ac != g_autoCopied.end() && ac->second == target.Text;
        if (!target.Text.empty() && !ownCopy) continue;

        target.Text = source.Text;
        target.Time = source.Time; // same "added" date as the original
        g_autoCopied[l.name] = source.Text;
    }
}

// Debug log: what became of the linked accounts of a note that was just edited
static void LogLinkedNoteState(const std::string& account)
{
    const KpData kp = GetKp(account);
    const NoteEntry& src = g_notes[account];
    int same = 0, own = 0;
    for (const KpLinked& l : kp.linked)
    {
        const NoteEntry& t = g_notes[l.name];
        if (t.Text == src.Text) ++same; else if (!t.Text.empty()) ++own;
    }
    LogAccount(account);
    DebugLog(SRC_ADDON, DST_NOTES, "Note Edited: %d Linked Accounts Known, %d Have This Note, %d Keep Their Own Note (Copy Setting %s)",
        (int)kp.linked.size(), same, own, g_copyToLinked ? "On" : "Off");
}

// Which players have their linked accounts expanded. Players not in this list follow the default.
static std::map<std::string, bool> g_expandedState;

static bool IsExpanded(const std::string& account)
{
    auto it = g_expandedState.find(account);
    return it != g_expandedState.end() ? it->second : g_linkedExpanded;
}

static const char* CLIP_BUTTON_LABEL = "Kp.me to Clipboard";
static float ClipButtonWidth()
{
    return ImGui::CalcTextSize(CLIP_BUTTON_LABEL).x + 2.0f * ImGui::GetStyle().FramePadding.x;
}

// ---- threshold highlight ----
static bool ThresholdActive(int kind) { return g_thrKind == kind && g_thrValue[kind] >= 0; }

static bool IsCombinedRow(const Member& m, const KpData& kp)
{
    return !m.IsLinked && m.LinkedCount > 0 && !IsExpanded(m.Account) && kp.status == KpStatus::Loaded;
}

// LI / UFE cell (col = COL_LI or COL_UFE) is below the threshold
static bool LiUfeBelow(const Member& m, int col)
{
    const int kind = col == COL_LI ? THR_LI : THR_UFE;
    if (!ThresholdActive(kind)) return false;
    const KpData kp = GetKp(m.Account);
    if (kp.status != KpStatus::Loaded) return false; // "-" and "..." are never highlighted
    int li = kp.li, ufe = kp.ufe;
    if (IsCombinedRow(m, kp)) KpCombined(kp, li, ufe);
    return (col == COL_LI ? li : ufe) < g_thrValue[kind];
}

// raid boss cell is below the threshold (the Raid tab shows the Nm bosses, the Raid Cm tab the Cm bosses)
static bool BossBelow(const Member& m, int boss)
{
    const int kind = g_dataSource == 1 ? THR_LOGS : THR_KP;
    if (!ThresholdActive(kind)) return false;
    const KpData kp = GetKpView(m.Account);
    if (kp.status != KpStatus::Loaded) return false;
    const long long v = IsCombinedRow(m, kp) ? KpCombinedBoss(kp, boss) : BossValue(kp.bosses, boss);
    return v < g_thrValue[kind];
}

// strike cell is below the threshold (cellKind: 1 = Nm cell on the Strikes tab, 2 = Cm cell on the Strikes Cm tab)
static bool StrikeBelow(const Member& m, int strike, int cellKind)
{
    const int kind = g_dataSource == 1 ? THR_LOGS : THR_KP;
    if (!ThresholdActive(kind)) return false;
    if (cellKind == 2 && !StrikeHasCm(strike)) return false;
    const KpData kp = GetKpView(m.Account);
    if (kp.status != KpStatus::Loaded) return false;
    const long long v = IsCombinedRow(m, kp) ? KpCombinedStrike(kp, strike, cellKind) : StrikeRaw(kp.bosses, strike, cellKind);
    return v < g_thrValue[kind];
}

// Marks the current cell. Returns 1 when a text color was pushed (caller pops it after drawing the text).
static int BeginThresholdCell(bool below)
{
    if (!below) return 0;
    if (g_thrStyle == 1)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(g_thrTextColor[0], g_thrTextColor[1], g_thrTextColor[2], g_thrTextColor[3]));
        return 1;
    }
    // cell background (also in "entire row" mode, so you see which cell caused it)
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
        ImGui::GetColorU32(ImVec4(g_thrCellColor[0], g_thrCellColor[1], g_thrCellColor[2], g_thrCellColor[3])));
    return 0;
}

// Call right after TableNextRow: tints the whole row in "entire row" mode
static void ThresholdRowTint(bool anyBelow)
{
    if (!anyBelow || g_thrStyle != 2) return;
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
        ImGui::GetColorU32(ImVec4(g_thrRowColor[0], g_thrRowColor[1], g_thrRowColor[2], g_thrRowColor[3])));
}


// --- click highlight: click a cell and its row and column are highlighted until you click elsewhere ---
static std::string g_hlRow;        // account of the clicked row ("" = nothing highlighted)
static int         g_hlCol = -1;   // table column index of the clicked cell
static int         g_hlLastTab = -1;
static bool        g_hlHandled = false; // a button (arrow, Kp.me ID, wing header) took the click this frame
static bool        g_hlSet = false;     // a cell took the click this frame
static bool        g_hlCell = false;    // the cell that is being drawn is highlighted

static void ClearClickHighlight() { g_hlRow.clear(); g_hlCol = -1; }
static bool ClickHighlightTab() { return g_clickHighlight && g_activeTab >= 0 && g_activeTab <= 4; } // not in Comments / Debug
static ImVec4 ClickHighlightColor()
{
    return g_clickHlColor.custom ? ToVec4(g_clickHlColor) : ImGui::GetStyleColorVec4(ImGuiCol_TableHeaderBg);
}
// Marks a click on a button inside a cell: it neither sets nor clears the highlight.
static void ClickHighlightIgnoreClick() { if (ImGui::IsItemClicked(0)) g_hlHandled = true; }

struct CellRect { ImVec2 mn, mx; };

// Call right after TableSetColumnIndex, before the cell is drawn
static CellRect HighlightBegin(const Member& m, int columnIndex)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x + 2.0f * st.CellPadding.x;
    CellRect r;
    r.mn = ImVec2(pos.x - st.CellPadding.x, pos.y - st.CellPadding.y);
    r.mx = ImVec2(r.mn.x + w, r.mn.y + 0.0f); // the height is added in HighlightEnd
    g_hlCell = false;
    if (ClickHighlightTab() && !g_hlRow.empty() && (SameAccount(m.Account, g_hlRow) || columnIndex == g_hlCol))
    {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ClickHighlightColor()));
        g_hlCell = true;
    }
    return r;
}

// Call after the cell is drawn: a left click inside the cell sets (or, on the same cell, clears) the highlight
static void HighlightEnd(const Member& m, int columnIndex, CellRect r, float rowHeight, bool lastColumn = false)
{
    g_hlCell = false;
    // the highlighted row also covers the empty space to the right of the last column (the window can be wider than the table)
    if (lastColumn && ClickHighlightTab() && !g_hlRow.empty() && SameAccount(m.Account, g_hlRow))
    {
        float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
        if (ImGui::GetScrollMaxY() > 0.0f) right -= ImGui::GetStyle().ScrollbarSize;
        if (right > r.mx.x)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2(r.mx.x, r.mn.y), ImVec2(right, r.mn.y + rowHeight), false);
            dl->AddRectFilled(ImVec2(r.mx.x, r.mn.y), ImVec2(right, r.mn.y + rowHeight), ImGui::GetColorU32(ClickHighlightColor()));
            dl->PopClipRect();
        }
    }
    if (!ClickHighlightTab() || g_hlHandled || !ImGui::IsMouseClicked(0) || !ImGui::IsWindowHovered()) return;
    r.mx.y = r.mn.y + rowHeight;
    if (!ImGui::IsMouseHoveringRect(r.mn, r.mx, false)) return;
    g_hlSet = true;
    if (SameAccount(m.Account, g_hlRow) && columnIndex == g_hlCol) ClearClickHighlight();
    else { g_hlRow = m.Account; g_hlCol = columnIndex; }
}

// "Add" in the Comments tab: an extra row at the top where the account name is typed. Nothing is stored
// until the name is confirmed with Enter; then the row becomes a normal "Add Note" row for that account.
static bool g_newRowActive = false; // the row for a new account is shown
static char g_newName[64] = "";    // text in its account box
static bool g_newFocus = false; // put the cursor into the account box
static bool g_newWasActive = false; // the box was in use in the last frame
static bool g_newBad = false; // Enter was pressed with a name that is not valid (red box)

// "Name.1234": a name without a dot, then a dot and four digits. Spaces around it are removed.
static bool NormalizeAccountName(const char* in, std::string& out)
{
    std::string t = in;
    const size_t a = t.find_first_not_of(" \t");
    const size_t b = t.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    t = t.substr(a, b - a + 1);
    if (t.size() < 6) return false;
    const size_t dot = t.size() - 5;
    if (t[dot] != '.') return false;
    for (size_t k = dot + 1; k < t.size(); ++k)
        if (t[k] < '0' || t[k] > '9') return false;
    for (size_t k = 0; k < dot; ++k)
    {
        const unsigned char c = (unsigned char)t[k];
        if (c < 0x20 || c == '.') return false; // no tab / line break (the notes file uses tabs), one dot only
    }
    out = t;
    return true;
}

// The cells of the row for a new account (Account is empty)
static void DrawNewAccountCell(int col, float rowHeight)
{
    if (col == COL_DATE)
    {
        CenterVertically(rowHeight, ImGui::GetTextLineHeight());
        AlignedText("-", g_align[COL_DATE]);
        return;
    }
    if (col != COL_ACCOUNT)
    {
        CenterVertically(rowHeight, ImGui::GetTextLineHeight());
        ImGui::TextDisabled("Enter the account name and press Enter");
        return;
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    float padY = (rowHeight - 2.0f * st.CellPadding.y - ImGui::GetFontSize()) * 0.5f;
    if (padY < 0.0f) padY = 0.0f;
    if (padY > st.FramePadding.y) padY = st.FramePadding.y;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, padY));
    CenterVertically(rowHeight, ImGui::GetFrameHeight());

    const bool redBox = g_newBad;
    if (redBox) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.10f, 0.10f, 0.85f));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (g_newFocus)
    {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetScrollY(0.0f); // the new row is the first row
        g_newFocus = false;
    }
    const bool enter = ImGui::InputTextWithHint("##newaccount", "Name.1234", g_newName, sizeof(g_newName),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemEdited()) g_newBad = false;
    if (ImGui::IsItemHovered()) PuTooltip("Account name like Name.1234, then press Enter");
    if (redBox) ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (enter)
    {
        std::string name;
        if (!NormalizeAccountName(g_newName, name))
        {
            g_newBad = true;   // red box, stay in the field
            g_newFocus = true;
        }
        else
        {
            const auto it = g_notes.find(name);
            const bool hasNote = it != g_notes.end() && !it->second.Text.empty();
            g_newRowActive = false;
            g_newName[0] = '\0';
            g_newBad = false;
            g_newWasActive = false;
            RequestNoteEdit(name, !hasNote); // existing note: show it; new account: cursor in its note field
        }
        return;
    }

    // left the box without a name: the empty row disappears again
    if (g_newWasActive && !active && !g_newFocus && g_newName[0] == '\0') g_newRowActive = false;
    g_newWasActive = active;
}

// Draws one cell of a player row. "col" is a column id (COL_ACCOUNT, COL_GROUP, ...)
static void DrawCell(int col, const Member& m, float rowHeight)
{
    char buf[16];

    if (m.Account.empty()) { DrawNewAccountCell(col, rowHeight); return; } // the row for a new account (Comments tab)

    if (col != COL_NOTES) // the note field is centered below, it has its own height
        CenterVertically(rowHeight, ImGui::GetTextLineHeight());

    switch (col)
    {
    case COL_ACCOUNT:
        if (m.IsLinked)
        {
            // linked accounts are indented below their main account
            ImGui::Indent(20.0f);
            AlignedText(m.Account.c_str(), g_align[COL_ACCOUNT], false, true);
            ImGui::Unindent(20.0f);
        }
        else if (m.LinkedCount > 0)
        {
            // arrow next to the account: shows or hides its linked accounts
            const bool expanded = IsExpanded(m.Account);

            // standard ImGui arrow button; no inner padding, so it is as high as the text
            // and cannot make the row taller; transparent until you hover over it
            ImGui::PushID(m.Account.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            if (ImGui::ArrowButton("##expand", expanded ? ImGuiDir_Down : ImGuiDir_Right))
                g_expandedState[m.Account] = !expanded;
            ClickHighlightIgnoreClick();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                if (expanded) PuTooltip("Hide Linked Accounts");
                else          PuTooltip("Show Linked Accounts");
            }
            ImGui::PopID();

            ImGui::SameLine(0.0f, 4.0f);
            AlignedText(m.Account.c_str(), g_align[COL_ACCOUNT], false, true);
        }
        else
        {
            AlignedText(m.Account.c_str(), g_align[COL_ACCOUNT], false, true);
        }
        break;

    case COL_KPID:
    {
        // plain text like the other cells; a click on the code opens the profile in the browser
        const KpData kp = GetKp(m.Account);
        const bool hasCode = kp.status == KpStatus::Loaded && !kp.kpid.empty();

        std::string text = "-";
        if (hasCode)                             text = kp.kpid;
        else if (kp.status == KpStatus::Loading) text = "...";

        AlignedText(text.c_str(), g_align[COL_KPID], false, true);
        if (hasCode && ImGui::IsItemHovered())
            PuTooltip("Open Player Profile");
        if (hasCode && ImGui::IsItemClicked(0))
        {
            OpenKillproofProfile(kp.kpid);
            g_hlHandled = true;
        }
        break;
    }

    case COL_GROUP:
        if (m.InGroup) snprintf(buf, sizeof(buf), "%d", (int)m.Subgroup + 1);
        else           snprintf(buf, sizeof(buf), "-");
        AlignedText(buf, g_align[COL_GROUP]);
        break;

    case COL_LI:
    case COL_UFE:
    {
        const KpData kp = GetKp(m.Account);

        // Collapsed: the row of the account in the group shows the totals of all its accounts.
        // Expanded: it shows its own numbers, the linked accounts have their own rows.
        const bool combine = !m.IsLinked && m.LinkedCount > 0 && !IsExpanded(m.Account) &&
            kp.status == KpStatus::Loaded;
        int li = kp.li, ufe = kp.ufe;
        if (combine) KpCombined(kp, li, ufe);

        if (kp.status == KpStatus::Loaded)       snprintf(buf, sizeof(buf), "%d", col == COL_LI ? li : ufe);
        else if (kp.status == KpStatus::Loading) snprintf(buf, sizeof(buf), "...");
        else                                     snprintf(buf, sizeof(buf), "-");
        const int thrPushed = BeginThresholdCell(LiUfeBelow(m, col));
        AlignedText(buf, g_align[col]);
        if (thrPushed) ImGui::PopStyleColor();

        if (ImGui::IsItemHovered())
        {
            if (combine)
            {
                std::vector<std::pair<std::string, std::string>> lines;
                lines.push_back({ m.Account, std::to_string(col == COL_LI ? kp.li : kp.ufe) });
                for (const KpLinked& l : kp.linked)
                {
                    if (l.status == KpStatus::Loaded) lines.push_back({ l.name, std::to_string(col == COL_LI ? l.li : l.ufe) });
                    else                              lines.push_back({ l.name, "private" });
                }
                AlignedValueTooltip("Combined Value", lines);
            }
            else
            {
                const std::string why = KpStatusText(kp);
                if (!why.empty()) PuDataTooltip("%s", why.c_str());
            }
        }
        break;
    }

    case COL_DATE:
    {
        // date and time the note was added
        const NoteEntry& e = g_notes[m.Account];
        char dateBuf[32] = "-";
        if (e.Time != 0 && !e.Text.empty())
        {
            std::tm tmv{};
            if (localtime_s(&tmv, &e.Time) == 0)
                strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M", &tmv);
        }
        AlignedText(dateBuf, g_align[COL_DATE]);
        break;
    }

    case COL_NOTES:
    {
        // editable note for this account
        NoteEntry& e = g_notes[m.Account];
        char noteBuf[NOTE_MAX_BYTES + 1];
        snprintf(noteBuf, sizeof(noteBuf), "%s", e.Text.c_str());

        const ImGuiStyle& st = ImGui::GetStyle();

        // Make the text box fit the row: shrink its inner padding if the row is low,
        // then center it vertically
        float wantFrame = rowHeight - 2.0f * st.CellPadding.y;
        float padY = (wantFrame - ImGui::GetFontSize()) * 0.5f;
        if (padY < 0.0f) padY = 0.0f;
        if (padY > st.FramePadding.y) padY = st.FramePadding.y;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, padY));
        CenterVertically(rowHeight, ImGui::GetFrameHeight());

        // Optional colors: cell background (the text box gets the same color) and text
        int colorsPushed = 0;
        if (g_noteBg.custom)
        {
            ImVec4 bg = ToVec4(g_noteBg);
            if (!g_hlCell) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(bg)); // not over the click highlight

            ImVec4 lighter(ClampF(bg.x + 0.08f, 0.0f, 1.0f), ClampF(bg.y + 0.08f, 0.0f, 1.0f),
                ClampF(bg.z + 0.08f, 0.0f, 1.0f), bg.w);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, lighter);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, lighter);
            colorsPushed += 3;
        }
        if (g_noteText.custom)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(g_noteText));
            ++colorsPushed;
        }
        if (g_hintColor.custom) // placeholder text "add a note"
        {
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ToVec4(g_hintColor));
            ++colorsPushed;
        }

        // Stretch the text box over the whole cell width, including the cell padding
        float fullWidth = ImGui::GetContentRegionAvail().x + 2.0f * st.CellPadding.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - st.CellPadding.x);

        ImGui::PushID(m.Account.c_str());
        ImGui::SetNextItemWidth(fullWidth);
        if (g_noteFocusPending && g_activeTab == 5 && SameAccount(m.Account, g_noteReqAccount))
        {
            ImGui::SetKeyboardFocusHere(); // "Add Note": the cursor is in the field
            g_noteFocusPending = false;
        }
        if (ImGui::InputTextWithHint("##note", "add a note", noteBuf, sizeof(noteBuf)))
        {
            e.Text = noteBuf;
            if (e.Text.empty())   e.Time = 0;                    // note removed
            else if (e.Time == 0) e.Time = std::time(nullptr);   // note added now (date shows right away)
            g_noteDirty = m.Account;                             // saved a moment after the last key / when the box is left
            g_noteLastEdit = ImGui::GetTime();
            g_noteDirtyCopy = g_copyToLinked && !m.IsLinked;      // linked accounts get it a moment after the last key
        }
        // Commit on any end of editing (Enter, click elsewhere, Tab), not only "deactivated after edit"
        if (ImGui::IsItemDeactivated() && SameAccount(m.Account, g_noteDirty))
        {
            if (e.Text.empty())   e.Time = 0;
            else if (e.Time == 0) e.Time = std::time(nullptr);

            // the same comment for the linked accounts that have no note yet
            if (g_copyToLinked && !m.IsLinked)
                CopyNoteToLinked(m.Account, e);

            SaveNotes();
            g_noteDirty.clear();
        }
        const bool noteActive = ImGui::IsItemActive();
        if (noteActive) g_activeNote = m.Account; // used by the comments tab
        if (noteActive && g_noteReqAdd && SameAccount(m.Account, g_noteReqAccount)) g_noteSeenActive = true;

        // highlight the search text inside the note (not while you are editing it)
        if (!noteActive && !g_searchLower.empty() && !e.Text.empty())
        {
            ImVec2 boxMin = ImGui::GetItemRectMin();
            ImVec2 boxMax = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(boxMin, boxMax, true);
            DrawSearchHighlight(e.Text.c_str(), ImVec2(boxMin.x + st.FramePadding.x, boxMin.y + padY),
                ImGui::GetTextLineHeight());
            draw->PopClipRect();
        }
        ImGui::PopID();

        ImGui::PopStyleColor(colorsPushed);
        ImGui::PopStyleVar();
        break;
    }
    }
}

static std::string ToLower(std::string text)
{
    for (char& ch : text) ch = (char)std::tolower((unsigned char)ch);
    return text;
}

// True if the row matches the search box: account name or comment text contains the search text
static bool MatchesSearch(const Member& m)
{
    if (g_searchLower.empty()) return true;
    if (IsKeptAccount(m.Account)) return true; // don't hide the row you are typing in

    if (ToLower(m.Account).find(g_searchLower) != std::string::npos) return true;

    auto it = g_notes.find(m.Account);
    if (it != g_notes.end() && ToLower(it->second.Text).find(g_searchLower) != std::string::npos)
        return true;

    // the KP ID, so a code from the chat can be pasted into the search box
    if (ToLower(GetKp(m.Account).kpid).find(g_searchLower) != std::string::npos)
        return true;

    return false;
}

// Draws a table with the given column ids (left to right) and player rows.
// tabId only has to be different for every tab.
static void DrawTable(const char* tabId, const std::vector<int>& cols, const std::vector<Member>& rows,
    int notesTotal = -1,   // >= 0: header reads "Account Note (Total: N)"
    bool keepSelf = false) // true: your own row is never hidden by the search
{
    // The table gets a new id whenever a width changes, so ImGui applies the new widths
    char tableId[48];
    snprintf(tableId, sizeof(tableId), "members_%s##%d", tabId, g_tableGen);

    // header background and table border colors (only if customized)
    int tableColors = 0;
    if (g_headerBg.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ToVec4(g_headerBg));
        ++tableColors;
    }
    if (g_tableBorder.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ToVec4(g_tableBorder));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ToVec4(g_tableBorder));
        tableColors += 2;
    }

    g_tableTopLocalY = ImGui::GetCursorPosY(); // measured for the window height
    g_extraHeight = 0.0f;                      // only the raid tab can have a horizontal scrollbar

    if (ImGui::BeginTable(tableId, (int)cols.size(),
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, -g_tableBottomReserve)))
    {
        // columns are created in the order chosen in the options (cols)
        for (int p = 0; p < (int)cols.size(); ++p)
        {
            int c = cols[p];
            if (c == COL_NOTES) ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthStretch, 1.0f); // takes the room that is left
            else                ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthFixed, g_width[c]);
        }

        // header row (drawn by hand so the header text can be aligned and the height set)
        float headerHeight = g_headerHeight.custom ? g_headerHeight.v : 0.0f;
        float textOffsetY = 0.0f;
        if (headerHeight > 0.0f)
        {
            textOffsetY = (headerHeight - ImGui::GetTextLineHeight()) * 0.5f
                - ImGui::GetStyle().CellPadding.y;
            if (textOffsetY < 0.0f) textOffsetY = 0.0f;
        }

        float headerY = 0.0f;
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
        for (int p = 0; p < (int)cols.size(); ++p)
        {
            int c = cols[p];
            ImGui::TableSetColumnIndex(p);
            if (p == 0) headerY = ImGui::GetCursorScreenPos().y;
            if (c == COL_NOTES) // where the note fields start; the search box lines up with it
                g_notesColumnScreenX = ImGui::GetCursorScreenPos().x - ImGui::GetStyle().CellPadding.x;
            if (textOffsetY > 0.0f)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            const char* name = COL_NAMES[c];
            char label[64];
            if (notesTotal >= 0 && c == COL_NOTES)
            {
                snprintf(label, sizeof(label), "%s (Total: %d)", COL_NAMES[c], notesTotal);
                name = label;
            }
            AlignedText(name, c == COL_NOTES ? 0 : g_align[c], g_headerBold);
        }

        float rowHeight = g_rowHeight.custom ? g_rowHeight.v : 0.0f;
        const float minRowHeight = ImGui::GetTextLineHeight() + 2.0f * ImGui::GetStyle().CellPadding.y;
        if (rowHeight < minRowHeight) rowHeight = minRowHeight; // so text is always centered correctly
        int   drawn = 0;
        float row0Y = 0.0f, row1Y = 0.0f;
        for (const Member& m : rows)
        {
            const bool isMe = keepSelf && !g_self.empty() && m.Account == g_self;
            if (!isMe && !m.Account.empty() && !MatchesSearch(m)) continue;

            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (!g_noteReqAccount.empty() && g_activeTab == 5 && SameAccount(m.Account, g_noteReqAccount))
            {
                if (g_noteScrollPending)
                {
                    ImGui::SetScrollHereY(0.3f);
                    g_noteScrollPending = false;
                    if (!g_noteReqAdd) g_noteFlash = 100; // Edit Note: show where the row is
                }
                if (g_noteFlash > 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 220, 0, 60));
            }
            if (g_thrStyle == 2 && (g_thrKind == THR_LI || g_thrKind == THR_UFE))
            {
                bool any = false;
                for (int c : cols)
                    if ((c == COL_LI || c == COL_UFE) && LiUfeBelow(m, c)) { any = true; break; }
                ThresholdRowTint(any);
            }
            for (int p = 0; p < (int)cols.size(); ++p)
            {
                ImGui::TableSetColumnIndex(p);
                if (p == 0)
                {
                    if (drawn == 0)      row0Y = ImGui::GetCursorScreenPos().y;
                    else if (drawn == 1) row1Y = ImGui::GetCursorScreenPos().y;
                }
                const CellRect cr = HighlightBegin(m, p);
                DrawCell(cols[p], m, rowHeight);
                HighlightEnd(m, p, cr, rowHeight, p == (int)cols.size() - 1);
            }
            ++drawn;
        }

        // remember what was measured, the window height is calculated from it
        g_lastRowCount = drawn;
        if (drawn >= 1) g_measuredHeader = row0Y - headerY;
        if (drawn >= 2) g_measuredPitch = row1Y - row0Y;

        ImGui::EndTable();
    }

    ImGui::PopStyleColor(tableColors);
}

// Column ids in the order chosen in the options. The comments tab only has three of them.
// Adds the linked accounts of every player. A player with linked accounts gets an arrow;
// when expanded, the linked accounts follow directly below as indented rows.
// An account that is in the group itself keeps its own row and is not repeated as a linked row.
static std::vector<Member> WithLinkedRows(const std::vector<Member>& base)
{
    std::set<std::string> present; // accounts that already have a row
    for (const Member& m : base) present.insert(m.Account);

    std::vector<Member> rows;
    for (const Member& m : base)
    {
        std::vector<Member> children; // linked accounts of this player without a row of their own
        const KpData kp = GetKp(m.Account);
        for (const KpLinked& l : kp.linked)
        {
            if (present.count(l.name)) continue;
            present.insert(l.name);

            Member child;
            child.Account = l.name;
            child.InGroup = false; // Group column shows "-"
            child.IsLinked = true;
            children.push_back(child);
        }

        Member parent = m;
        parent.LinkedCount = (int)children.size();
        rows.push_back(parent);

        if (IsExpanded(m.Account))
            for (const Member& child : children) rows.push_back(child);
    }
    return rows;
}

enum TabKind { TAB_GENERAL, TAB_STRIKES, TAB_COMMENTS, TAB_RAID };

static std::vector<int> ColumnsInOrder(TabKind tab)
{
    std::vector<int> cols;
    for (int p = 0; p < COL_COUNT; ++p)
    {
        int c = g_order[p];
        if (tab == TAB_COMMENTS && c != COL_ACCOUNT && c != COL_DATE && c != COL_NOTES) continue;
        if (tab == TAB_GENERAL && c == COL_GROUP) continue; // no Group column on the General tab
        if ((tab == TAB_RAID || tab == TAB_STRIKES) && c != COL_ACCOUNT && c != COL_KPID) continue; // wing / strike columns follow
        cols.push_back(c);
    }
    return cols;
}


// ---------------------------------------------------------------------------
// Raid tab: account columns, then one column per wing (last boss) or per boss (expanded wing)
// ---------------------------------------------------------------------------
struct RaidCol { int boss; int wing; };

// The Raid tab shows W1 - W8 (normal mode). The Raid Cm tab shows W8 Cm with killproof.me and, with Wingman,
// all wings that have a challenge mode (W3 Cm to W8 Cm).
static std::vector<int> RaidWingsFor(bool cm)
{
    if (!cm)                 return { 0, 1, 2, 3, 4, 5, 6, 7 };
    if (g_dataSource == 1)   return { 9, 10, 11, 12, 13, 8 };
    return { 8 };
}

static std::vector<RaidCol> RaidColumns(bool cm = false)
{
    std::vector<RaidCol> cols;
    for (int w : RaidWingsFor(cm))
    {
        const int first = RAID_WINGS[w].first;
        const int last = first + RAID_WINGS[w].count - 1;
        if (g_raidExpanded[w])
            for (int b = first; b <= last; ++b) cols.push_back({ b, w });
        else
            cols.push_back({ last, w });
    }
    return cols;
}

// "W1" to "W8"; the challenge mode wings (W3 to W8 on the Raid Cm tab) have the same labels
static void WingLabel(int wing, char* out, size_t size)
{
    if (wing == 8)       snprintf(out, size, "W8");         // the Raid Cm tab is clear enough without "Cm"
    else if (wing > 8)   snprintf(out, size, "W%d", wing - 6); // wings 9 - 13 are W3 Cm - W7 Cm
    else                 snprintf(out, size, "W%d", wing + 1);
}

// width of a collapsed wing column: the wing width setting, but never narrower than its label
static float CollapsedWingWidth(int wing)
{
    char text[16];
    WingLabel(wing, text, sizeof(text));
    const float needed = (g_showTriangles ? ImGui::GetFontSize() * 0.55f + 4.0f : 0.0f) + ImGui::CalcTextSize(text).x + 3.0f;
    return g_wingWidth > needed ? g_wingWidth : needed;
}

// "triangle W1" in the header; returns true when clicked
static bool WingHeaderLabel(int wing, bool expanded)
{
    char text[16];
    WingLabel(wing, text, sizeof(text));

    const float lineH = ImGui::GetTextLineHeight();
    const float tri = g_showTriangles ? ImGui::GetFontSize() * 0.55f : 0.0f;
    const float gap = g_showTriangles ? 4.0f : 0.0f;
    const float textW = ImGui::CalcTextSize(text).x;
    const float totalW = tri + gap + textW + 1.0f;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(wing);
    const bool clicked = ImGui::InvisibleButton("##wing", ImVec2(totalW, lineH));
    const bool hovered = ImGui::IsItemHovered();
    ClickHighlightIgnoreClick();
    ImGui::PopID();

    if (hovered)
    {
        const RaidWing& rw = RAID_WINGS[wing];
        if (!expanded) PuDataTooltip("Shows %s", RAID_BOSSES[rw.first + rw.count - 1].name);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    // the text may be wider than a narrow column, so the clip rectangle is widened
    draw->PushClipRect(pos, ImVec2(pos.x + totalW + 4.0f, pos.y + lineH), false);

    const ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Text);
    const float cy = pos.y + lineH * 0.5f;
    if (g_showTriangles)
    {
        if (expanded)
            draw->AddTriangleFilled(ImVec2(pos.x, cy - tri * 0.4f), ImVec2(pos.x + tri, cy - tri * 0.4f),
                ImVec2(pos.x + tri * 0.5f, cy + tri * 0.4f), col);
        else
            draw->AddTriangleFilled(ImVec2(pos.x, cy - tri * 0.5f), ImVec2(pos.x + tri * 0.8f, cy),
                ImVec2(pos.x, cy + tri * 0.5f), col);
    }

    // without triangles the label itself lights up on hover (it is still clickable)
    const ImU32 textCol = g_showTriangles ? ImGui::GetColorU32(ImGuiCol_Text) : col;
    const ImVec2 tp(pos.x + tri + gap, pos.y);
    draw->AddText(tp, textCol, text);
    if (g_headerBold) draw->AddText(ImVec2(tp.x + 1.0f, tp.y), textCol, text);

    draw->PopClipRect();
    return clicked;
}

// One boss cell of a player row
static void DrawBossCell(const Member& m, int boss, float rowHeight)
{
    CenterVertically(rowHeight, ImGui::GetTextLineHeight());

    const KpData kp = GetKpView(m.Account);
    const bool combine = !m.IsLinked && m.LinkedCount > 0 && !IsExpanded(m.Account) &&
        kp.status == KpStatus::Loaded;

    long long value = 0;
    bool known = kp.status == KpStatus::Loaded;
    if (known) value = combine ? KpCombinedBoss(kp, boss) : BossValue(kp.bosses, boss);

    char buf[24];
    if (kp.status == KpStatus::Loading)  snprintf(buf, sizeof(buf), "...");
    else if (known && value > 0)         snprintf(buf, sizeof(buf), "%lld", value);
    else                                 snprintf(buf, sizeof(buf), "-");

    const int thrPushed = BeginThresholdCell(BossBelow(m, boss));
    AlignedText(buf, g_bossAlign);
    if (thrPushed) ImGui::PopStyleColor();

    if (g_dataTooltips && ImGui::IsItemHovered())
    {
        if (!known)
        {
            const std::string why = KpStatusText(kp);
            if (!why.empty()) PuDataTooltip("%s", why.c_str());
        }
        else if (combine)
        {
            // one line per account, the values lined up (right-aligned) in a column
            std::vector<std::pair<std::string, std::string>> lines;
            char vb[32];
            snprintf(vb, sizeof(vb), "%lld", BossValue(kp.bosses, boss));
            lines.push_back({ m.Account, vb });
            for (const KpLinked& l : kp.linked)
            {
                if (l.status == KpStatus::Loaded)
                {
                    snprintf(vb, sizeof(vb), "%lld", BossValue(l.bosses, boss));
                    lines.push_back({ l.name, vb });
                }
                else lines.push_back({ l.name, g_dataSource == 1 ? "no data" : "private" });
            }
            float nameW = 0.0f, valW = 0.0f;
            for (const auto& ln : lines)
            {
                const float nw = ImGui::CalcTextSize(ln.first.c_str()).x;
                const float vw = ImGui::CalcTextSize(ln.second.c_str()).x;
                if (nw > nameW) nameW = nw;
                if (vw > valW)  valW = vw;
            }
            ImGui::BeginTooltip();
            TooltipTitle(RAID_BOSSES[boss].name);
            for (const auto& ln : lines)
            {
                ImGui::TextUnformatted(ln.first.c_str());
                ImGui::SameLine(nameW + 16.0f + valW - ImGui::CalcTextSize(ln.second.c_str()).x);
                ImGui::TextUnformatted(ln.second.c_str());
            }
            ImGui::EndTooltip();
        }
        else
        {
            ImGui::BeginTooltip();
            TooltipTitle(RAID_BOSSES[boss].name);
            if (g_dataSource == 1) ImGui::Text("%lld Successful Log(s)", value);
            else                   ImGui::Text("%lld Tokens", value);
            ImGui::EndTooltip();
        }
    }
}

// defined further down (width of all columns of the raid / strikes tabs)
static float RaidColumnsWidth(unsigned mask, bool cm);
static float StrikeColumnsWidth(bool cm);

static void DrawRaidTable(const std::vector<Member>& rows, bool cm = false)
{
    const std::vector<int>     fixedCols = ColumnsInOrder(TAB_RAID);
    const std::vector<RaidCol> bossCols = RaidColumns(cm);
    const int nFixed = (int)fixedCols.size();
    const int nBoss = (int)bossCols.size();
    const int tailCols[2] = { COL_DATE, COL_NOTES }; // right of the wings, always in this order
    const int nCols = nFixed + nBoss + 2;

    bool anyExpanded = false;
    unsigned mask = 0;
    for (int w : RaidWingsFor(cm))
        if (g_raidExpanded[w]) { anyExpanded = true; mask |= 1u << w; }

    // a different set of columns needs a different table id
    char tableId[64];
    snprintf(tableId, sizeof(tableId), "%s##%d_%u", cm ? "members_raidcm" : "members_raid", g_tableGen, mask);

    int tableColors = 0;
    if (g_headerBg.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ToVec4(g_headerBg));
        ++tableColors;
    }
    if (g_tableBorder.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ToVec4(g_tableBorder));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ToVec4(g_tableBorder));
        tableColors += 2;
    }

    g_tableTopLocalY = ImGui::GetCursorPosY();
    g_notesColumnScreenX = -1.0f; // no notes column here, the search box goes to the right edge

    // does everything fit? if not, a horizontal scrollbar appears and the window needs more height
    const ImGuiStyle& st = ImGui::GetStyle();
    float totalWidth = 0.0f;
    const float colExtra = 2.0f * st.CellPadding.x + 1.0f;
    for (int c : fixedCols) totalWidth += g_width[c] + colExtra;
    for (const RaidCol& rc : bossCols) totalWidth += (g_raidExpanded[rc.wing] ? g_bossWidth : CollapsedWingWidth(rc.wing)) + colExtra;
    for (int c : tailCols) totalWidth += g_width[c] + colExtra;
    g_extraHeight = totalWidth + 1.0f > ImGui::GetContentRegionAvail().x ? st.ScrollbarSize : 0.0f;

    int toggleWing = -1;

    // inner width: all columns at their set width, or the whole window if that is wider (the Account Note column takes the rest)
    float innerWidth = RaidColumnsWidth(mask, cm);
    static bool s_raidVScroll = false; // last frame: did the table have a vertical scrollbar? (only then reserve its width)
    const float reserveR = s_raidVScroll ? st.ScrollbarSize : 0.0f;
    if (ImGui::GetContentRegionAvail().x - reserveR > innerWidth) innerWidth = ImGui::GetContentRegionAvail().x - reserveR;
    if (ImGui::BeginTable(tableId, nCols,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, 0.0f), innerWidth))
    {
        s_raidVScroll = ImGui::GetScrollMaxY() > 0.0f;
        for (int c : fixedCols)
            ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthFixed, g_width[c]);
        for (const RaidCol& rc : bossCols)
            ImGui::TableSetupColumn(RAID_BOSSES[rc.boss].abbr, ImGuiTableColumnFlags_WidthFixed,
                g_raidExpanded[rc.wing] ? g_bossWidth : CollapsedWingWidth(rc.wing));
        for (int c : tailCols)
        {
            if (c == COL_NOTES) ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthStretch, 1.0f); // takes the room that is left
            else                ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthFixed, g_width[c]);
        }
        ImGui::TableSetupScrollFreeze(nFixed, anyExpanded ? 2 : 1);

        float headerHeight = g_headerHeight.custom ? g_headerHeight.v : 0.0f;
        float textOffsetY = 0.0f;
        if (headerHeight > 0.0f)
        {
            textOffsetY = (headerHeight - ImGui::GetTextLineHeight()) * 0.5f - st.CellPadding.y;
            if (textOffsetY < 0.0f) textOffsetY = 0.0f;
        }

        // header row 1: fixed column names and the wing labels
        float headerY = 0.0f;
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
        for (int p = 0; p < nFixed; ++p)
        {
            ImGui::TableSetColumnIndex(p);
            if (p == 0) headerY = ImGui::GetCursorScreenPos().y;
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            AlignedText(COL_NAMES[fixedCols[p]], g_align[fixedCols[p]], g_headerBold);
        }
        for (int k = 0; k < (int)bossCols.size(); ++k)
        {
            if (k > 0 && bossCols[k - 1].wing == bossCols[k].wing) continue; // label only in the first column of a wing
            ImGui::TableSetColumnIndex(nFixed + k);
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            const int w = bossCols[k].wing;
            if (WingHeaderLabel(w, g_raidExpanded[w])) toggleWing = w;
        }
        for (int t = 0; t < 2; ++t)
        {
            const int c = tailCols[t];
            ImGui::TableSetColumnIndex(nFixed + nBoss + t);
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            AlignedText(COL_NAMES[c], c == COL_NOTES ? 0 : g_align[c], g_headerBold);
        }

        // header row 2: boss names, only above expanded wings
        if (anyExpanded)
        {
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
            for (int k = 0; k < (int)bossCols.size(); ++k)
            {
                if (!g_raidExpanded[bossCols[k].wing]) continue;
                ImGui::TableSetColumnIndex(nFixed + k);
                if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
                ImTextureID iconTex;
                if (GetIconTexture(RaidIconFor(bossCols[k].boss), iconTex))
                    AlignedImage(iconTex, g_iconSize, g_bossAlign);
                else
                    AlignedText(RAID_BOSSES[bossCols[k].boss].abbr, g_bossAlign, g_headerBold);
                if (ImGui::IsItemHovered()) TitleTooltip(RAID_BOSSES[bossCols[k].boss].name);
            }
        }

        // player rows
        float rowHeight = g_rowHeight.custom ? g_rowHeight.v : 0.0f;
        const float minRowHeight = ImGui::GetTextLineHeight() + 2.0f * st.CellPadding.y;
        if (rowHeight < minRowHeight) rowHeight = minRowHeight;

        int   drawn = 0;
        float row0Y = 0.0f, row1Y = 0.0f;
        for (const Member& m : rows)
        {
            const bool isMe = !g_self.empty() && m.Account == g_self;
            if (!isMe && !MatchesSearch(m)) continue;

            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (g_thrStyle == 2 && (g_thrKind == THR_KP || g_thrKind == THR_LOGS))
            {
                bool any = false;
                for (const RaidCol& rc : bossCols)
                    if (BossBelow(m, rc.boss)) { any = true; break; }
                ThresholdRowTint(any);
            }
            for (int p = 0; p < nFixed; ++p)
            {
                ImGui::TableSetColumnIndex(p);
                if (p == 0)
                {
                    if (drawn == 0)      row0Y = ImGui::GetCursorScreenPos().y;
                    else if (drawn == 1) row1Y = ImGui::GetCursorScreenPos().y;
                }
                const CellRect cr = HighlightBegin(m, p);
                DrawCell(fixedCols[p], m, rowHeight);
                HighlightEnd(m, p, cr, rowHeight);
            }
            for (int k = 0; k < (int)bossCols.size(); ++k)
            {
                ImGui::TableSetColumnIndex(nFixed + k);
                const CellRect cr = HighlightBegin(m, nFixed + k);
                ImGui::PushID(k);
                DrawBossCell(m, bossCols[k].boss, rowHeight);
                ImGui::PopID();
                HighlightEnd(m, nFixed + k, cr, rowHeight);
            }
            for (int t = 0; t < 2; ++t)
            {
                ImGui::TableSetColumnIndex(nFixed + nBoss + t);
                const CellRect cr = HighlightBegin(m, nFixed + nBoss + t);
                DrawCell(tailCols[t], m, rowHeight);
                HighlightEnd(m, nFixed + nBoss + t, cr, rowHeight, t == 1);
            }
            ++drawn;
        }

        g_lastRowCount = drawn;
        if (drawn >= 1) g_measuredHeader = row0Y - headerY;
        if (drawn >= 2) g_measuredPitch = row1Y - row0Y;

        ImGui::EndTable();
    }

    ImGui::PopStyleColor(tableColors);

    if (toggleWing >= 0)
    {
        g_raidExpanded[toggleWing] = !g_raidExpanded[toggleWing];
    }
}


// ---------------------------------------------------------------------------
// Strikes tab: one Nm column per strike. Strikes Cm tab: one Cm column per strike that has a challenge mode.
// ---------------------------------------------------------------------------
struct StrikeCol { int strike; int kind; }; // kind: 1 normal, 2 Cm

static std::vector<StrikeCol> StrikeColumns(bool cm = false)
{
    std::vector<StrikeCol> cols;
    for (int i = 0; i < STRIKE_COUNT; ++i)
    {
        if (cm && !StrikeHasCm(i)) continue;
        cols.push_back({ i, cm ? 2 : 1 });
    }
    return cols;
}

static float StrikeColumnWidth(int i)
{
    const float needed = ImGui::CalcTextSize(STRIKES[i].abbr).x + 3.0f;
    return g_wingWidth > needed ? g_wingWidth : needed;
}

// strike shortcut in the header (full name as tooltip)
static void StrikeHeaderLabel(int strike)
{
    ImGui::TextUnformatted(STRIKES[strike].abbr);
    if (g_headerBold)
    {
        const ImVec2 mn = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 1.0f, mn.y), ImGui::GetColorU32(ImGuiCol_Text), STRIKES[strike].abbr);
    }
    if (ImGui::IsItemHovered()) TitleTooltip(STRIKES[strike].name);
}

static void DrawStrikeCell(const Member& m, int strike, int kind, float rowHeight)
{
    CenterVertically(rowHeight, ImGui::GetTextLineHeight());

    const KpData kp = GetKpView(m.Account);
    const bool combine = IsCombinedRow(m, kp);
    const bool known = kp.status == KpStatus::Loaded;
    long long value = 0;
    if (known) value = combine ? KpCombinedStrike(kp, strike, kind) : StrikeRaw(kp.bosses, strike, kind);

    char buf[24];
    if (kp.status == KpStatus::Loading)  snprintf(buf, sizeof(buf), "...");
    else if (known && value > 0)         snprintf(buf, sizeof(buf), "%lld", value);
    else                                 snprintf(buf, sizeof(buf), "-");

    const int thrPushed = BeginThresholdCell(StrikeBelow(m, strike, kind));
    AlignedText(buf, g_bossAlign);
    if (thrPushed) ImGui::PopStyleColor();

    if (g_dataTooltips && ImGui::IsItemHovered())
    {
        if (!known)
        {
            const std::string why = KpStatusText(kp);
            if (!why.empty()) PuDataTooltip("%s", why.c_str());
        }
        else if (combine)
        {
            // one line per account, the values lined up (right-aligned) in one column; only the values of this tab (Nm or Cm)
            std::vector<std::pair<std::string, std::string>> lines;
            lines.push_back({ m.Account, std::to_string(StrikeRaw(kp.bosses, strike, kind)) });
            for (const KpLinked& l : kp.linked)
            {
                if (l.status == KpStatus::Loaded) lines.push_back({ l.name, std::to_string(StrikeRaw(l.bosses, strike, kind)) });
                else                              lines.push_back({ l.name, g_dataSource == 1 ? "no data" : "private" });
            }
            AlignedValueTooltip(STRIKES[strike].name, lines);
        }
        else
        {
            ImGui::BeginTooltip();
            TooltipTitle(STRIKES[strike].name);
            if (g_dataSource == 1) ImGui::Text("%lld Successful Log(s)", value);
            else                   ImGui::Text("%lld Tokens", value);
            ImGui::EndTooltip();
        }
    }
}

static void DrawStrikeTable(const std::vector<Member>& rows, bool cm = false)
{
    const std::vector<int>       fixedCols = ColumnsInOrder(TAB_STRIKES);
    const std::vector<StrikeCol> cols = StrikeColumns(cm);
    const int nFixed = (int)fixedCols.size();
    const int nStr = (int)cols.size();
    const int tailCols[2] = { COL_DATE, COL_NOTES };
    const int nCols = nFixed + nStr + 2;

    char tableId[64];
    snprintf(tableId, sizeof(tableId), "%s##%d", cm ? "members_strikescm" : "members_strikes", g_tableGen);

    int tableColors = 0;
    if (g_headerBg.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ToVec4(g_headerBg));
        ++tableColors;
    }
    if (g_tableBorder.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ToVec4(g_tableBorder));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ToVec4(g_tableBorder));
        tableColors += 2;
    }

    g_tableTopLocalY = ImGui::GetCursorPosY();
    g_notesColumnScreenX = -1.0f;

    const ImGuiStyle& st = ImGui::GetStyle();
    const float colExtra = 2.0f * st.CellPadding.x + 1.0f;
    float totalWidth = 0.0f;
    for (int c : fixedCols) totalWidth += g_width[c] + colExtra;
    for (const StrikeCol& sc : cols) totalWidth += StrikeColumnWidth(sc.strike) + colExtra;
    for (int c : tailCols) totalWidth += g_width[c] + colExtra;
    g_extraHeight = totalWidth + 1.0f > ImGui::GetContentRegionAvail().x ? st.ScrollbarSize : 0.0f;

    float innerWidth = StrikeColumnsWidth(cm); // see the raid table
    static bool s_strikeVScroll = false; // see the raid table
    const float reserveR = s_strikeVScroll ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    if (ImGui::GetContentRegionAvail().x - reserveR > innerWidth) innerWidth = ImGui::GetContentRegionAvail().x - reserveR;
    if (ImGui::BeginTable(tableId, nCols,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, 0.0f), innerWidth))
    {
        s_strikeVScroll = ImGui::GetScrollMaxY() > 0.0f;
        for (int c : fixedCols)
            ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthFixed, g_width[c]);
        for (const StrikeCol& sc : cols)
            ImGui::TableSetupColumn(STRIKES[sc.strike].abbr, ImGuiTableColumnFlags_WidthFixed,
                StrikeColumnWidth(sc.strike));
        for (int c : tailCols)
        {
            if (c == COL_NOTES) ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthStretch, 1.0f); // takes the room that is left
            else                ImGui::TableSetupColumn(COL_NAMES[c], ImGuiTableColumnFlags_WidthFixed, g_width[c]);
        }
        ImGui::TableSetupScrollFreeze(nFixed, 1);

        float headerHeight = g_headerHeight.custom ? g_headerHeight.v : 0.0f;
        float textOffsetY = 0.0f;
        if (headerHeight > 0.0f)
        {
            textOffsetY = (headerHeight - ImGui::GetTextLineHeight()) * 0.5f - st.CellPadding.y;
            if (textOffsetY < 0.0f) textOffsetY = 0.0f;
        }

        // header row: fixed column names and the strike shortcuts
        float headerY = 0.0f;
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
        for (int p = 0; p < nFixed; ++p)
        {
            ImGui::TableSetColumnIndex(p);
            if (p == 0) headerY = ImGui::GetCursorScreenPos().y;
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            AlignedText(COL_NAMES[fixedCols[p]], g_align[fixedCols[p]], g_headerBold);
        }
        for (int k = 0; k < nStr; ++k)
        {
            ImGui::TableSetColumnIndex(nFixed + k);
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            StrikeHeaderLabel(cols[k].strike);
        }
        for (int t = 0; t < 2; ++t)
        {
            const int c = tailCols[t];
            ImGui::TableSetColumnIndex(nFixed + nStr + t);
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            AlignedText(COL_NAMES[c], c == COL_NOTES ? 0 : g_align[c], g_headerBold);
        }

        float rowHeight = g_rowHeight.custom ? g_rowHeight.v : 0.0f;
        const float minRowHeight = ImGui::GetTextLineHeight() + 2.0f * st.CellPadding.y;
        if (rowHeight < minRowHeight) rowHeight = minRowHeight;

        int   drawn = 0;
        float row0Y = 0.0f, row1Y = 0.0f;
        for (const Member& m : rows)
        {
            const bool isMe = !g_self.empty() && m.Account == g_self;
            if (!isMe && !MatchesSearch(m)) continue;

            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (g_thrStyle == 2 && (g_thrKind == THR_KP || g_thrKind == THR_LOGS))
            {
                bool any = false;
                for (const StrikeCol& sc : cols)
                    if (StrikeBelow(m, sc.strike, sc.kind)) { any = true; break; }
                ThresholdRowTint(any);
            }
            for (int p = 0; p < nFixed; ++p)
            {
                ImGui::TableSetColumnIndex(p);
                if (p == 0)
                {
                    if (drawn == 0)      row0Y = ImGui::GetCursorScreenPos().y;
                    else if (drawn == 1) row1Y = ImGui::GetCursorScreenPos().y;
                }
                const CellRect cr = HighlightBegin(m, p);
                DrawCell(fixedCols[p], m, rowHeight);
                HighlightEnd(m, p, cr, rowHeight);
            }
            for (int k = 0; k < nStr; ++k)
            {
                ImGui::TableSetColumnIndex(nFixed + k);
                const CellRect cr = HighlightBegin(m, nFixed + k);
                ImGui::PushID(k);
                DrawStrikeCell(m, cols[k].strike, cols[k].kind, rowHeight);
                ImGui::PopID();
                HighlightEnd(m, nFixed + k, cr, rowHeight);
            }
            for (int t = 0; t < 2; ++t)
            {
                ImGui::TableSetColumnIndex(nFixed + nStr + t);
                const CellRect cr = HighlightBegin(m, nFixed + nStr + t);
                DrawCell(tailCols[t], m, rowHeight);
                HighlightEnd(m, nFixed + nStr + t, cr, rowHeight, t == 1);
            }
            ++drawn;
        }

        g_lastRowCount = drawn;
        if (drawn >= 1) g_measuredHeader = row0Y - headerY;
        if (drawn >= 2) g_measuredPitch = row1Y - row0Y;

        ImGui::EndTable();
    }

    ImGui::PopStyleColor(tableColors);
}

// ---------------------------------------------------------------------------
// Window width that fits the columns of a tab
// ---------------------------------------------------------------------------
// every column also needs its cell padding on both sides and one pixel for the border line
static float ColumnExtra()
{
    return 2.0f * ImGui::GetStyle().CellPadding.x + 1.0f;
}

static float SumWidths(const std::vector<int>& cols)
{
    float sum = 0.0f;
    for (int c : cols) sum += g_width[c] + ColumnExtra();
    return sum;
}

// width of all columns of the raid tab; bit w of "mask" = wing w shows all its bosses
static float RaidColumnsWidth(unsigned mask, bool cm = false)
{
    float sum = SumWidths(ColumnsInOrder(TAB_RAID));
    sum += g_width[COL_DATE] + g_width[COL_NOTES] + 2.0f * ColumnExtra(); // Note Date and Account Note
    for (int w : RaidWingsFor(cm))
        sum += (mask & (1u << w)) ? (g_bossWidth + ColumnExtra()) * RAID_WINGS[w].count
        : CollapsedWingWidth(w) + ColumnExtra();
    return sum;
}

// width of all columns of the strikes tabs (the Cm tab only has the strikes with a challenge mode)
static float StrikeColumnsWidth(bool cm)
{
    float sum = SumWidths(ColumnsInOrder(TAB_STRIKES));
    sum += g_width[COL_DATE] + g_width[COL_NOTES] + 2.0f * ColumnExtra();
    for (const StrikeCol& sc : StrikeColumns(cm)) sum += StrikeColumnWidth(sc.strike) + ColumnExtra();
    return sum;
}

// width of the filter row of the Debug tab (the window is made just wide enough: 10 px to spare)
static float DebugRowWidth()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float sp = st.ItemSpacing.x;
    return ImGui::CalcTextSize("Source").x + sp + 150.0f + sp + ImGui::CalcTextSize("Destination").x + sp + 150.0f + sp
        + ImGui::CalcTextSize("Event").x + sp + 130.0f + sp
        + ImGui::CalcTextSize("Reset Filters").x + 2.0f * st.FramePadding.x + sp + ImGui::CalcTextSize("500 of 500 entries").x
        + 8.0f; // + window padding and borders (added later) = 10 px of room
}

static float TabColumnsWidth(int tab, unsigned mask)
{
    switch (tab)
    {
    case 1:  return RaidColumnsWidth(mask, false);
    case 2:  return RaidColumnsWidth(mask, true);
    case 3:  return StrikeColumnsWidth(false);
    case 4:  return StrikeColumnsWidth(true);
    case 5:  return SumWidths(ColumnsInOrder(TAB_COMMENTS));
    case 6:  return DebugRowWidth(); // Debug tab
    default: return SumWidths(ColumnsInOrder(TAB_GENERAL));
    }
}

// width of the top row (threshold, search box, clipboard button), so it is never cut off
static constexpr float SOURCE_COMBO_WIDTH = 90.0f;
static constexpr float SEARCH_ROOM = 150.0f; // space kept free in the top row for the search box that slides out

static float TopRowWidth()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    // the search box slides out to the left of its arrow button: room for it is reserved
    return ImGui::GetFrameHeight() + st.ItemSpacing.x + 10.0f + SOURCE_COMBO_WIDTH + st.ItemSpacing.x + 135.0f + st.ItemSpacing.x + 64.0f
        + st.ItemSpacing.x + ImGui::GetFrameHeight() // clear button of the threshold field
        + 30.0f + SEARCH_ROOM + ImGui::GetFrameHeight() + st.ItemSpacing.x + ClipButtonWidth() + 2.0f * st.WindowPadding.x + 2.0f;
}

// Title bar: "Unofficial PlayerUtility", plus a yellow remark while the own account name is unknown
static const char* const WINDOW_TITLE = "Unofficial PlayerUtility";
static const char* const TITLE_REMARK = "Please Restart the Game to Complete the Installation";
static const float       TITLE_GAP = 14.0f; // space between the add-on name and the remark

static float TitleBarWidth()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const std::string full = std::string(WINDOW_TITLE) + TITLE_REMARK;
    return ImGui::CalcTextSize(full.c_str()).x + TITLE_GAP + 2.0f * st.FramePadding.x + ImGui::GetFontSize() + st.ItemInnerSpacing.x + 12.0f;
}

// window width = columns + window padding + borders (+ the vertical scrollbar when there are many rows)
static float WindowWidthFor(float columnsWidth)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    float w = columnsWidth + 2.0f * st.WindowPadding.x + 2.0f;
    if (w < TopRowWidth()) w = TopRowWidth();
    if (!g_selfKnown && w < TitleBarWidth()) w = TitleBarWidth(); // room for the remark in the title bar
    if (g_lastRowCount > g_maxRows) w += st.ScrollbarSize;
    return w;
}


// Tab "Debug": the log (time, source, event). Only shown while "Show debug info" is ticked.
// filters of the Debug tab (not saved; "" = All)
static std::string g_logFilterSource;
static std::string g_logFilterDest;
static char        g_logFilterText[64] = "";

static void DrawDebugTab()
{
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight())); // the space where the description used to be
    if (ImGui::Button("Clear")) ClearDebugLog();
    ImGui::SameLine();
    if (ImGui::Button("Open Folder") && APIDefs)
    {
        const fs::path dir = APIDefs->Paths_GetAddonDirectory("UnofficialPlayerUtility");
        ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f)); // space under the buttons

    static size_t lastCount = 0; // entries shown in the table (previous frame)

    // sources and destinations that occur in the log (for the drop-downs)
    std::set<std::string> sources, dests;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        total = g_logEntries.size();
        for (const LogEntry& e : g_logEntries) { sources.insert(e.source); dests.insert(e.dest); }
    }
    // a value that is no longer in the log cannot stay selected
    if (!g_logFilterSource.empty() && !sources.count(g_logFilterSource)) g_logFilterSource.clear();
    if (!g_logFilterDest.empty() && !dests.count(g_logFilterDest))     g_logFilterDest.clear();

    auto filterCombo = [](const char* id, const char* label, std::string& value, const std::set<std::string>& values)
        {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo(id, value.empty() ? "All" : value.c_str()))
            {
                if (ImGui::Selectable("All", value.empty())) value.clear();
                for (const std::string& v : values)
                    if (ImGui::Selectable(v.c_str(), v == value)) value = v;
                ImGui::EndCombo();
            }
        };
    filterCombo("##logsrc", "Source", g_logFilterSource, sources);
    ImGui::SameLine();
    filterCombo("##logdst", "Destination", g_logFilterDest, dests);
    ImGui::SameLine();
    ImGui::TextUnformatted("Event");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputText("##logtext", g_logFilterText, sizeof(g_logFilterText));
    ImGui::SameLine();
    if (ImGui::Button("Reset Filters"))
    {
        g_logFilterSource.clear();
        g_logFilterDest.clear();
        g_logFilterText[0] = '\0';
    }
    ImGui::SameLine();
    ImGui::Text("%d of %d entries", (int)lastCount, (int)total);

    const std::string textFilter = ToLower(g_logFilterText);
    auto matches = [&](const LogEntry& e)
        {
            if (!g_logFilterSource.empty() && e.source != g_logFilterSource) return false;
            if (!g_logFilterDest.empty() && e.dest != g_logFilterDest)   return false;
            if (!textFilter.empty() && ToLower(e.text).find(textFilter) == std::string::npos) return false;
            return true;
        };

    const ImGuiStyle& st = ImGui::GetStyle();
    g_tableTopLocalY = ImGui::GetCursorPosY(); // measured for the window height
    g_extraHeight = 0.0f;
    g_measuredHeader = -1.0f;
    g_measuredPitch = -1.0f;

    // the same colors and sizes as the other tabs
    int tableColors = 0;
    if (g_headerBg.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ToVec4(g_headerBg));
        ++tableColors;
    }
    if (g_tableBorder.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ToVec4(g_tableBorder));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ToVec4(g_tableBorder));
        tableColors += 2;
    }

    if (ImGui::BeginTable("debuglog", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupScrollFreeze(0, 1);

        const float headerHeight = g_headerHeight.custom ? g_headerHeight.v : 0.0f;
        float textOffsetY = 0.0f;
        if (headerHeight > 0.0f)
        {
            textOffsetY = (headerHeight - ImGui::GetTextLineHeight()) * 0.5f - st.CellPadding.y;
            if (textOffsetY < 0.0f) textOffsetY = 0.0f;
        }
        float rowHeight = g_rowHeight.custom ? g_rowHeight.v : 0.0f;
        const float minRowHeight = ImGui::GetTextLineHeight() + 2.0f * st.CellPadding.y;
        if (rowHeight < minRowHeight) rowHeight = minRowHeight;

        static const char* const names[4] = { "Time", "Source", "Event", "Destination" };
        float headerY = 0.0f, row0Y = 0.0f, row1Y = 0.0f;
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
        for (int c = 0; c < 4; ++c)
        {
            ImGui::TableSetColumnIndex(c);
            if (c == 0) headerY = ImGui::GetCursorScreenPos().y;
            if (textOffsetY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffsetY);
            AlignedText(names[c], 0, g_headerBold);
        }

        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        static std::string menuAccount; // account of the row that was right-clicked
        float rowTopY = 0.0f;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            for (const LogEntry& e : g_logEntries)
            {
                if (!matches(e)) continue;
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                const std::string* cells[4] = { &e.time, &e.source, &e.text, &e.dest };
                for (int c = 0; c < 4; ++c)
                {
                    ImGui::TableSetColumnIndex(c);
                    float cellY = 0.0f;
                    if (c == 0)
                    {
                        const float y = ImGui::GetCursorScreenPos().y;
                        cellY = y;
                        if (count == 0) row0Y = y;
                        else if (count == 1) row1Y = y;
                    }
                    CenterVertically(rowHeight, ImGui::GetTextLineHeight());
                    ImGui::TextUnformatted(cells[c]->c_str());
                    if (c == 0) rowTopY = cellY - st.CellPadding.y;
                }
                ++count;

                // right-click on a row that is about an account: menu
                if (!e.account.empty() && ImGui::IsWindowHovered())
                {
                    const ImVec2 wp = ImGui::GetWindowPos();
                    if (ImGui::IsMouseHoveringRect(ImVec2(wp.x, rowTopY), ImVec2(wp.x + ImGui::GetWindowWidth(), rowTopY + rowHeight), false)) // not clipped: the clip rect is that of the last column here
                    {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        {
                            menuAccount = e.account;
                            ImGui::OpenPopup("##logrowmenu");
                        }
                        else if (!ImGui::IsPopupOpen("##logrowmenu"))
                            PuTooltip("Right-Click for Account Note Options");
                    }
                }
            }
        }

        if (ImGui::BeginPopup("##logrowmenu"))
        {
            const auto note = g_notes.find(menuAccount);
            const bool hasNote = note != g_notes.end() && !note->second.Text.empty();
            if (ImGui::MenuItem(("Add Note for " + menuAccount).c_str(), nullptr, false, !hasNote))
                RequestNoteEdit(menuAccount, true);
            if (ImGui::MenuItem(("Edit Note for " + menuAccount).c_str(), nullptr, false, hasNote))
                RequestNoteEdit(menuAccount, false);
            if (ImGui::MenuItem("Copy to Clipboard"))
                ImGui::SetClipboardText(menuAccount.c_str());
            ImGui::EndPopup();
        }
        if (count != lastCount && atBottom) ImGui::SetScrollHereY(1.0f); // follow new entries
        lastCount = count;
        g_lastRowCount = (int)count;
        if (count >= 1) g_measuredHeader = row0Y - headerY;
        if (count >= 2) g_measuredPitch = row1Y - row0Y;
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(tableColors);
}

// Tab "Comments": every account that has a note, newest first
static void DrawCommentsTab()
{
    std::vector<Member> rows;
    for (const auto& kv : g_notes)
    {
        // keep the row you are typing in, even if you momentarily empty its text
        if (kv.second.Text.empty() && !IsKeptAccount(kv.first)) continue;
        Member m;
        m.Account = kv.first;
        rows.push_back(m);
    }

    std::sort(rows.begin(), rows.end(), [](const Member& x, const Member& y)
        {
            std::time_t tx = g_notes.at(x.Account).Time;
            std::time_t ty = g_notes.at(y.Account).Time;
            if (tx != ty) return tx > ty;      // newest first, unknown dates last
            return x.Account < y.Account;
        });

    // the row for a new account is the first row
    if (g_newRowActive) rows.insert(rows.begin(), Member());

    // number of accounts that really have a comment (not counting a row being emptied right now)
    int total = 0;
    for (const auto& kv : g_notes)
        if (!kv.second.Text.empty()) ++total;

    // the table leaves room for a bar at the bottom of the window that stays in place while the list scrolls
    const ImGuiStyle& st = ImGui::GetStyle();
    const float barHeight = ImGui::GetFrameHeight() + 2.0f * st.ItemSpacing.y + 2.0f; // gap, 2 px, gap, button
    g_tableBottomReserve = barHeight;
    DrawTable("comments", ColumnsInOrder(TAB_COMMENTS), rows, total);
    g_tableBottomReserve = 0.0f;
    g_extraHeight = barHeight; // the window is that much taller (measured like the table, used in the next frame)

    // "Add": a row where the name of an account is typed (the account does not have to be in the group)
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (ImGui::Button("Add"))
    {
        g_newRowActive = true;
        g_newFocus = true;
        g_search[0] = '\0'; // so a search does not hide the row
        g_searchLower.clear();
    }
    if (ImGui::IsItemHovered()) PuTooltip("Add a note for any account, even if it is not in your group");
}

// Threshold entry that fits the tab and the data source: UFE on General, Kp.me or Logs on the raid / strike tabs.
// It is chosen when the tab or the source changes; a different entry starts with an empty field (= off).
static void AutoSelectThreshold()
{
    const int category = g_activeTab == 0 ? 0 : (g_activeTab >= 1 && g_activeTab <= 4 ? 1 : -1);
    static int lastKey = -2;
    const int key = category < 0 ? -1 : category * 2 + g_dataSource;
    if (key == lastKey) return;
    lastKey = key;

    int kind = g_thrKind;
    if (category == 0)      kind = THR_UFE;
    else if (category == 1) kind = g_dataSource == 1 ? THR_LOGS : THR_KP;
    if (kind == g_thrKind) return;

    g_thrKind = kind;
    for (int i = 0; i < THR_COUNT; ++i) g_thrValue[i] = -1;
    SyncThresholdText();
}

static void FlushNoteEdits()
{
    // save a note a moment after the last key, in case the game never reports that the box was left
    if (!g_noteDirty.empty() && ImGui::GetTime() - g_noteLastEdit > 1.0)
    {
        if (g_noteDirtyCopy) CopyNoteToLinked(g_noteDirty, g_notes[g_noteDirty]);
        LogLinkedNoteState(g_noteDirty);
        SaveNotes();
        g_noteDirty.clear();
    }
}

static void AddonRender()
{
    FlushNoteEdits();
    g_hlHandled = false;
    g_hlSet = false;
    if (g_noteFlash > 0) --g_noteFlash;
    if (g_noteReqFrames > 0 && --g_noteReqFrames == 0)
    {
        g_noteReqAccount.clear();
        g_noteFocusPending = g_noteScrollPending = g_gotoComments = false;
    }
    g_wmWanted = g_visible && g_dataSource == 1;
    static bool lastVisible = false;
    if (g_visible != lastVisible)
    {
        DebugLog(SRC_NEXUS, DST_ADDON, "Window %s", g_visible ? "Rendered" : "Hidden");
        lastVisible = g_visible;
        g_wmWanted = g_visible && g_dataSource == 1;
        if (g_visible)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            RequestKpForGroup(); // window just opened: look up everybody who is still missing
        }
    }
    if (!g_visible) return;

    // window border: color and thickness (only if customized)
    int windowColors = 0, windowVars = 0;
    if (g_windowBorder.custom)
    {
        ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(g_windowBorder));
        ++windowColors;
    }
    if (g_windowBorderSize.custom)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, g_windowBorderSize.v);
        ++windowVars;
    }

    ImGui::SetNextWindowSize(ImVec2(880, 150), ImGuiCond_FirstUseEver);

    // Window height follows the number of rows (up to "max rows"). Only the width can be resized.
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float textMin = ImGui::GetTextLineHeight() + 2.0f * st.CellPadding.y;
        float rowH = g_rowHeight.custom ? g_rowHeight.v : 0.0f;
        float headerH = g_headerHeight.custom ? g_headerHeight.v : 0.0f;
        if (rowH < textMin)    rowH = textMin;
        if (headerH < textMin) headerH = textMin;

        // values measured last frame; estimates until the first table was drawn
        float top = g_tableTopLocalY >= 0.0f ? g_tableTopLocalY
            : ImGui::GetFrameHeight() * 3.0f + st.WindowPadding.y + st.ItemSpacing.y + st.ItemInnerSpacing.y;
        float header = g_measuredHeader > 0.0f ? g_measuredHeader : headerH;
        float pitch = g_measuredPitch > 0.0f ? g_measuredPitch : rowH + 1.0f;

        int rowsShown = g_lastRowCount;
        if (rowsShown < 1)          rowsShown = 1;
        if (rowsShown > g_maxRows)  rowsShown = g_maxRows;

        float height = top + header + rowsShown * pitch + st.WindowPadding.y + 4.0f + g_extraHeight;

        // Width follows the tab: when the tab (or the expanded wings, or a column width) changes,
        // the window is resized once so all columns fit. Afterwards you can still drag it.
        unsigned mask = 0;
        if (g_activeTab == 1 || g_activeTab == 2)
            for (int w : RaidWingsFor(g_activeTab == 2))
                if (g_raidExpanded[w]) mask |= 1u << w;
        const long long key = (long long)g_activeTab | ((long long)mask << 4) | ((long long)g_dataSource << 18) | ((long long)g_tableGen << 20);
        static bool haveKey = false;
        static long long lastKey = 0;
        // The first frames after the window is shown also fit the width to the tab (ignores a size remembered from earlier)
        static int initialFits = 2;
        if (!haveKey) { haveKey = true; lastKey = key; }
        if (key != lastKey || initialFits > 0)
        {
            lastKey = key;
            if (initialFits > 0) --initialFits;
            ImGui::SetNextWindowSize(ImVec2(WindowWidthFor(TabColumnsWidth(g_activeTab, mask)), height), ImGuiCond_Always);
        }

        // never narrower than the columns (the Account Note column has its set width as the minimum); Raid tab: all wings with their bosses hidden
        const float minWidth = WindowWidthFor(g_activeTab == 1 ? RaidColumnsWidth(0, false)
            : g_activeTab == 2 ? RaidColumnsWidth(0, true)
            : g_activeTab == 3 ? StrikeColumnsWidth(false)
            : g_activeTab == 4 ? StrikeColumnsWidth(true)
            : g_activeTab == 0 ? TabColumnsWidth(0, 0)
            : g_activeTab == 5 ? TabColumnsWidth(5, 0) : 0.0f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(minWidth, height), ImVec2(FLT_MAX, height));
    }
    if (ImGui::Begin(WINDOW_TITLE, &g_visible, ImGuiWindowFlags_NoCollapse))
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        // Own account name unknown (the add-on was installed during this game session): remark in the title bar
        if (g_self.empty())
        {
            const ImGuiStyle& ts = ImGui::GetStyle();
            const ImVec2 wp = ImGui::GetWindowPos();
            const float barH = ImGui::GetFontSize() + 2.0f * ts.FramePadding.y;
            const float padL = ts.FramePadding.x;
            const float padR = ts.FramePadding.x + ImGui::GetFontSize() + ts.ItemInnerSpacing.x; // close button
            const float avail = ImGui::GetWindowWidth() - padL - padR;
            const float titleW = ImGui::CalcTextSize(WINDOW_TITLE).x;
            const float x0 = wp.x + padL + (avail - titleW) * ts.WindowTitleAlign.x + titleW;
            const float y0 = wp.y + (barH - ImGui::GetFontSize()) * ts.WindowTitleAlign.y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(wp, ImVec2(wp.x + ImGui::GetWindowWidth() - padR, wp.y + barH), false);
            dl->AddText(ImVec2(x0 + TITLE_GAP, y0), IM_COL32(255, 220, 0, 255), TITLE_REMARK);
            dl->PopClipRect();
        }

        // remember which note was being edited last frame, then reset the marker
        g_keepAccount = g_activeNote;
        g_activeNote.clear();

        // Expand / collapse all wings (Raid and Raid Cm tab) with one click
        {
            const bool raidTab = g_activeTab == 1 || g_activeTab == 2;
            const bool usable = raidTab;
            bool anyCollapsed = false;
            if (raidTab)
                for (int w : RaidWingsFor(g_activeTab == 2)) if (!g_raidExpanded[w]) anyCollapsed = true;
            if (!usable) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
            if (ImGui::ArrowButton("##expandall", anyCollapsed ? ImGuiDir_Right : ImGuiDir_Down) && usable)
            {
                for (int w : RaidWingsFor(g_activeTab == 2)) g_raidExpanded[w] = anyCollapsed;
            }
            if (!usable) ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                PuTooltip(!usable ? "No Content to Expand"
                    : anyCollapsed ? "Expand Visible Content" : "Collapse Visible Content");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x + 10.0f); // 10px extra before the dropdown
        }

        AutoSelectThreshold();

        // Data source: Kp.me is always the start value; Wingman is only asked when it is chosen
        ImGui::SetNextItemWidth(SOURCE_COMBO_WIDTH);
        if (ImGui::Combo("##datasource", &g_dataSource, "Kp.me\0Wingman\0"))
        {
            DebugLog(SRC_ADDON, DST_NONE, "Data Source Changed To %s", g_dataSource == 1 ? "Wingman" : "Kp.me");
            g_wmWanted = g_visible && g_dataSource == 1;
            if (g_dataSource == 1) RequestWmForGroup();
        }
        if (ImGui::IsItemHovered())
            PuTooltip("Data Source Dropdown");
        ImGui::SameLine();

        // Threshold: which number is checked and the limit (empty = off)
        ImGui::SetNextItemWidth(135.0f);
        if (ImGui::Combo("##thrkind", &g_thrKind, "Threshold Kp.me\0Threshold Logs\0Threshold LI\0Threshold UFE\0"))
        {
            // a new choice starts with an empty field (= off)
            for (int i = 0; i < THR_COUNT; ++i) g_thrValue[i] = -1;
            SyncThresholdText();
            SaveSettings();
        }
        if (ImGui::IsItemHovered())
            PuTooltip("Threshold Selection Based on Data Source");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputText("##thrvalue", g_thrText[g_thrKind], 7, ImGuiInputTextFlags_CharsDecimal)) // 6 digits
            g_thrValue[g_thrKind] = g_thrText[g_thrKind][0] ? std::atoi(g_thrText[g_thrKind]) : -1;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::IsItemHovered())
            PuTooltip("Values Below Threshold are Highlighted\nRight-Click for Predefined Values");

        // right-click on the field: list of predefined values (set in the options)
        if (ImGui::BeginPopupContextItem("##thrpresets", ImGuiPopupFlags_MouseButtonRight))
        {
            if (g_thrPresets.empty()) ImGui::TextDisabled("None defined (Options > Colors)");
            for (int preset : g_thrPresets)
            {
                char label[16];
                snprintf(label, sizeof(label), "%d", preset);
                if (ImGui::Selectable(label, g_thrValue[g_thrKind] == preset))
                {
                    g_thrValue[g_thrKind] = preset;
                    SyncThresholdText();
                    SaveSettings();
                }
            }
            ImGui::EndPopup();
        }

        // delete button, only while a value is entered
        if (g_thrValue[g_thrKind] >= 0)
        {
            ImGui::SameLine();
            if (ImGui::Button("X##thrclear", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
            {
                g_thrValue[g_thrKind] = -1;
                SyncThresholdText();
                SaveSettings();
            }
            if (ImGui::IsItemHovered())
                PuTooltip("Clear Threshold");
        }

        // Right side: arrow button that slides the search box out to the left, then the "Kp.me to Clipboard" button.
        // The box stays open as long as it contains text; it closes when it is empty and loses focus (or on Escape).
        const float searchWidth = ClampF(g_width[COL_NOTES] - 50.0f, SEARCH_ROOM, 600.0f);
        const ImGuiStyle& topSt = ImGui::GetStyle();
        const float clipBtnW = ClipButtonWidth();
        const float arrowW = ImGui::GetFrameHeight();
        ImGui::SameLine(0.0f, 30.0f);
        const float rowY = ImGui::GetCursorScreenPos().y;
        const float minX = ImGui::GetCursorScreenPos().x;
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - topSt.WindowPadding.x;
        const float arrowX = rightEdge - clipBtnW - topSt.ItemSpacing.x - arrowW;

        static bool  searchOpen = false;
        static float searchAnim = 0.0f;       // 0 = closed, 1 = fully open
        static bool  focusSearch = false;
        static bool  arrowHoveredLast = false;
        static bool  searchFocused = false;   // state of the last frame (hides the placeholder while typing)
        static int   openFrames = 0;          // frames since the box was opened (gives the new focus a moment to arrive)

        if (g_search[0]) searchOpen = true;   // text in the box: it stays open
        if (searchOpen) ++openFrames; else openFrames = 0;
        const float animStep = ImGui::GetIO().DeltaTime / 0.15f;
        searchAnim += searchOpen ? animStep : -animStep;
        searchAnim = ClampF(searchAnim, 0.0f, 1.0f);

        // free space between the number field and the arrow button
        const float boxMax = ClampF(arrowX - minX, 0.0f, searchWidth);
        if (searchAnim > 0.0f && boxMax >= 20.0f)
        {
            const float boxW = ClampF(boxMax * searchAnim, 1.0f, boxMax);
            ImGui::SetCursorScreenPos(ImVec2(arrowX - boxW, rowY)); // touches the arrow button
            ImGui::SetNextItemWidth(boxW);
            int searchColors = 0;
            if (g_hintColor.custom)
            {
                ImGui::PushStyleColor(ImGuiCol_TextDisabled, ToVec4(g_hintColor)); // placeholder text
                ++searchColors;
            }
            if (g_searchBg.custom)
                searchColors += PushInputBackground(g_searchBg);
            if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
            ImGui::InputText("##search", g_search, sizeof(g_search));
            const bool active = ImGui::IsItemActive();
            ImGui::PopStyleColor(searchColors);

            if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) // Escape: clear and close
            {
                g_search[0] = '\0';
                searchOpen = false;
            }
            else if (searchOpen && !active && openFrames > 3 && !g_search[0] && !arrowHoveredLast)
            {
                searchOpen = false; // empty and no longer focused
            }
            searchFocused = active;
        }
        else
        {
            searchFocused = false;
        }

        // the arrow button
        ImGui::SetCursorScreenPos(ImVec2(arrowX, rowY));
        if (ImGui::ArrowButton("##searchtoggle", searchOpen ? ImGuiDir_Right : ImGuiDir_Left))
        {
            if (searchOpen && !g_search[0]) searchOpen = false;
            else if (!searchOpen) { searchOpen = true; focusSearch = true; }
            // with text in the box it stays open (clear the text or press Escape to close it)
        }
        arrowHoveredLast = ImGui::IsItemHovered();
        if (arrowHoveredLast)
            PuTooltip(g_search[0] ? "Clear Content to Close Search" : (searchOpen ? "Close Search" : "Open Search"));
        g_searchLower = ToLower(g_search);

        // button: copies your own Kp.me ID
        {
            static double copiedAt = -100.0;
            const KpData own = g_self.empty() ? KpData() : GetKp(g_self);
            const bool haveId = own.status == KpStatus::Loaded && !own.kpid.empty();
            const bool justCopied = ImGui::GetTime() - copiedAt < 1.5;

            ImGui::SameLine(0.0f, topSt.ItemSpacing.x);
            if (!haveId) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, topSt.Alpha * 0.5f);
            if (ImGui::Button(justCopied ? "Copied!" : CLIP_BUTTON_LABEL, ImVec2(clipBtnW, 0.0f)) && haveId)
            {
                ImGui::SetClipboardText(own.kpid.c_str());
                copiedAt = ImGui::GetTime();
            }
            if (!haveId) ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                if (haveId)                        PuTooltip("Copies Kp.me to Clipboard");
                else if (g_self.empty())           PuTooltip("Account Name Not Known");
                else                               PuTooltip("Kp.me ID Not Available");
            }
        }

        // a little space under the dropdowns
        ImGui::Dummy(ImVec2(0.0f, ClampF(10.0f - 2.0f * ImGui::GetStyle().ItemSpacing.y, 1.0f, 10.0f)));

        // You are always in the table, even when you are not in a group
        std::vector<Member> shown = g_members;
        if (!g_self.empty())
        {
            bool found = false;
            for (const Member& m : shown)
                if (m.Account == g_self) { found = true; break; }
            if (!found)
            {
                Member me;
                me.Account = g_self;
                me.InGroup = false;
                shown.insert(shown.begin(), me);
            }
        }

        if (ImGui::BeginTabBar("##playerutility_tabs"))
        {
            // General is the tab shown when the add-on is loaded
            static bool selectGeneral = true;
            const ImGuiTabItemFlags generalFlags = selectGeneral ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            selectGeneral = false;
            if (ImGui::BeginTabItem("General", nullptr, generalFlags))
            {
                g_activeTab = 0;
                DrawTable("general", ColumnsInOrder(TAB_GENERAL), WithLinkedRows(shown), -1, true);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Raid"))
            {
                g_activeTab = 1;
                DrawRaidTable(WithLinkedRows(shown), false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Raid Cm"))
            {
                g_activeTab = 2;
                DrawRaidTable(WithLinkedRows(shown), true);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Strikes"))
            {
                g_activeTab = 3;
                DrawStrikeTable(WithLinkedRows(shown), false);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Strikes Cm"))
            {
                g_activeTab = 4;
                DrawStrikeTable(WithLinkedRows(shown), true);
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags commentsFlags = g_gotoComments ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Comments", nullptr, commentsFlags))
            {
                g_gotoComments = false;
                g_activeTab = 5;
                DrawCommentsTab();
                ImGui::EndTabItem();
            }
            if (g_debug && ImGui::BeginTabItem("Advanced"))
            {
                g_activeTab = 6;
                DrawDebugTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            if (g_activeTab != 5) g_newRowActive = false; // the row for a new account is dropped when the tab is left
        }

        // click highlight: switching tabs clears it; so does a click elsewhere in the window (not on the title bar)
        if (g_activeTab != g_hlLastTab) { ClearClickHighlight(); g_hlLastTab = g_activeTab; }
        if (!g_hlRow.empty() && !g_hlHandled && !g_hlSet && ImGui::IsMouseClicked(0) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            ImGui::GetIO().MousePos.y > ImGui::GetWindowPos().y + ImGui::GetFrameHeight())
            ClearClickHighlight();
    }
    ImGui::End();

    ImGui::PopStyleVar(windowVars);
    ImGui::PopStyleColor(windowColors);
}

// --- small helpers for the options panel ---
// x position (inside the options window) where all input boxes start
static float OPT_COL = 270.0f; // each options tab sets its own value

static void OptionGap() { ImGui::Dummy(ImVec2(0.0f, 4.0f)); }      // space between options
static void GroupGap() { ImGui::Dummy(ImVec2(0.0f, 10.0f)); }     // space between groups

// Section heading of an options tab: bold, in the theme's accent color, with a line below it
static void SectionHeading(const char* text, bool first = false)
{
    if (!first) GroupGap();
    const ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const ImVec2 pos = ImGui::GetItemRectMin(); // where the text really was drawn
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.0f, pos.y), ImGui::GetColorU32(col), text); // fake bold
    ImGui::Separator();
    OptionGap();
}

// Explanation under an option: dimmed so it is not mistaken for a label or a heading
static void OptionDescription(const char* text)
{
    ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    dim.w *= 0.78f; // a little softer than the labels, but easy to read
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    OptionGap();
}

// "Label      [ value ]" with the box at the shared column
static void OptionLabel(const char* label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(OPT_COL);
}

static void ColorOption(const char* label, ColorSetting& s)
{
    ImGui::PushID(label);
    if (ImGui::Checkbox(label, &s.custom)) SaveSettings();
    if (s.custom)
    {
        ImGui::SameLine(OPT_COL);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::ColorEdit4("##color", s.c, ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
    }
    ImGui::PopID();
    OptionGap();
}

// a color without checkbox
static void PlainColorOption(const char* label, float* c)
{
    ImGui::PushID(label);
    OptionLabel(label);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_AlphaBar);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
    ImGui::PopID();
    OptionGap();
}

static void FloatOption(const char* label, FloatSetting& s, float minV, float maxV)
{
    ImGui::PushID(label);
    if (ImGui::Checkbox(label, &s.custom)) SaveSettings();
    if (s.custom)
    {
        ImGui::SameLine(OPT_COL);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputFloat("in px##value", &s.v, 0.0f, 0.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            s.v = ClampF(s.v, minV, maxV);
            SaveSettings();
        }
    }
    ImGui::PopID();
    OptionGap();
}

// whole-number box with the unit after it; true when the edit is finished
static bool PxOption(const char* label, int* v)
{
    ImGui::PushID(label);
    OptionLabel(label);
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("in px##v", v, 0, 0);
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    OptionGap();
    return done;
}

// drop-down with the label on the left
static bool ComboOption(const char* label, int* v, const char* items, float width = 100.0f)
{
    ImGui::PushID(label);
    OptionLabel(label);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::Combo("##combo", v, items);
    ImGui::PopID();
    OptionGap();
    return changed;
}

// whole-number box with +/- buttons and the label on the left
static bool StepOption(const char* label, int* v)
{
    ImGui::PushID(label);
    OptionLabel(label);
    ImGui::SetNextItemWidth(100.0f);
    const bool changed = ImGui::InputInt("##step", v, 1, 5);
    ImGui::PopID();
    OptionGap();
    return changed;
}

// Options tab "Columns": order, width and alignment of the table columns
static void OptionsColumnsTab()
{
    OPT_COL = 270.0f;
    // what is typed in the width fields; only applied when you press Enter or click away
    static int edit[COL_COUNT];
    static int editWing = 40, editBoss = 44;
    static int syncedGen = -1;
    if (syncedGen != g_tableGen)
    {
        for (int i = 0; i < COL_COUNT; ++i) edit[i] = (int)g_width[i];
        editWing = (int)g_wingWidth;
        editBoss = (int)g_bossWidth;
        syncedGen = g_tableGen;
    }

    if (ImGui::CollapsingHeader("Order (Left To Right)"))
    {
        for (int p = 0; p < COL_COUNT; ++p)
        {
            ImGui::PushID(1000 + p);

            if (ImGui::ArrowButton("##left", ImGuiDir_Left) && p > 0)
            {
                std::swap(g_order[p], g_order[p - 1]);
                ++g_tableGen; // rebuild the table in the new order
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##right", ImGuiDir_Right) && p < COL_COUNT - 1)
            {
                std::swap(g_order[p], g_order[p + 1]);
                ++g_tableGen;
                SaveSettings();
            }
            ImGui::SameLine();
            ImGui::Text("%d. %s", p + 1, COL_NAMES[g_order[p]]);

            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Width And Alignment"))
    {
        OPT_COL = 90.0f; // short labels: boxes close to them
        for (int p = 0; p < COL_COUNT; ++p)
        {
            const int c = g_order[p];
            ImGui::PushID(c);
            ImGui::TextUnformatted(COL_NAMES[c]);

            if (PxOption(c == COL_NOTES ? "Min. Width" : "Width", &edit[c]))
            {
                if (edit[c] < 20)  edit[c] = 20;
                if (edit[c] > 600) edit[c] = 600;
                g_width[c] = (float)edit[c];
                ++g_tableGen; // rebuild the table with the new width
                SaveSettings();
            }

            if (c != COL_NOTES) // the note field is always left-aligned
            {
                if (ComboOption("Alignment", &g_align[c], "Left\0Center\0Right\0"))
                    SaveSettings();
            }

            ImGui::PopID();
            GroupGap();
        }
    }

    OPT_COL = 270.0f;
    if (ImGui::CollapsingHeader("Raid And Strikes Tab Columns"))
    {
        if (PxOption("Collapsed Wing Width", &editWing))
        {
            if (editWing < 20)  editWing = 20;
            if (editWing > 300) editWing = 300;
            g_wingWidth = (float)editWing;
            ++g_tableGen;
            SaveSettings();
        }

        if (PxOption("Boss / Strike Column Width", &editBoss))
        {
            if (editBoss < 30)  editBoss = 30;
            if (editBoss > 300) editBoss = 300;
            g_bossWidth = (float)editBoss;
            ++g_tableGen;
            SaveSettings();
        }

        if (ComboOption("Number Alignment", &g_bossAlign, "Left\0Center\0Right\0"))
            SaveSettings();

        if (ImGui::Checkbox("Show Triangles In The Wing / Strike Headers", &g_showTriangles))
        {
            ++g_tableGen; // rebuild the tables and refit the window width
            SaveSettings();
        }
        OptionDescription("Hidden triangles allow narrower columns. Click the header name to expand or collapse.");

        OptionDescription("The Raid and Raid Cm tabs use the wing triangles; the Strikes tabs have one column per strike.");
    }
}

// Options tab "General": behaviour of the window and the tables
static void OptionsGeneralTab()
{
    OPT_COL = 190.0f;
    static int editIcon = 30;
    static int syncedGen = -1;
    if (syncedGen != g_tableGen) { editIcon = (int)g_iconSize; syncedGen = g_tableGen; }

    SectionHeading("Window", true);
    if (StepOption("Max Rows Shown", &g_maxRows))
    {
        if (g_maxRows < 1)  g_maxRows = 1;
        if (g_maxRows > 50) g_maxRows = 50;
        SaveSettings();
    }
    OptionDescription("The window grows with the players up to this many rows, then scrolls.");
    if (ImGui::Checkbox("Show Tooltips", &g_tooltips)) SaveSettings();
    OptionGap();
    if (ImGui::Checkbox("Show Data Tooltips", &g_dataTooltips)) SaveSettings();
    OptionGap();
    OptionDescription("Data tooltips show the Kp.me and Wingman values of the LI, UFE, Raid and Strikes cells and the wing, boss and strike names in the headers. They have their own switch, so they can stay on while the other tooltips are off.");
    if (ImGui::Checkbox("Show Quick Access Icon", &g_quickAccess)) { ApplyQuickAccess(); SaveSettings(); }
    OptionGap();

    SectionHeading("Table");
    if (ImGui::Checkbox("Highlight Clicked Row and Column", &g_clickHighlight))
    {
        if (!g_clickHighlight) ClearClickHighlight();
        SaveSettings();
    }
    OptionGap();
    OptionDescription("Click a cell to highlight its row and column until you click elsewhere. Not used in the Comments tab. The color is set in the Colors tab.");

    SectionHeading("Raid Tab");
    if (ImGui::Checkbox("Show Boss Icons Instead Of Names (Raid Tab)", &g_showIcons))
    {
        ++g_tableGen;
        SaveSettings();
    }
    OptionGap();
    if (PxOption("Raid Icon Size (16 - 48)", &editIcon))
    {
        g_iconSize = ClampF((float)editIcon, 16.0f, 48.0f);
        ++g_tableGen;
        SaveSettings();
    }
    OptionDescription("Icons are used for the bosses of expanded wings. The Strikes tabs always show text.");

    SectionHeading("Linked Accounts (Both Sources)");
    if (ImGui::Checkbox("Expand Linked Accounts By Default", &g_linkedExpanded))
        SaveSettings();
    OptionGap();
    if (ImGui::Checkbox("Copy Comments to Linked Accounts", &g_copyToLinked))
        SaveSettings();
    OptionGap();
    OptionDescription("New comments can be stored on all linked accounts. This only applies, if the linked accounts don't already have any comments. If a comment is already available it is not overwritten.");

}

// Options tab "Layout": sizes and text style
static void OptionsLayoutTab()
{
    OPT_COL = 190.0f;
    OptionDescription("Unticked options follow the Nexus theme.");

    SectionHeading("Table", true);
    FloatOption("Header Height", g_headerHeight, 10.0f, 100.0f);
    FloatOption("Row Height", g_rowHeight, 10.0f, 100.0f);
    FloatOption("Window Border Width", g_windowBorderSize, 0.0f, 10.0f);

    SectionHeading("Text");
    if (ImGui::Checkbox("Bold Header Text", &g_headerBold)) SaveSettings();
    OptionGap();
    if (ImGui::Checkbox("Bold Tooltip Titles", &g_tooltipBold)) SaveSettings();
    OptionGap();

}

// Options tab "Colors": every color of the add-on
static void OptionsColorsTab()
{
    OPT_COL = 200.0f;
    OptionDescription("Unticked colors follow the Nexus theme.");

    ColorOption("Header Color", g_headerBg);
    ColorOption("Table Border Color", g_tableBorder);
    ColorOption("Click Highlight Color", g_clickHlColor);
    ColorOption("Window Border Color", g_windowBorder);
    ColorOption("Note Background", g_noteBg);
    ColorOption("Note Text Color", g_noteText);
    ColorOption("Placeholder Text Color", g_hintColor);
    ColorOption("Search Box Background", g_searchBg);

    PlainColorOption("Search Highlight Color", g_highlight);

    SectionHeading("Threshold Highlight");
    if (ComboOption("Highlight Style", &g_thrStyle, "Cell background\0Number text\0Entire row\0", 160.0f))
        SaveSettings();
    OptionDescription("Entire row also keeps the cell background on the number that is below the threshold.");

    PlainColorOption("Cell Background Color", g_thrCellColor);
    PlainColorOption("Number Text Color", g_thrTextColor);
    PlainColorOption("Row Color", g_thrRowColor);

    // values of the right-click menu of the threshold field
    OptionLabel("Predefined Values");
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("##thrpresets", g_thrPresetText, sizeof(g_thrPresetText));
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        g_thrPresets = ParsePresets(g_thrPresetText);
        SyncPresetText(); // shows the cleaned list: sorted, no duplicates
        SaveSettings();
    }
    OptionGap();
    OptionDescription("Shown when you right-click the threshold field. Separate with commas, up to 12 values (0 - 999999).");

}

// Options tab "Data": options that depend on the data sources
static void OptionsDataTab()
{
    OPT_COL = 190.0f;
    SectionHeading("Raid Tab (Kp.me Source Only)", true);
    if (StepOption("Tokens Per Coffer (0 - 5)", &g_cofferValue))
    {
        if (g_cofferValue < 0) g_cofferValue = 0;
        if (g_cofferValue > 5) g_cofferValue = 5;
        SaveSettings();
    }
    OptionDescription("Coffers can have different amounts of tokens inside. When not opened, you can select here for how many tokens a coffer should count. By default a coffer counts as three tokens, which is consistent with other addons.");

}

// Options tab "Import": notes from the Player List addon
static void OptionsImportTab()
{
    static char path[600] = "";
    static bool initialised = false;
    static std::string result;
    if (!initialised)
    {
        initialised = true;
        if (APIDefs)
        {
            // <game folder>\addons\UnofficialPlayerUtility  ->  <game folder>\addons\arcdps\player_list.toml
            const fs::path addons = fs::path(APIDefs->Paths_GetAddonDirectory("UnofficialPlayerUtility")).parent_path();
            snprintf(path, sizeof(path), "%s", PathToUtf8(addons / "arcdps" / "player_list.toml").c_str());
        }
    }

    OptionDescription("Imports the comments of the Player List addon (player_list.toml).");
    OptionDescription("Names are taken as written in the file. An account that already has a note keeps it.");
    GroupGap();

    OPT_COL = 90.0f;
    OptionLabel("File");
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("##plpath", path, sizeof(path));
    OptionGap();

    if (ImGui::Button("Import From PlayerList Addon"))
    {
        const PlayerListImport r = ImportPlayerList(PathFromUtf8(path));
        if (!r.found)
        {
            result = "File not found. Check the path above.";
            DebugLog(SRC_ADDON, DST_NOTES, "Player List Import: File Not Found");
        }
        else
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "Imported %d. Skipped: %d already had a note, %d without a valid account name, %d without a comment.",
                r.imported, r.keptOwn, r.invalid, r.noComment);
            result = buf;
            DebugLog(SRC_ADDON, DST_NOTES, "Player List Import: %d Imported, %d Had a Note, %d Invalid Name, %d No Comment",
                r.imported, r.keptOwn, r.invalid, r.noComment);
            if (r.imported > 0) SaveNotes();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Folder"))
    {
        // the folder that contains the file (or the typed path itself when it is a folder)
        fs::path target = PathFromUtf8(path);
        std::error_code ec;
        if (!fs::is_directory(target, ec)) target = target.parent_path();
        if (!target.empty())
            ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (!result.empty())
    {
        OptionGap();
        ImGui::TextUnformatted(result.c_str());
    }
    GroupGap();
    GroupGap(); // space above the bottom row
}

// what "Reset This Tab" does for each options tab
static void ResetGeneralTab() { ResetGeneralSettings(); ApplyQuickAccess(); ++g_tableGen; DebugLog(SRC_ADDON, DST_SETTINGS, "General Settings Reset"); SaveSettings(); }
static void ResetLayoutTab() { ResetLayoutSettings(); ++g_tableGen; DebugLog(SRC_ADDON, DST_SETTINGS, "Layout Settings Reset"); SaveSettings(); }
static void ResetColumnsTab() { ResetColumnSettings(); ++g_tableGen; DebugLog(SRC_ADDON, DST_SETTINGS, "Column Settings Reset"); SaveSettings(); }
static void ResetColorsTab() { ResetColorSettings(); DebugLog(SRC_ADDON, DST_SETTINGS, "Color Settings Reset"); SaveSettings(); }
static void ResetAdvancedTab() { ResetAdvancedSettings(); DebugLog(SRC_ADDON, DST_SETTINGS, "Advanced Settings Reset"); SaveSettings(); }
static void ResetDataTab() { ResetDataSettings(); DebugLog(SRC_ADDON, DST_SETTINGS, "Data Settings Reset"); SaveSettings(); }

// Options tab "Advanced"
static void OptionsAdvancedTab()
{
    SectionHeading("Debug", true);
    if (ImGui::Checkbox("Show Debug Tab", &g_debug)) SaveSettings();
    OptionGap();
    OptionDescription("Adds an Advanced tab to the window with the event log of the add-on.");
}

// Shown in Nexus' options window: the tabs and a bottom row with Reset This Tab and Reset All
static void AddonOptions()
{
    void (*tabReset)() = nullptr; // reset of the tab that is shown (none for Import)
    if (ImGui::BeginTabBar("##playerutility_options"))
    {
        if (ImGui::BeginTabItem("General"))
        {
            OptionsGeneralTab();
            tabReset = ResetGeneralTab;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layout"))
        {
            OptionsLayoutTab();
            tabReset = ResetLayoutTab;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Columns"))
        {
            OptionsColumnsTab();
            tabReset = ResetColumnsTab;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Colors"))
        {
            OptionsColorsTab();
            tabReset = ResetColorsTab;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Data"))
        {
            OptionsDataTab();
            tabReset = ResetDataTab;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Import"))
        {
            OptionsImportTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced"))
        {
            OptionsAdvancedTab();
            tabReset = ResetAdvancedTab;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    // no reset for the shown tab: the button is dimmed and does nothing
    if (!tabReset) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
    const bool resetTab = ImGui::Button("Reset This Tab");
    if (!tabReset) ImGui::PopStyleVar();
    if (resetTab && tabReset) tabReset();
    ImGui::SameLine();
    if (ImGui::Button("Reset All"))
    {
        ResetSettings();
        ApplyQuickAccess();
        ++g_tableGen;
        DebugLog(SRC_ADDON, DST_SETTINGS, "Settings Reset to Defaults");
        SaveSettings();
    }
}

static void ToggleKeybind(const char* /*id*/, bool isRelease)
{
    if (!isRelease) g_visible = !g_visible;
}

// ---------------------------------------------------------------------------
// Load / unload
// ---------------------------------------------------------------------------
static void AddonLoad(AddonAPI_t* api)
{
    APIDefs = api;

    // Required: use Nexus' ImGui context and allocators
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void* (*)(size_t, void*))APIDefs->ImguiMalloc,
        (void (*)(void*, void*))APIDefs->ImguiFree);

    fs::path dir = APIDefs->Paths_GetAddonDirectory("UnofficialPlayerUtility");
    fs::create_directories(dir);
    StartDebugLog(dir);
    DebugLog(SRC_NEXUS, DST_ADDON, "Add-on Loaded (Version %d.%d.%d.%d)", (int)AddonDef.Version.Major, (int)AddonDef.Version.Minor, (int)AddonDef.Version.Build, (int)AddonDef.Version.Revision);
    g_settingsPath = dir / "settings.txt";
    ResetSettings(); // the defaults; a settings file of the user overrides them
    LoadSettings();
    g_notesPath = dir / "player_notes.txt";
    LoadNotes();

    StartKpWorker(); // background thread for the killproof.me lookups
    PreloadIcons();  // Nexus loads the boss icons in the background
    ApplyQuickAccess();

    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);
    APIDefs->Events_Subscribe("EV_UNOFFICIAL_EXTRAS_SQUAD_UPDATE", OnSquadUpdate);
    APIDefs->Events_Subscribe("EV_ACCOUNT_NAME", OnAccountName);
    APIDefs->InputBinds_RegisterWithString("KB_UNOFFICIALPLAYERUTILITY_TOGGLE", ToggleKeybind, "CTRL+SHIFT+K");
}

static void AddonUnload()
{
    DebugLog(SRC_NEXUS, DST_ADDON, "Add-on Unloaded");
    if (!g_noteDirty.empty()) { if (g_noteDirtyCopy) CopyNoteToLinked(g_noteDirty, g_notes[g_noteDirty]); SaveNotes(); g_noteDirty.clear(); }
    if (g_qaShown && APIDefs->QuickAccess_Remove) { APIDefs->QuickAccess_Remove(QA_ID); g_qaShown = false; }
    APIDefs->InputBinds_Deregister("KB_UNOFFICIALPLAYERUTILITY_TOGGLE");
    APIDefs->Events_Unsubscribe("EV_ACCOUNT_NAME", OnAccountName);
    APIDefs->Events_Unsubscribe("EV_UNOFFICIAL_EXTRAS_SQUAD_UPDATE", OnSquadUpdate);
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);

    StopKpWorker(); // wait for the background thread before the DLL goes away

    std::lock_guard<std::mutex> lock(g_mutex);
    g_members.clear();
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    AddonDef.Signature = -445566; // unique negative number while not hosted on Raidcore
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "Unofficial PlayerUtility";
    AddonDef.Version.Major = VERSION_MAJOR;
    AddonDef.Version.Minor = VERSION_MINOR;
    AddonDef.Version.Build = VERSION_BUILD;
    AddonDef.Version.Revision = VERSION_REVISION;
    AddonDef.Author = "Namt";
    AddonDef.Description = "Provide data from KillProof and Wingman and add notes to accounts.";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    return &AddonDef;
}
