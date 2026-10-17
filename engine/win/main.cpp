// dB-Deck audio engine process.
//
//   dbdeck-engine.exe [--config-dir <dir>] [--pipe <name>] [--quiet]
//
// Runs until it receives the "shutdown" IPC command or a console close
// signal. A per-user named mutex prevents duplicate engine instances.

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "ComUtil.h"
#include "EngineService.h"
#include "PipeServer.h"
#include "util/Log.h"

using namespace dbdeck;
using namespace dbdeck::win;

namespace {

EngineService* g_service = nullptr;

BOOL WINAPI consoleHandler(DWORD) {
    if (g_service) g_service->requestShutdown();
    return TRUE;
}

std::filesystem::path appDataDir() {
    PWSTR path = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) p = path;
    CoTaskMemFree(path);
    return p / L"dB-Deck";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    EngineOptions opts;
    opts.configDir = appDataDir();
    opts.pipeName = defaultPipeName();
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config-dir" && i + 1 < argc) opts.configDir = argv[++i];
        else if (a == L"--pipe" && i + 1 < argc) opts.pipeName = std::wstring(L"\\\\.\\pipe\\") + argv[++i];
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--version") {
            std::printf("dB-Deck engine 0.1.0 (protocol %d)\n", kProtocolVersion);
            return 0;
        }
    }

    // One engine per user session (named after the pipe so tests can run side by side).
    std::wstring mutexName = L"Local\\dB-Deck.Engine." + std::to_wstring(std::hash<std::wstring>{}(opts.pipeName));
    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, mutexName.c_str()));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "dB-Deck engine is already running.\n");
        return 2;
    }

    log::init(opts.configDir / L"logs" / L"engine.log", !quiet);
    log::info("Config directory: " + toUtf8(opts.configDir.wstring()));
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    int code = 0;
    {
        EngineService service(opts);
        g_service = &service;
        std::string error;
        if (!service.start(error)) {
            log::error("Engine failed to start: " + error);
            code = 1;
        } else {
            service.run();
        }
        g_service = nullptr;
    }
    log::info("Engine exited with code " + std::to_string(code));
    return code;
}
