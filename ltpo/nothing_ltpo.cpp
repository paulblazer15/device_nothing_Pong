/*
 * Copyright (C) 2026 The AxionAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr char kSkipFramePath[] = "/sys/panel_feature/skip_frame_mode";
constexpr char kMeasuredFpsPath[] = "/sys/class/drm/card0/sde-crtc-0/measured_fps";
constexpr char kDisplayEnabledPath[] = "/sys/class/drm/card0/card0-DSI-1/enabled";
constexpr char kBrightnessPath[] = "/sys/class/backlight/panel0-backlight/brightness";
constexpr char kInputDirectory[] = "/dev/input";

constexpr int kActiveIndex = 16;
constexpr int k60HzIndex = 7;
constexpr int k30HzIndex = 8;
constexpr int k10HzIndex = 10;
constexpr int k1HzIndex = 11;
constexpr int64_t kOverlayFrameGraceNs = 2000000000LL;
constexpr unsigned long long kOverlayMaxFrames = 2;

std::atomic<int64_t> gLastInputNs{0};

int64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

bool PathExists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

std::vector<pollfd> OpenInputDevices() {
    std::vector<pollfd> devices;
    DIR* directory = opendir(kInputDirectory);
    if (directory == nullptr) {
        PLOG(WARNING) << "Unable to open " << kInputDirectory;
        return devices;
    }

    while (dirent* entry = readdir(directory)) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        const std::string path = std::string(kInputDirectory) + "/" + entry->d_name;
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            devices.push_back({.fd = fd, .events = POLLIN, .revents = 0});
        }
    }
    closedir(directory);
    return devices;
}

void CloseInputDevices(std::vector<pollfd>* devices) {
    for (const auto& device : *devices) {
        close(device.fd);
    }
    devices->clear();
}

void MonitorInput() {
    while (true) {
        auto devices = OpenInputDevices();
        const int64_t rescanAt = NowNs() + 10000000000LL;

        while (NowNs() < rescanAt) {
            const int result = poll(devices.data(), devices.size(), 250);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                PLOG(WARNING) << "Input poll failed";
                break;
            }
            if (result == 0) {
                continue;
            }

            for (auto& device : devices) {
                if ((device.revents & POLLIN) == 0) {
                    continue;
                }

                input_event events[16];
                while (read(device.fd, events, sizeof(events)) > 0) {
                    gLastInputNs.store(NowNs(), std::memory_order_relaxed);
                }
            }
        }

        CloseInputDevices(&devices);
    }
}

bool ReadDisplayEnabled(bool* enabled) {
    std::string value;
    if (!android::base::ReadFileToString(kDisplayEnabledPath, &value)) {
        return false;
    }
    *enabled = android::base::Trim(value) == "enabled";
    return true;
}

bool ReadBrightness(int* brightness) {
    std::string value;
    if (!android::base::ReadFileToString(kBrightnessPath, &value)) {
        return false;
    }
    return android::base::ParseInt(android::base::Trim(value), brightness);
}

unsigned long long ReadRecentFrameCount() {
    std::string value;
    if (!android::base::ReadFileToString(kMeasuredFpsPath, &value)) {
        return 0;
    }

    unsigned long long frameCount = 0;
    const size_t position = value.find("frame_count:");
    if (position == std::string::npos ||
        sscanf(value.c_str() + position, "frame_count:%llu", &frameCount) != 1) {
        return 0;
    }
    return frameCount;
}

int ReadMinimumFps() {
    const int requested =
            android::base::GetIntProperty("persist.vendor.display.ltpo.min_fps", 1);
    if (requested >= 60) return 60;
    if (requested >= 30) return 30;
    if (requested >= 10) return 10;
    return 1;
}

int DesiredIndex(int64_t idleMs, int minimumFps, int64_t idle30Ms, int64_t idle10Ms,
                 int64_t idle1Ms) {
    if (idleMs < idle30Ms) return kActiveIndex;
    if (minimumFps == 60) return k60HzIndex;
    if (idleMs < idle10Ms || minimumFps == 30) return k30HzIndex;
    if (idleMs < idle1Ms || minimumFps == 10) return k10HzIndex;
    return k1HzIndex;
}

const char* IndexName(int index) {
    switch (index) {
        case k60HzIndex:
            return "60 Hz idle";
        case k30HzIndex:
            return "30 Hz idle";
        case k10HzIndex:
            return "10 Hz idle";
        case k1HzIndex:
            return "1 Hz idle";
        default:
            return "active";
    }
}

int IndexToFps(int index) {
    switch (index) {
        case k60HzIndex:
            return 60;
        case k30HzIndex:
            return 30;
        case k10HzIndex:
            return 10;
        case k1HzIndex:
            return 1;
        default:
            return 120;
    }
}

bool ApplyIndex(int index) {
    if (!android::base::WriteStringToFile(std::to_string(index), kSkipFramePath)) {
        PLOG(WARNING) << "Failed to switch panel to " << IndexName(index) << " (index " << index
                      << ")";
        return false;
    }
    if (!android::base::SetProperty("vendor.display.ltpo.current_fps",
                                    std::to_string(IndexToFps(index)))) {
        LOG(WARNING) << "Unable to publish the effective panel refresh rate";
    }
    LOG(INFO) << "Panel state: " << IndexName(index) << " (index " << index << ")";
    return true;
}

}  // namespace

int main(int /* argc */, char** argv) {
    android::base::InitLogging(argv,
                               android::base::LogdLogger(android::base::SYSTEM));

    // The kernel only creates panel_feature for Pong's NT37705 panel. The RM692E5
    // fallback panel has no matching low-refresh command tables and is left alone.
    if (!PathExists(kSkipFramePath)) {
        LOG(ERROR) << "NT37705 LTPO interface is unavailable; controller will not start";
        return 0;
    }

    // Qualcomm's generic SM8450 boot script enables an HWC idle fallback that
    // changes the physical base mode to 60 Hz. Pong's panel driver only accepts
    // skip-frame commands while the base mode is 120 Hz, so the LTPO controller
    // replaces that fallback with the panel's native 30/10/1 Hz sequence.
    if (!android::base::SetProperty("vendor.display.enable_allow_idle_fallback", "0")) {
        LOG(WARNING) << "Unable to disable the Qualcomm HWC idle fallback";
    }

    gLastInputNs.store(NowNs(), std::memory_order_relaxed);
    std::thread inputThread(MonitorInput);
    inputThread.detach();

    int appliedIndex = -1;
    int previousBrightness = -1;
    bool wasEnabled = false;
    int64_t lastActivityNs = NowNs();
    int64_t lastTransitionNs = 0;

    LOG(INFO) << "Pong NT37705 LTPO controller started";

    while (true) {
        bool displayEnabled = false;
        if (!ReadDisplayEnabled(&displayEnabled) || !displayEnabled) {
            appliedIndex = -1;
            wasEnabled = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        const int64_t nowNs = NowNs();
        if (!wasEnabled) {
            lastActivityNs = nowNs;
            wasEnabled = true;
        }

        int brightness = -1;
        if (ReadBrightness(&brightness)) {
            if (previousBrightness >= 0 && brightness != previousBrightness) {
                lastActivityNs = nowNs;
            }
            previousBrightness = brightness;
        }

        const int64_t inputNs = gLastInputNs.load(std::memory_order_relaxed);
        const unsigned long long frameCount = ReadRecentFrameCount();
        // Updating the developer refresh-rate overlay produces one compositor
        // frame after a panel transition. Ignore that bounded echo while still
        // treating input, brightness changes, or a real frame burst as activity.
        const bool overlayOnlyFrames =
                lastTransitionNs > 0 && nowNs - lastTransitionNs < kOverlayFrameGraceNs &&
                frameCount <= kOverlayMaxFrames;
        if (inputNs > lastActivityNs || (frameCount > 0 && !overlayOnlyFrames)) {
            lastActivityNs = std::max(inputNs, nowNs);
        }

        const bool enabled = android::base::GetBoolProperty(
                "persist.vendor.display.ltpo.enabled", true);
        int targetIndex = kActiveIndex;
        if (enabled) {
            const int minimumFps = ReadMinimumFps();
            const int64_t idle30Ms = std::max<int64_t>(
                    250, android::base::GetIntProperty(
                                 "persist.vendor.display.ltpo.idle_30_ms", 1500));
            const int64_t idle10Ms = std::max<int64_t>(
                    idle30Ms, android::base::GetIntProperty(
                                      "persist.vendor.display.ltpo.idle_10_ms", 4000));
            const int64_t idle1Ms = std::max<int64_t>(
                    idle10Ms, android::base::GetIntProperty(
                                      "persist.vendor.display.ltpo.idle_1_ms", 8000));
            const int64_t idleMs = (nowNs - lastActivityNs) / 1000000LL;
            targetIndex = DesiredIndex(idleMs, minimumFps, idle30Ms, idle10Ms, idle1Ms);
        }

        if (targetIndex != appliedIndex) {
            if (ApplyIndex(targetIndex)) {
                lastTransitionNs = nowNs;
            }
            // A fixed 60/90 Hz display mode can temporarily reject the NT37705
            // command. Record the attempt to avoid repeatedly hitting the kernel.
            appliedIndex = targetIndex;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
