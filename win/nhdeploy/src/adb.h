#pragma once

#include <string>
#include <vector>

struct CmdResult {
    int code = -1;
    std::string out;
};

// Runs `exe args...`, captures stdout+stderr, returns the exit code.
CmdResult run_capture(const std::string &exe, const std::vector<std::string> &args);

// Runs and inherits the console (used for the logcat tail). Blocks.
int run_stream(const std::string &exe, const std::vector<std::string> &args);

std::string trim(const std::string &s);
std::vector<std::string> split_lines(const std::string &s);
std::string shell_quote(const std::string &s);

// Locates adb: $NH_ADB, $ANDROID_HOME/$ANDROID_SDK_ROOT platform-tools, then PATH.
std::string find_adb();

// Serials reported by `adb devices`, excluding the header and offline entries.
std::vector<std::string> adb_devices(const std::string &adb);
