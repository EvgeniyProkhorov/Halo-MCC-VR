#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>
// Constants only (native raster size and the resolution_scale limits) so the
// launcher and the DLL's config clamp can never disagree. The launcher does
// not link config.cpp; it reads the one line it needs itself.
#include "../common/config.h"

// Starts the MCC shipping executable directly — which is exactly what Steam's
// official "Play without anti-cheat" option runs, and what the Microsoft Store
// package's own "Halo: MCC Anti-Cheat Disabled (Mods and Limited Services)"
// entry runs, so EAC is never started — and loads halo3xr.dll into it before
// the game's first instruction runs.
//
// Injection method: classic CreateRemoteThread + LoadLibraryW. The process is
// created suspended, a remote thread loads our DLL, then the game resumes.
//
// Two editions ship the same game with different shipping executables:
//
//   Steam                MCC\Binaries\Win64\MCC-Win64-Shipping.exe
//   Microsoft Store      MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe
//   (Xbox app/Game Pass)
//
// The per-game modules the DLL actually hooks (halo3.dll, halo3odst.dll,
// haloreach.dll) are byte-identical between the two editions apart from their
// Authenticode signature and PE checksum, so one build serves both and no
// signature, offset or struct layout changes. Only the launch differs:
//
//   * Only the Steam edition needs Steam. The Store edition is licensed by the
//     Xbox app, so requiring steam.exe there would block a valid install.
//   * The SteamAppId environment variables exist purely so Steamworks does not
//     bounce the process ("relaunch me through Steam"), which would kill the
//     process we just injected into and start a fresh, unmodded one. The Store
//     build does not use Steamworks, so it is not given them.
//
// Edition is decided by the install layout, not just the executable name: the
// Store package roots the game at ...\Content\ next to MicrosoftGame.config,
// which no Steam install has. That way a Store install whose executable was
// renamed to the Steam name — the old community workaround this replaces — is
// still recognised as a Store install and is not asked to start Steam.

static const wchar_t* kSteamAppId = L"976730"; // Halo: The Master Chief Collection

static const wchar_t* kSteamExeName = L"MCC-Win64-Shipping.exe";
static const wchar_t* kStoreExeName = L"MCCWinStore-Win64-Shipping.exe";

enum class Edition
{
    Steam,
    Store,
};

static const wchar_t* EditionName(Edition edition)
{
    return edition == Edition::Store ? L"Microsoft Store / Xbox app (Game Pass)"
                                     : L"Steam";
}

static std::wstring g_logPath;

static void LauncherLog(const char* fmt, ...)
{
    if (g_logPath.empty())
        return;
    FILE* f = nullptr;
    _wfopen_s(&f, g_logPath.c_str(), L"at");
    if (!f)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void ErrorBox(const std::wstring& text)
{
    LauncherLog("ERROR shown to user: (see message box)");
    MessageBoxW(nullptr, text.c_str(), L"Halo MCC VR launcher", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

static std::wstring WinErr(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"(unknown error)";
    if (buf)
        LocalFree(buf);
    wchar_t num[32];
    swprintf_s(num, L" (code %lu)", code);
    return s + num;
}

static bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ProcessRunning(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static float ReadResolutionScale(const std::wstring& path)
{
    float scale = 1.0f;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rt");
    if (f)
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            float parsed = 1.0f;
            if (sscanf_s(line, " resolution_scale = %f", &parsed) == 1 &&
                std::isfinite(parsed))
                scale = parsed;
        }
        fclose(f);
    }
    // Any value in range is honored exactly; the named tiers are only F1
    // shortcuts. (Before 2026-07-20 this snapped to those six tiers,
    // so a hand-typed 0.90 silently became 0.80.)
    return std::clamp(scale, kResolutionScaleMin, kResolutionScaleMax);
}

static int ScaleEven(int base, float scale)
{
    int value = static_cast<int>(std::lround(static_cast<float>(base) * scale));
    if (value & 1)
        ++value;
    return value;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring dir(selfPath);
    dir.resize(dir.find_last_of(L'\\')); // folder containing the launcher, no trailing slash

    g_logPath = dir + L"\\halo3xr_launcher.log";
    // fresh log each launch
    {
        FILE* f = nullptr;
        _wfopen_s(&f, g_logPath.c_str(), L"wt");
        if (f) fclose(f);
    }
    LauncherLog("launcher started; self dir = %ls", dir.c_str());

    const std::wstring dllPath = dir + L"\\halo3xr.dll";
    if (!FileExists(dllPath))
    {
        ErrorBox(L"halo3xr.dll is missing from:\n" + dir +
                 L"\n\nCopy halo3xr.dll from the package into this same folder,\n"
                 L"next to halo3xr_launcher.exe.");
        return 1;
    }

    // The launcher lives in <game>\Halo_MCC_VR\, so the game exe is found by
    // walking up from here. A few levels are tried so a dev copy placed
    // elsewhere inside the game folder also works. Both editions' executable
    // names are tried at every level, so one launcher serves either install.
    std::wstring gameExe, gameDir, gameRoot;
    std::wstring probe = dir;
    bool foundStoreExeName = false;
    for (int i = 0; i < 6 && !probe.empty(); i++)
    {
        const std::wstring binDir = probe + L"\\MCC\\Binaries\\Win64";
        // The Store name is tried first: a genuine Steam install never ships
        // it, so finding it is unambiguous.
        const std::wstring storeCand = binDir + L"\\" + kStoreExeName;
        const std::wstring steamCand = binDir + L"\\" + kSteamExeName;
        if (FileExists(storeCand))
        {
            gameExe = storeCand;
            foundStoreExeName = true;
        }
        else if (FileExists(steamCand))
        {
            gameExe = steamCand;
        }
        if (!gameExe.empty())
        {
            gameDir = binDir;
            gameRoot = probe;
            break;
        }
        const size_t slash = probe.find_last_of(L'\\');
        if (slash == std::wstring::npos)
            break;
        probe.resize(slash);
    }
    if (gameExe.empty())
    {
        ErrorBox(L"Could not find the Halo: MCC program near:\n" + dir +
                 L"\n\nLooked for both editions' executables:\n"
                 L"  MCC\\Binaries\\Win64\\" + kSteamExeName + L"   (Steam)\n"
                 L"  MCC\\Binaries\\Win64\\" + kStoreExeName + L"   (Microsoft Store)\n\n"
                 L"The Halo_MCC_VR folder must sit inside the game's install\n"
                 L"folder - the one that contains the MCC folder. For the\n"
                 L"Microsoft Store / Xbox app edition that is the Content folder.");
        return 1;
    }

    // MicrosoftGame.config sits beside the MCC folder in the Store package and
    // in no Steam install, so it identifies the edition even when the
    // executable was renamed to the Steam name by hand.
    const bool storeLayout = FileExists(gameRoot + L"\\MicrosoftGame.config");
    const Edition edition =
        (foundStoreExeName || storeLayout) ? Edition::Store : Edition::Steam;
    LauncherLog("game exe = %ls", gameExe.c_str());
    LauncherLog("detected edition: %ls (store exe name=%d, MicrosoftGame.config=%d)",
                EditionName(edition), foundStoreExeName ? 1 : 0, storeLayout ? 1 : 0);

    // Steam only owns the Steam edition's licensing. The Store edition is
    // licensed by the Xbox app, so demanding Steam there would reject a
    // perfectly valid install.
    if (edition == Edition::Steam && !ProcessRunning(L"steam.exe"))
    {
        ErrorBox(L"Steam is not running.\n\nStart Steam first, then run this launcher again.\n"
                 L"(This Steam copy of the game needs Steam to verify ownership.)");
        return 1;
    }
    // Either edition's process blocks the other: MCC allows one instance, and
    // an already-running copy is not the one we injected into.
    if (ProcessRunning(kSteamExeName) || ProcessRunning(kStoreExeName))
    {
        ErrorBox(L"Halo: The Master Chief Collection is already running.\n\n"
                 L"Close it first, then run this launcher again.");
        return 1;
    }

    if (edition == Edition::Steam)
    {
        // Make Steamworks believe the game was launched correctly, so it does
        // not ask Steam to relaunch it (which would drop the process we inject
        // into). The child process inherits these environment variables.
        SetEnvironmentVariableW(L"SteamAppId", kSteamAppId);
        SetEnvironmentVariableW(L"SteamGameId", kSteamAppId);
        SetEnvironmentVariableW(L"SteamOverlayGameId", kSteamAppId);
        LauncherLog("set SteamAppId=%ls; creating game process (suspended)", kSteamAppId);
    }
    else
    {
        // The Store build does not link Steamworks, so it is launched clean.
        LauncherLog("Store edition: no Steam requirement, no SteamAppId set; "
                    "creating game process (suspended)");
    }

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // M2 stereo: render the game into a wide surface matching Halo's VR
    // projection rather than the normal 16:9 desktop frame. Windowed mode
    // avoids asking the monitor for a non-standard display mode.
    // PSVR2's symmetric coverage has a tangent-space aspect near 1.386:1.
    // 2912x2100 keeps approximately the same pixel cost as 2448x2496 while
    // allowing Halo to generate the correct wide raster/culling frustum.
    // resolution_scale reduces both dimensions together, preserving this
    // aspect and Halo's culling/projection. The DLL then upscales the complete
    // eye into the unchanged full-size OpenXR projection. This can help
    // GPU-limited systems; it cannot remove CPU/per-render engine cost.
    // (kNativeRenderWidth/Height live in ../common/config.h so the F1 menu can
    // show the same pixel count this line will produce.)
    const std::wstring primaryConfig = dir + L"/halomccvr.cfg";
    const std::wstring legacyConfig = dir + L"/halo3xr.cfg";
    const DWORD primaryAttributes = GetFileAttributesW(primaryConfig.c_str());
    const bool primaryExists = primaryAttributes != INVALID_FILE_ATTRIBUTES &&
        (primaryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    const float resolutionScale =
        ReadResolutionScale(primaryExists ? primaryConfig : legacyConfig);
    const int renderWidth = ScaleEven(kNativeRenderWidth, resolutionScale);
    const int renderHeight = ScaleEven(kNativeRenderHeight, resolutionScale);
    // Hand the DLL the EXACT render surface size we are launching with, so that
    // when fit_desktop_window is on it can force MCC's swapchain backbuffer to
    // this exact size (matching this ScaleEven, so the two can never disagree)
    // and preserve it through window resize. The child inherits these because
    // CreateProcessW is given a NULL environment block. Harmless when the fit is
    // off -- the DLL only reads them in that case.
    wchar_t envBuf[16];
    swprintf_s(envBuf, L"%d", renderWidth);
    SetEnvironmentVariableW(L"HALO3XR_RENDER_W", envBuf);
    swprintf_s(envBuf, L"%d", renderHeight);
    SetEnvironmentVariableW(L"HALO3XR_RENDER_H", envBuf);
    wchar_t renderArgs[96];
    swprintf_s(renderArgs, L" -WINDOWED -ResX=%d -ResY=%d",
               renderWidth, renderHeight);
    const wchar_t quote = 34;
    std::wstring cmdline(1, quote);
    cmdline += gameExe;
    cmdline += quote;
    cmdline += renderArgs;
    LauncherLog("VR render command line: -WINDOWED -ResX=%d -ResY=%d "
                "(resolution_scale %.2f; native %dx%d)",
                renderWidth, renderHeight, resolutionScale,
                kNativeRenderWidth, kNativeRenderHeight);
    if (!CreateProcessW(gameExe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, gameDir.c_str(), &si, &pi))
    {
        const DWORD e = GetLastError();
        LauncherLog("CreateProcessW failed: %lu", e);
        ErrorBox(L"Could not start the game:\n" + WinErr(e));
        return 1;
    }
    LauncherLog("game process created, pid %lu", pi.dwProcessId);

    // Write the DLL path into the game process and make it call LoadLibraryW
    // on it. LoadLibraryW lives at the same address in every 64-bit process.
    bool injected = false;
    std::wstring failWhy;
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
        failWhy = L"VirtualAllocEx: " + WinErr(GetLastError());
    else if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPath.c_str(), bytes, nullptr))
        failWhy = L"WriteProcessMemory: " + WinErr(GetLastError());
    else
    {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibrary, remoteMem, 0, nullptr);
        if (!thread)
            failWhy = L"CreateRemoteThread: " + WinErr(GetLastError());
        else
        {
            if (WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                if (exitCode != 0) // LoadLibrary returns the module handle; 0 means it failed
                    injected = true;
                else
                    failWhy = L"LoadLibraryW returned NULL inside the game "
                              L"(missing dependency or blocked DLL?)";
            }
            else
                failWhy = L"the injection thread timed out";
            CloseHandle(thread);
        }
        VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
    }

    if (!injected)
    {
        LauncherLog("injection FAILED: %ls", failWhy.c_str());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ErrorBox(L"Could not load the VR mod into the game, so the game was closed.\n\n"
                 L"Reason: " + failWhy);
        return 1;
    }
    LauncherLog("DLL injected OK; resuming game");

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Watch the game for a short while. If it exits almost immediately, it
    // bounced through Steam, failed the Store licence check, or crashed on
    // startup — capture the exit code so we can see what happened instead of
    // the process silently vanishing.
    const DWORD watchMs = 12000;
    const DWORD waitRes = WaitForSingleObject(pi.hProcess, watchMs);
    if (waitRes == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        LauncherLog("game process EXITED within %lus, exit code %lu (0x%08X)", watchMs / 1000, code, code);
        CloseHandle(pi.hProcess);
        const std::wstring why =
            edition == Edition::Store
                ? L"This usually means the Xbox app could not confirm your licence for the\n"
                  L"game. Open the Xbox app and start Halo: MCC once so it signs in, close\n"
                  L"it, then run this launcher again."
                : L"This usually means it relaunched through Steam or the anti-cheat-off\n"
                  L"mode refused this launch.";
        ErrorBox(L"The game closed itself right after starting (exit code " +
                 std::to_wstring(code) + L").\n\n" + why +
                 L"\n\nDetails are in halo3xr_launcher.log.");
        return 1;
    }
    LauncherLog("game still running after %lus - launch looks good", watchMs / 1000);
    CloseHandle(pi.hProcess);
    return 0;
}
