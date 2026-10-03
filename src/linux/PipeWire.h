// Linux: set the PipeWire graph rate via "pw-metadata -n settings 0 clock.force-rate <Hz>".
// If the graph runs at the stream's rate, PipeWire does not resample.
#pragma once
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cstdlib>

#include "../Common.h"

extern char** environ;

namespace ar {
namespace lnx {

class PipeWire {
    uint32_t forced_ = 0;

    static bool InPath(const char* exe) {
        const char* path = getenv("PATH");
        std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == std::string::npos) end = p.size();
            std::string full = p.substr(start, end - start) + "/" + exe;
            if (access(full.c_str(), X_OK) == 0) return true;
            start = end + 1;
        }
        return false;
    }

    static bool Run(std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) return false;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return false;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

public:
    static bool Available() { return InPath("pw-metadata"); }

    uint32_t Forced() const { return forced_; }

    bool ForceRate(uint32_t hz) {
        bool ok = Run({"pw-metadata", "-n", "settings", "0", "clock.force-rate", std::to_string(hz)});
        if (ok) forced_ = hz;
        return ok;
    }

    // 0 = release the forced rate, PipeWire chooses again
    void Release() {
        if (!forced_) return;
        Run({"pw-metadata", "-n", "settings", "0", "clock.force-rate", "0"});
        forced_ = 0;
    }
};

}  // namespace lnx
}  // namespace ar
