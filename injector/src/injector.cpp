// injector.cpp — launcher/injector for gmod-mcp (x64).
//
// Subcommands (machine-readable JSON on stdout):
//   injector status [--name gmod.exe] [--dll core.dll]
//   injector inject [--name gmod.exe] [--dll core.dll]
//   injector eject  [--name gmod.exe] [--dll core.dll]
//
// Finds the Garry's Mod process, reports whether core.dll is loaded and whether
// the IPC pipe answers, and performs LoadLibrary injection on demand. Must be the
// same bitness as the target (x64 for the x86-64 GMod branch).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>

namespace {

const char* kPipeName = "\\\\.\\pipe\\gmod_mcp";

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default: o += c;
        }
    }
    return o;
}

void PrintJsonResult(bool ok, const std::string& fields, const std::string& error = "") {
    std::string out = "{\"ok\":";
    out += ok ? "true" : "false";
    if (!fields.empty()) { out += ","; out += fields; }
    if (!ok && !error.empty()) { out += ",\"error\":\""; out += JsonEscape(error); out += "\""; }
    out += "}";
    printf("%s\n", out.c_str());
}

std::string LowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

void* FindModule(DWORD pid, const std::string& moduleName); // fwd decl

// Returns the pid of the real, fully-loaded game process. The x86-64 GMod branch
// uses Chromium (CEF), which spawns helper subprocesses (and a brief launcher)
// that share the gmod.exe name. Only the real game process has lua_shared.dll
// loaded. We REQUIRE that module — injecting into an early/CEF process during
// startup can crash the launch. Returns 0 if the game isn't ready yet.
DWORD FindProcess(const std::vector<std::string>& names) {
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            std::string exe = LowerStr(pe.szExeFile);
            bool nameMatch = false;
            for (const auto& n : names) if (exe == LowerStr(n)) { nameMatch = true; break; }
            if (nameMatch && FindModule(pe.th32ProcessID, "lua_shared.dll")) {
                found = pe.th32ProcessID; // the engine/game process — the only safe target
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Returns base address of `moduleName` in process `pid`, or nullptr.
void* FindModule(DWORD pid, const std::string& moduleName) {
    void* base = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32 me = {}; me.dwSize = sizeof(me);
    std::string target = LowerStr(moduleName);
    if (Module32First(snap, &me)) {
        do {
            if (LowerStr(me.szModule) == target) { base = me.modBaseAddr; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// True if any process with one of these names exists (even if not yet ready).
bool AnyNamedProcess(const std::vector<std::string>& names) {
    bool any = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            std::string exe = LowerStr(pe.szExeFile);
            for (const auto& n : names) if (exe == LowerStr(n)) { any = true; break; }
        } while (!any && Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return any;
}

bool PipeAlive() {
    HANDLE h = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string ExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? "." : p.substr(0, slash);
}

bool Inject(DWORD pid, const std::string& dllPath, std::string& err) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { err = "OpenProcess failed (run as admin?)"; return false; }

    // Wide path bytes to write into the target.
    std::wstring wpath(dllPath.begin(), dllPath.end());
    SIZE_T bytes = (wpath.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { err = "VirtualAllocEx failed"; CloseHandle(proc); return false; }

    if (!WriteProcessMemory(proc, remote, wpath.c_str(), bytes, nullptr)) {
        err = "WriteProcessMemory failed";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLib) { err = "LoadLibraryW resolve failed"; VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false; }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) {
        err = "CreateRemoteThread failed";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode); // low 32 bits of the HMODULE on success

    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exitCode == 0) { err = "LoadLibraryW returned NULL in target (bad DLL path/bitness?)"; return false; }
    return true;
}

bool Eject(DWORD pid, void* moduleBase, std::string& err) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION, FALSE, pid);
    if (!proc) { err = "OpenProcess failed"; return false; }
    auto freeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"));
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, freeLib, moduleBase, 0, nullptr);
    if (!thread) { err = "CreateRemoteThread(FreeLibrary) failed"; CloseHandle(proc); return false; }
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    CloseHandle(proc);
    return true;
}

std::string GetOpt(int argc, char** argv, const std::string& flag, const std::string& def) {
    for (int i = 2; i < argc - 1; ++i)
        if (flag == argv[i]) return argv[i + 1];
    return def;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintJsonResult(false, "", "usage: injector <status|inject|eject> [--name gmod.exe] [--dll core.dll]");
        return 2;
    }

    std::string cmd = argv[1];
    std::string procName = GetOpt(argc, argv, "--name", "gmod.exe");
    std::string dllPath = GetOpt(argc, argv, "--dll", ExeDir() + "\\core.dll");

    // Derive the DLL basename for module enumeration.
    std::string dllName = dllPath;
    size_t s = dllName.find_last_of("\\/");
    if (s != std::string::npos) dllName = dllName.substr(s + 1);

    std::vector<std::string> names = { procName, "gmod.exe", "hl2.exe" };
    DWORD pid = FindProcess(names);

    if (cmd == "status") {
        bool running = pid != 0;                 // ready = has lua_shared loaded
        bool loading = !running && AnyNamedProcess(names);
        void* mod = running ? FindModule(pid, dllName) : nullptr;
        bool injected = mod != nullptr;
        bool pipe = PipeAlive();
        char fields[320];
        _snprintf_s(fields, sizeof(fields), _TRUNCATE,
                    "\"running\":%s,\"loading\":%s,\"pid\":%lu,\"injected\":%s,\"pipeAlive\":%s",
                    running ? "true" : "false", loading ? "true" : "false", pid,
                    injected ? "true" : "false", pipe ? "true" : "false");
        PrintJsonResult(true, fields);
        return 0;
    }

    if (cmd == "inject") {
        if (!pid) {
            if (AnyNamedProcess(names))
                PrintJsonResult(false, "\"loading\":true",
                                "Garry's Mod is still loading (lua_shared not up yet) - wait for the main menu, then retry");
            else
                PrintJsonResult(false, "", "Garry's Mod is not running");
            return 1;
        }
        if (FindModule(pid, dllName)) {
            PrintJsonResult(true, "\"alreadyInjected\":true");
            return 0;
        }
        // Verify the DLL exists before we try.
        if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            PrintJsonResult(false, "", "DLL not found: " + dllPath);
            return 1;
        }
        std::string err;
        if (!Inject(pid, dllPath, err)) { PrintJsonResult(false, "", err); return 1; }
        PrintJsonResult(true, "\"injected\":true");
        return 0;
    }

    if (cmd == "eject") {
        if (!pid) { PrintJsonResult(false, "", "Garry's Mod is not running"); return 1; }
        void* mod = FindModule(pid, dllName);
        if (!mod) { PrintJsonResult(true, "\"injected\":false"); return 0; }
        std::string err;
        if (!Eject(pid, mod, err)) { PrintJsonResult(false, "", err); return 1; }
        PrintJsonResult(true, "\"ejected\":true");
        return 0;
    }

    PrintJsonResult(false, "", "unknown command: " + cmd);
    return 2;
}
