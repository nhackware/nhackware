#include "adb.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

static std::string build_cmdline(const std::string &exe, const std::vector<std::string> &args) {
    std::string cl = "\"" + exe + "\"";
    for (const auto &a : args) {
        cl += " ";
        if (a.find_first_of(" \t\"") == std::string::npos) {
            cl += a;
        } else {
            std::string esc;
            for (char c : a) {
                if (c == '"')
                    esc += "\\\"";
                else if (c == '\\')
                    esc += "\\\\";
                else
                    esc += c;
            }
            cl += "\"" + esc + "\"";
        }
    }
    return cl;
}

CmdResult run_capture(const std::string &exe, const std::vector<std::string> &args) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return {-1, "CreatePipe failed"};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cl = build_cmdline(exe, args);
    std::vector<char> clbuf(cl.begin(), cl.end());
    clbuf.push_back('\0');

    if (!CreateProcessA(nullptr, clbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        DWORD e = GetLastError();
        CloseHandle(rd);
        CloseHandle(wr);
        return {-1, "CreateProcess failed: " + std::to_string(e)};
    }
    CloseHandle(wr); // must close our copy or ReadFile never sees EOF

    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {(int)code, out};
}

int run_stream(const std::string &exe, const std::vector<std::string> &args) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cl = build_cmdline(exe, args);
    std::vector<char> clbuf(cl.begin(), cl.end());
    clbuf.push_back('\0');

    if (!CreateProcessA(nullptr, clbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (!line.empty())
            out.push_back(line);
    }
    return out;
}

std::string shell_quote(const std::string &s) {
    // adb shell parses its own layer; single quotes survive both adb and mksh.
    return "'" + s + "'";
}

static bool file_exists(const std::string &p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

std::string find_adb() {
    const char *env = getenv("NH_ADB");
    if (env && *env && file_exists(env))
        return env;

    const char *roots[] = {getenv("ANDROID_HOME"), getenv("ANDROID_SDK_ROOT"), nullptr};
    for (const char *root : roots) {
        if (!root || !*root)
            continue;
        std::string cand = std::string(root) + "\\platform-tools\\adb.exe";
        if (file_exists(cand))
            return cand;
    }

    std::string local = std::string(getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : "") +
                        "\\Android\\Sdk\\platform-tools\\adb.exe";
    if (getenv("LOCALAPPDATA") && file_exists(local))
        return local;

    return "adb"; // fall back to PATH
}

std::vector<std::string> adb_devices(const std::string &adb) {
    CmdResult r = run_capture(adb, {"devices"});
    std::vector<std::string> out;
    for (const auto &line : split_lines(r.out)) {
        if (line.rfind("List of devices", 0) == 0 || line[0] == '*')
            continue;
        auto sp = line.find('\t');
        if (sp == std::string::npos)
            continue;
        std::string serial = trim(line.substr(0, sp));
        std::string state = trim(line.substr(sp + 1));
        if (state != "device")
            continue;
        out.push_back(serial);
    }
    return out;
}
