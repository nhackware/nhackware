// nhdeploy - Windows side of the nhackware injection chain.
//
// The emulator's guest Android cannot be ptraced from Windows, so this drives adb:
// push the injector + payload, root the guest, relax SELinux, then run the
// on-device ptrace injector against the target process.
//
//   nhdeploy --package com.example.game
//   nhdeploy --package com.example.game --watch --log
//   nhdeploy --unload
//
#include "adb.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string package;
    std::string serial;
    std::string abi = "x86_64";
    std::string lib;
    std::string injector;
    std::string dest = "/data/local/tmp/nhmenu";
    std::string activity;
    std::string unload_handle;
    int timeout_s = 60;
    bool root = true;
    bool setenforce = true;
    bool watch = false;
    bool log = false;
    bool unload = false;
};

std::string g_adb;
bool g_have_su = false;
bool g_is_root_shell = false;

void usage() {
    std::cout <<
        "nhdeploy - inject libnhmenu.so into an Android 11 emulator guest\n"
        "\n"
        "  nhdeploy --package <pkg> [options]\n"
        "  nhdeploy --unload [--handle 0x...] [--package <pkg>]\n"
        "\n"
        "options:\n"
        "  --package <pkg>        target process/package name\n"
        "  --serial <s>           adb serial (default: first online device)\n"
        "  --abi <name>           x86_64 (default) | arm64-v8a\n"
        "  --lib <path>           libnhmenu.so   (default: ..\\..\\out\\android\\<abi>\\libnhmenu.so)\n"
        "  --injector <path>      nhinject       (default: ..\\..\\out\\android\\<abi>\\nhinject)\n"
        "  --dest <dir>           device staging dir (default: /data/local/tmp/nhmenu)\n"
        "  --activity <comp>      'am start -n' component to launch before injecting\n"
        "  --timeout <s>          seconds to wait for the process (default 60)\n"
        "  --watch                re-inject whenever the target restarts\n"
        "  --log                  tail 'logcat -s NHMENU' after injecting\n"
        "  --no-root              skip 'adb root'\n"
        "  --no-setenforce        skip 'setenforce 0'\n"
        "  --unload               dlclose a previous injection\n"
        "  --handle <0x...>       handle to dlclose (default: last one recorded)\n";
}

std::vector<std::string> adb_base(const std::string &serial) {
    return {"-s", serial};
}

CmdResult adb(const std::string &serial, const std::vector<std::string> &args) {
    std::vector<std::string> full = adb_base(serial);
    full.insert(full.end(), args.begin(), args.end());
    return run_capture(g_adb, full);
}

// `adb shell` body, routed through su when adbd is not already root.
CmdResult shell(const std::string &serial, const std::string &cmd) {
    if (g_is_root_shell || !g_have_su)
        return adb(serial, {"shell", cmd});
    return adb(serial, {"shell", "su -c " + shell_quote(cmd)});
}

std::string prop(const std::string &serial, const std::string &key) {
    CmdResult r = shell(serial, "getprop " + key);
    return trim(r.out);
}

std::string pidof(const std::string &serial, const std::string &pkg) {
    CmdResult r = shell(serial, "pidof " + pkg);
    std::string out = trim(r.out);
    if (out.empty())
        return "";
    // pidof can return several pids; take the first
    auto sp = out.find(' ');
    return sp == std::string::npos ? out : out.substr(0, sp);
}

std::string state_path() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    fs::path p(buf);
    p.replace_filename("nhdeploy.state");
    return p.string();
}

void save_state(const std::string &serial, const std::string &pkg, const std::string &handle) {
    std::ofstream f(state_path(), std::ios::trunc);
    if (f)
        f << serial << "\n" << pkg << "\n" << handle << "\n";
}

bool load_state(std::string &serial, std::string &pkg, std::string &handle) {
    std::ifstream f(state_path());
    if (!f)
        return false;
    std::getline(f, serial);
    std::getline(f, pkg);
    std::getline(f, handle);
    serial = trim(serial);
    pkg = trim(pkg);
    handle = trim(handle);
    return !serial.empty();
}

bool establish_root(const std::string &serial, const Options &o) {
    if (o.root) {
        CmdResult r = adb(serial, {"root"});
        std::string msg = trim(r.out);
        std::cout << "[adb] root: " << msg << "\n";
        // adbd restarts when it switches uid; wait for it to come back
        adb(serial, {"wait-for-device"});
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }

    CmdResult id = adb(serial, {"shell", "id -u"});
    g_is_root_shell = (trim(id.out) == "0");
    std::cout << "[adb] shell uid = " << trim(id.out)
              << (g_is_root_shell ? " (root)" : "") << "\n";

    if (!g_is_root_shell) {
        CmdResult su = adb(serial, {"shell", "su -c id -u"});
        g_have_su = (trim(su.out) == "0");
        std::cout << "[adb] su available: " << (g_have_su ? "yes" : "no") << "\n";
        if (!g_have_su) {
            std::cerr << "[!] no root: ptrace will be denied. Use a userdebug/eng emulator\n"
                         "    image or a rooted player build (LDPlayer/Nox/MuMu).\n";
            return false;
        }
    }

    if (o.setenforce) {
        CmdResult r = shell(serial, "getenforce");
        std::string mode = trim(r.out);
        std::cout << "[adb] selinux: " << mode << "\n";
        if (mode == "Enforcing") {
            shell(serial, "setenforce 0");
            std::cout << "[adb] setenforce 0 -> " << trim(shell(serial, "getenforce").out) << "\n";
        }
    }
    return true;
}

bool stage(const std::string &serial, const Options &o) {
    if (!fs::exists(o.injector)) {
        std::cerr << "[!] injector not found: " << o.injector << "\n"
                  << "    run build\\build_android.ps1 -Abi " << o.abi << " first\n";
        return false;
    }
    if (!o.unload && !fs::exists(o.lib)) {
        std::cerr << "[!] payload not found: " << o.lib << "\n"
                  << "    run build\\build_android.ps1 -Abi " << o.abi << " first\n";
        return false;
    }

    shell(serial, "mkdir -p " + o.dest);

    CmdResult r = adb(serial, {"push", o.injector, o.dest + "/nhinject"});
    std::cout << "[adb] " << trim(r.out) << "\n";
    if (r.code != 0)
        return false;

    if (!o.unload) {
        r = adb(serial, {"push", o.lib, o.dest + "/libnhmenu.so"});
        std::cout << "[adb] " << trim(r.out) << "\n";
        if (r.code != 0)
            return false;
    }

    r = shell(serial, "chmod 755 " + o.dest + "/nhinject");
    if (r.code != 0) {
        std::cerr << "[!] chmod failed: " << trim(r.out) << "\n";
        return false;
    }

    // /data/local/tmp is noexec on some builds; catch it here instead of at exec time
    CmdResult probe = shell(serial, o.dest + "/nhinject 2>&1 | head -n 1");
    if (probe.out.find("Permission denied") != std::string::npos) {
        std::cerr << "[!] " << o.dest << " is mounted noexec; retry with --dest /data/data/"
                  << o.package << "/files\n";
        return false;
    }
    return true;
}

std::string wait_for_pid(const std::string &serial, const Options &o) {
    for (int i = 0; i < o.timeout_s * 2; i++) {
        std::string pid = pidof(serial, o.package);
        if (!pid.empty()) {
            std::cout << "[target] " << o.package << " pid " << pid << "\n";
            return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (i % 4 == 3)
            std::cout << "[target] waiting for " << o.package << " ...\n";
    }
    return "";
}

std::string inject(const std::string &serial, const Options &o, const std::string &pid) {
    std::string cmd = o.dest + "/nhinject " + pid + " " + o.dest + "/libnhmenu.so 2>&1";
    CmdResult r = shell(serial, cmd);
    std::cout << "[nhinject] " << trim(r.out) << "\n";

    std::string handle;
    for (const auto &line : split_lines(r.out)) {
        auto pos = line.find("NHMENU_HANDLE=");
        if (pos != std::string::npos)
            handle = trim(line.substr(pos + 14));
    }
    if (!handle.empty()) {
        save_state(serial, o.package, handle);
        std::cout << "[nhdeploy] recorded handle " << handle << " for --unload\n";
    }
    return handle;
}

int do_unload(const std::string &serial, Options &o) {
    if (o.unload_handle.empty()) {
        std::string s, p, h;
        if (!load_state(s, p, h) || h.empty()) {
            std::cerr << "[!] no handle recorded; pass --handle 0x...\n";
            return 1;
        }
        o.unload_handle = h;
        if (o.package.empty())
            o.package = p;
    }
    if (o.package.empty()) {
        std::cerr << "[!] --unload needs --package (or a recorded state file)\n";
        return 1;
    }
    std::string pid = pidof(serial, o.package);
    if (pid.empty()) {
        std::cerr << "[!] " << o.package << " is not running\n";
        return 1;
    }
    CmdResult r = shell(serial, o.dest + "/nhinject -u " + pid + " " + o.unload_handle + " 2>&1");
    std::cout << "[nhinject] " << trim(r.out) << "\n";
    return r.code;
}

bool check_abi(const std::string &serial, const Options &o) {
    std::string abi = prop(serial, "ro.product.cpu.abi");
    std::string rel = prop(serial, "ro.build.version.release");
    std::string sdk = prop(serial, "ro.build.version.sdk");
    std::cout << "[guest] android " << rel << " (sdk " << sdk << "), abi " << abi << "\n";

    std::string want = (o.abi == "arm64-v8a") ? "arm64-v8a" : "x86_64";
    if (!abi.empty() && abi != want) {
        std::cerr << "[!] guest abi is '" << abi << "' but you are deploying '" << want
                  << "'. Rebuild with --abi " << abi << ".\n";
        return false;
    }
    if (sdk == "30" || rel.rfind("11", 0) == 0)
        std::cout << "[guest] confirmed Android 11\n";
    return true;
}

} // namespace

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[!] " << what << " needs a value\n";
                exit(2);
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--package") {
            o.package = next("--package");
        } else if (a == "--serial") {
            o.serial = next("--serial");
        } else if (a == "--abi") {
            o.abi = next("--abi");
        } else if (a == "--lib") {
            o.lib = next("--lib");
        } else if (a == "--injector") {
            o.injector = next("--injector");
        } else if (a == "--dest") {
            o.dest = next("--dest");
        } else if (a == "--activity") {
            o.activity = next("--activity");
        } else if (a == "--timeout") {
            o.timeout_s = atoi(next("--timeout").c_str());
        } else if (a == "--handle") {
            o.unload_handle = next("--handle");
        } else if (a == "--watch") {
            o.watch = true;
        } else if (a == "--log") {
            o.log = true;
        } else if (a == "--no-root") {
            o.root = false;
        } else if (a == "--no-setenforce") {
            o.setenforce = false;
        } else if (a == "--unload") {
            o.unload = true;
        } else {
            std::cerr << "[!] unknown option " << a << "\n";
            usage();
            return 2;
        }
    }

    if (o.package.empty() && !o.unload) {
        usage();
        return 2;
    }

    g_adb = find_adb();
    std::cout << "[adb] using " << g_adb << "\n";

    auto devices = adb_devices(g_adb);
    if (devices.empty()) {
        std::cerr << "[!] no online adb devices. Start the emulator and check 'adb devices'.\n";
        return 1;
    }
    if (o.serial.empty()) {
        o.serial = devices.front();
        if (devices.size() > 1)
            std::cout << "[adb] " << devices.size() << " devices online, using " << o.serial
                      << " (pick another with --serial)\n";
    } else {
        bool found = false;
        for (const auto &d : devices)
            if (d == o.serial)
                found = true;
        if (!found) {
            std::cerr << "[!] serial " << o.serial << " is not online\n";
            return 1;
        }
    }
    std::cout << "[adb] serial " << o.serial << "\n";

    // Default payload paths, relative to the exe: out/android/<abi>/
    char exepath[MAX_PATH];
    GetModuleFileNameA(nullptr, exepath, MAX_PATH);
    fs::path root = fs::path(exepath).parent_path().parent_path().parent_path();
    if (o.lib.empty())
        o.lib = (root / "out" / "android" / o.abi / "libnhmenu.so").string();
    if (o.injector.empty())
        o.injector = (root / "out" / "android" / o.abi / "nhinject").string();

    if (!check_abi(o.serial, o) && !o.unload)
        return 1;

    if (!establish_root(o.serial, o))
        return 1;

    if (!stage(o.serial, o))
        return 1;

    if (o.unload)
        return do_unload(o.serial, o);

    if (!o.activity.empty()) {
        CmdResult r = shell(o.serial, "am start -n " + o.activity);
        std::cout << "[am] " << trim(r.out) << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    std::string pid = wait_for_pid(o.serial, o);
    if (pid.empty()) {
        std::cerr << "[!] " << o.package << " never started\n";
        return 1;
    }

    // The game needs a beat after fork before its linker is in a safe state to
    // drive with ptrace; injecting into a half-initialised process deadlocks.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    std::string handle = inject(o.serial, o, pid);
    if (handle.empty()) {
        std::cerr << "[!] injection failed\n";
        return 1;
    }

    if (o.log) {
        std::cout << "\n--- logcat -s NHMENU (ctrl+c to stop) ---\n";
        adb(o.serial, {"logcat", "-s", "NHMENU:V"});
    }

    if (o.watch) {
        std::cout << "[watch] monitoring " << o.package << "\n";
        std::string last = pid;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::string cur = pidof(o.serial, o.package);
            if (cur.empty() || cur == last)
                continue;
            std::cout << "[watch] restarted as pid " << cur << ", re-injecting\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            inject(o.serial, o, cur);
            last = cur;
        }
    }

    return 0;
}
