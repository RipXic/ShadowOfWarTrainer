// Shadow of War Trainer - loader.
// Attaches ShadowOfWarTrainer.dll to ShadowOfWar.exe. Nothing in the game folder is modified (no ASI, no mod manager).
//   ShadowOfWarTrainer.exe            game not running: start it through Steam and attach as soon as its graphics DLLs
//                                     load (before the swapchain exists). Game already running: attach now.
//   ShadowOfWarTrainer.exe --attach   only attach to a running game
//   ShadowOfWarTrainer.exe --watch    stay in the background and attach every time the game starts
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "shell32.lib")

static const wchar_t* kGameExe = L"ShadowOfWar.exe";
static const wchar_t* kDllName = L"ShadowOfWarTrainer.dll";
static const wchar_t* kSteamUrl = L"steam://rungameid/356190";

static void say(WORD color, const char* tag, const char* msg) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, color); printf("%s", tag);
    SetConsoleTextAttribute(h, 7); printf(" %s\n", msg);
}
#define INFO(m) say(8, "  ..", m)
#define GOOD(m) say(10, "  OK", m)
#define BAD(m)  say(12, "FAIL", m)

static DWORD find_pid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{ sizeof e }; DWORD pid = 0;
    for (BOOL k = Process32FirstW(s, &e); k; k = Process32NextW(s, &e))
        if (_wcsicmp(e.szExeFile, kGameExe) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(s);
    return pid;
}

static bool has_module(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{ sizeof m }; bool found = false;
    for (BOOL k = Module32FirstW(s, &m); k; k = Module32NextW(s, &m))
        if (_wcsicmp(m.szModule, name) == 0) { found = true; break; }
    CloseHandle(s);
    return found;
}

struct WndFind { DWORD pid; bool found; };
static BOOL CALLBACK wnd_cb(HWND h, LPARAM lp) { auto* f = (WndFind*)lp; DWORD p = 0; GetWindowThreadProcessId(h, &p); if (p == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->found = true; return FALSE; } return TRUE; }
static bool has_window(DWORD pid) { WndFind f{ pid, false }; EnumWindows(wnd_cb, (LPARAM)&f); return f.found; }

static bool trainer_active(DWORD pid) {
    char nm[64]; sprintf_s(nm, "Local\\ShadowOfWarTrainerActive_%lu", pid);
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, nm);
    if (h) { CloseHandle(h); return true; }
    return false;
}

// Each attach loads its own copy (loaded\ShadowOfWarTrainer_<time>.dll), so the DLL next to the loader is never locked
// by the game and can be replaced while the game runs.
static std::wstring stage_copy(const std::wstring& dir, const std::wstring& dll) {
    std::wstring ld = dir + L"\\loaded";
    CreateDirectoryW(ld.c_str(), nullptr);
    // remove copies from earlier runs. A copy the game still has loaded is locked, so DeleteFile fails on it and it stays.
    {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((ld + L"\\*.dll").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((ld + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[96];
    std::wstring stem = kDllName; stem = stem.substr(0, stem.size() - 4);   // ShadowOfWarTrainer
    swprintf_s(name, L"\\%s_%04d%02d%02d_%02d%02d%02d_%03d.dll", stem.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring dst = ld + name;
    return CopyFileW(dll.c_str(), dst.c_str(), FALSE) ? dst : L"";
}

static bool inject(DWORD pid, const std::wstring& dll) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { BAD("Cannot open ShadowOfWar.exe (run the loader as the same user as the game)."); return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool okw = mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr);
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = okw ? CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr) : nullptr;
    DWORD code = 0;
    if (t) { WaitForSingleObject(t, 20000); GetExitCodeThread(t, &code); CloseHandle(t); }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return t && code != 0;
}

// --watch: stays in the background (no window), attaches the trainer every time the game starts - however it
// was launched (Steam, shortcut, ...). Attaches as soon as DirectX loads.
static int watch_mode(const std::wstring& dir, const std::wstring& dll) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ShadowOfWarTrainerWatcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;          // one watcher only
    FreeConsole();
    for (;;) {
        DWORD pid = 0;
        while (!(pid = find_pid())) Sleep(25);
        if (!trainer_active(pid)) {
            int waited = 0;
            while (!(has_window(pid) && (has_module(pid, L"dxgi.dll") || has_module(pid, L"d3d11.dll"))) && waited < 180 * 20) { Sleep(50); ++waited; DWORD now = find_pid(); if (!now) break; pid = now; }
            if (find_pid() == pid) {
                Sleep(150);
                std::wstring copy = stage_copy(dir, dll);
                if (!copy.empty()) inject(pid, copy);
            }
        }
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);         // sleep until this game session ends
        if (ph) { WaitForSingleObject(ph, INFINITE); CloseHandle(ph); } else Sleep(2000);
        Sleep(1000);
    }
    (void)single;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"Shadow of War Trainer");
    printf("\n  SHADOW OF WAR TRAINER\n  ---------------------\n\n");
    bool attach_only = argc > 1 && _wcsicmp(argv[1], L"--attach") == 0;

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dll = dir + L"\\" + kDllName;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { BAD("The trainer DLL is missing next to the loader."); Sleep(4000); return 2; }
    if (argc > 1 && _wcsicmp(argv[1], L"--watch") == 0) return watch_mode(dir, dll);

    DWORD pid = find_pid();
    bool early = false;
    if (!pid) {
        if (attach_only) { BAD("Middle-earth™: Shadow of War™ is not running."); Sleep(3000); return 3; }
        INFO("Middle-earth™: Shadow of War™ is not running - starting it through Steam...");
        ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        for (int i = 0; i < 180 * 40 && !pid; ++i) { Sleep(25); pid = find_pid(); }
        if (!pid) { BAD("Middle-earth™: Shadow of War™ did not start within 3 minutes."); Sleep(4000); return 4; }
        early = true;
    }
    char buf[160]; sprintf_s(buf, "Found ShadowOfWar.exe (pid %lu)", pid); GOOD(buf);
    if (trainer_active(pid)) { GOOD("Trainer is already attached."); Sleep(3000); return 0; }

    if (early) {
        // attach the moment the game loads its graphics stack: the DX11 hook then sees the swapchain being created
        INFO("Waiting for the game to load DirectX...");
        int waited = 0;
        // the exe may restart itself once while starting: follow the pid until DirectX loads
        while (!(has_window(pid) && (has_module(pid, L"dxgi.dll") || has_module(pid, L"d3d11.dll"))) && waited < 180 * 20) {
            Sleep(50); ++waited;
            DWORD now = find_pid();
            if (!now) { for (int k = 0; k < 600 && !now; ++k) { Sleep(50); now = find_pid(); } if (!now) { BAD("Middle-earth™: Shadow of War™ closed."); return 5; } }
            if (now != pid) { pid = now; INFO("Game restarted itself - following the new process."); }
        }
        Sleep(150);
    } else {
        INFO("Game already running - late attach.");
    }

    std::wstring copy = stage_copy(dir, dll);
    if (copy.empty()) { BAD("Could not copy the trainer DLL into the loaded folder."); Sleep(4000); return 8; }
    if (!inject(pid, copy)) { BAD("Attach failed - see trainer.log next to the DLL."); Sleep(5000); return 7; }
    GOOD(early ? "Trainer attached early (full overlay support)." : "Trainer attached.");
    INFO("Open the trainer in game with the key shown in README.txt.");
    Sleep(3000);
    return 0;
}
