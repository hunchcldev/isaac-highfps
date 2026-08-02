// Loader for the high-FPS mod.
//
//   inject.exe <dll>                  attach to a running isaac-ng.exe
//   inject.exe <dll> --launch <exe>   start the game ourselves, then inject
//
// Attaching to a *running* Isaac fails with ACCESS_DENIED on this build (the Steam
// DRM stub in the .bind section hardens the process), so --launch is the mode that
// actually works: CreateProcess hands us a full-access handle for a child we own.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>

static DWORD FindPid(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof pe};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool InjectInto(HANDLE proc, const char* dll) {
    size_t n = strlen(dll) + 1;
    void* remote = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll, n, nullptr)) {
        printf("remote write failed: %lu\n", GetLastError());
        return false;
    }
    // Both processes are 32-bit, so kernel32 sits at the same base in both.
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { printf("CreateRemoteThread failed: %lu\n", GetLastError()); return false; }

    WaitForSingleObject(th, 15000);
    DWORD ret = 0;
    GetExitCodeThread(th, &ret);
    CloseHandle(th);
    if (!ret) { printf("LoadLibraryA returned NULL in the target\n"); return false; }
    printf("injected (remote module 0x%08lX)\n", ret);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: inject.exe <dll> [--launch <isaac-ng.exe>]\n");
        return 2;
    }
    char dll[MAX_PATH];
    if (!GetFullPathNameA(argv[1], MAX_PATH, dll, nullptr) ||
        GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        printf("no such dll: %s\n", argv[1]);
        return 2;
    }

    if (argc >= 4 && strcmp(argv[2], "--launch") == 0) {
        char exe[MAX_PATH];
        if (!GetFullPathNameA(argv[3], MAX_PATH, exe, nullptr)) { printf("bad exe path\n"); return 2; }

        // Steam must already be running; the game resolves its app id through the
        // running client. Working directory has to be the install dir or resource
        // loading fails.
        char dir[MAX_PATH];
        strcpy(dir, exe);
        if (char* slash = strrchr(dir, '\\')) *slash = 0;

        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
            printf("CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }
        printf("launched pid %lu, waiting for the loader to settle...\n", pi.dwProcessId);
        // Let the real loader map kernel32 and friends before we ask for LoadLibraryA.
        WaitForInputIdle(pi.hProcess, 10000);
        Sleep(2500);

        bool ok = InjectInto(pi.hProcess, dll);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return ok ? 0 : 1;
    }

    DWORD pid = FindPid(L"isaac-ng.exe");
    if (!pid) { printf("isaac-ng.exe is not running\n"); return 1; }
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        printf("OpenProcess(%lu) failed: %lu - use --launch instead\n", pid, GetLastError());
        return 1;
    }
    bool ok = InjectInto(proc, dll);
    CloseHandle(proc);
    return ok ? 0 : 1;
}
