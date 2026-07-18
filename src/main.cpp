#include <windows.h>
#include <cstdio>
#include <string>
#include "logger.h"
#include "gui.h"
#include "updater.h"

// -------------------------------------------------------------------------
// Crash handler state
// -------------------------------------------------------------------------
static std::wstring g_exePath;
static std::wstring g_logPath;

static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;

    // Noise: skip entirely without logging
    if (code == EXCEPTION_BREAKPOINT      ||  // debugger artifact
        code == EXCEPTION_SINGLE_STEP     ||  // debugger artifact
        code == 0x406D1388u               ||  
        code == 0x40010006u               ||  
        code == 0x40010003u)                  
        return EXCEPTION_CONTINUE_SEARCH;

    LOG_ERR("Exception 0x%08lX at %p (thread %lu)",
        code,
        ep ? ep->ExceptionRecord->ExceptionAddress : nullptr,
        GetCurrentThreadId());

    return EXCEPTION_CONTINUE_SEARCH;
}

static std::wstring ResolveExePath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static std::wstring ParseCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring arg;
    if (argc >= 2) arg = argv[1];
    LocalFree(argv);
    return arg;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    g_exePath = ResolveExePath();

    Logger::Init(g_exePath);
    g_logPath = Logger::GetLogPath();
    LOG_INFO("DXTViewer v" APP_VERSION " starting — exe: %ls", g_exePath.c_str());

    PVOID veh = AddVectoredExceptionHandler(0, VectoredExceptionHandler);
    LOG_DEBUG("Vectored exception handler registered: %p", veh);

    std::wstring cmdArg = ParseCommandLine();
    LOG_DEBUG("Command-line archive arg: %ls", cmdArg.empty() ? L"(none)" : cmdArg.c_str());

    if (!GuiInit(hInst, nCmdShow))
    {
        LOG_ERR("GuiInit failed — aborting");
        Logger::Shutdown();
        return 1;
    }

    if (!cmdArg.empty())
        GuiOpenArchive(GuiGetMainHwnd(), cmdArg);

    LOG_INFO("Entering message loop");

    int exitCode = GuiRun();

    if (veh) RemoveVectoredExceptionHandler(veh);
    LOG_INFO("Exiting with code %d", exitCode);
    Logger::Shutdown();

    return exitCode;
}
