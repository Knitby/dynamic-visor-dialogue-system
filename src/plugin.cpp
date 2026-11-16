// Dynamic Visor Dialogue system (DVDs)
// Copyright (C) 2026 Knitby
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, version 3.
// It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details. You should have received a copy of the license with this
// program; if not, see <https://www.gnu.org/licenses/>.
//
// Keeps the player's helmet visor lowered during dialogue when it was lowered before.
// KCSE plugin for Kingdom Come: Deliverance 2, WHGame.dll 1.5.6 (game version 0x01050600).
//
// How it works (all RVAs found with Ghidra, Ghidra address - 0x180000000 = RVA; per build, see kBuilds):
//  * The vanilla visor toggle (RVA 0x2AA79CC) flags the helmet items with 0x80000 (RVA 0x46643C).
//    We remember which items those are while the player's toggle runs.
//  * A dialogue re-adds the player's gear to temporary clothing managers (AddItem, RVA 0x9DDB58) and
//    creates a NEW C_VisorPoseModifier that starts open. When one of the remembered helmets is re-added
//    with the flag set, we arm the next new modifier.
//  * That modifier (RVA 0x111201C = Execute) gets target=1, enabled=1, weight=1.0 (the closed look) and
//    is kept that way on every Execute, because the game resets target to 0 within a few frames.
//
// Layout of C_VisorPoseModifier: +8 float weight, +0x10 short joint index (-1 none),
// +0x12 byte enabled, +0x13 byte target.

#include <MinHook.h>

#include "KCSE/KCSEAPI.h"
#include "REL.h"
#include "crysystem/SSystemGlobalEnvironment.h"

#include <chrono>
#include <cstring>
#include <mutex>

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t    kGameVersion   = 0x01050600;  // 1.5.6

// Addresses (RVAs) of the five functions we need, per supported build of WHGame.dll. The game version
// number is the same (1.5.6) for every storefront, but each storefront has its own build with its own
// addresses, so the build is recognised by the PE header (timestamp + image size) of the loaded WHGame.dll.
// Any other build gets no hooks at all.
struct BuildInfo {
    const char*   name;
    std::uint32_t timestamp;   // IMAGE_FILE_HEADER.TimeDateStamp
    std::uint32_t imageSize;   // IMAGE_OPTIONAL_HEADER.SizeOfImage
    std::size_t   toggle;      // player visor toggle (Z)
    std::size_t   setFlag;     // SetElementFlag(item, flag, value)
    std::size_t   addItem;     // clothing manager AddItem(mgr, item)
    std::size_t   poseExec;    // C_VisorPoseModifier::Execute
    std::size_t   isType;      // IsType(item, type), 0x10 = helmet
};

constexpr BuildInfo kBuilds[] = {
    { "Steam", 0x6A350E20, 0x05B2D000, 0x2AA79CC, 0x46643C, 0x9DDB58, 0x111201C, 0x469590 },
    { "Epic",  0x6A34F917, 0x05B18000, 0x2A9CAB4, 0x465F1C, 0x4F3320, 0x110FA9C, 0x469070 },
};
const BuildInfo* g_build = nullptr;
constexpr std::uint32_t kVisorFlag   = 0x80000;
constexpr long long   kArmWindowMs   = 3000;        // armed state expires after this
constexpr long long   kIdleMs        = 1500;        // a modifier not run for this long is gone / new
constexpr int         kMaxLogLines   = 200;
constexpr int         kPoseSlots     = 128;
constexpr int         kMaxHelmets    = 8;

using Fn = uint64_t (*)(void*, void*, void*, void*);
Fn g_origToggle = nullptr;
Fn g_origSetFlag = nullptr;
Fn g_origAdd = nullptr;
Fn g_origExec = nullptr;

std::atomic<int>       g_lines{ 0 };
std::atomic<long long> g_lastToggleMs{ 0 };
std::atomic<long long> g_armUntilMs{ 0 };
std::atomic<void*>     g_stickyPose{ nullptr };
std::atomic<long long> g_stickyLastMs{ 0 };
std::atomic<long long> g_stickyStartMs{ 0 };
thread_local bool      t_inToggle = false;

// Helmet items the player's toggle has flagged.
std::mutex g_helmetMutex;
void*      g_helmets[kMaxHelmets] = {};

// Pose modifiers seen recently.
struct PoseSeen { std::uintptr_t ptr = 0; long long last = 0; };
PoseSeen   g_pose[kPoseSlots];
std::mutex g_poseMutex;

#define DVDS_LOG(...)                                                          \
    do {                                                                     \
        if (g_lines.fetch_add(1) < kMaxLogLines) {                           \
            auto* env_ = SSystemGlobalEnvironment::GetInstance();            \
            if (env_ && env_->pLog) env_->pLog->LogAlways(__VA_ARGS__);      \
        }                                                                    \
    } while (0)

long long NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- safe memory access -------------------------------------------------------------------

long long SafeU32(void* base, std::size_t offset)
{
    if (!base) return -1;
    __try {
        return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(base) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool IsHelmetType(void* item)
{
    if (!item) return false;
    __try {
        auto fn = reinterpret_cast<std::uint8_t (*)(void*, unsigned)>(REL::Offset(g_build->isType).address());
        return fn(item, 0x10) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPose(void* self, float* w, int* joint, int* en, int* tgt)
{
    __try {
        auto* p = reinterpret_cast<std::uint8_t*>(self);
        *w     = *reinterpret_cast<float*>(p + 8);
        *joint = *reinterpret_cast<std::int16_t*>(p + 0x10);
        *en    = *(p + 0x12);
        *tgt   = *(p + 0x13);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WritePoseClosed(void* self)
{
    __try {
        auto* p = reinterpret_cast<std::uint8_t*>(self);
        *(p + 0x13) = 1;
        *(p + 0x12) = 1;
        *reinterpret_cast<float*>(p + 8) = 1.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- helmet set ---------------------------------------------------------------------------

void HelmetAdd(void* item)
{
    std::lock_guard<std::mutex> lock(g_helmetMutex);
    int freeSlot = -1;
    for (int i = 0; i < kMaxHelmets; ++i) {
        if (g_helmets[i] == item) return;
        if (!g_helmets[i] && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) freeSlot = 0;   // full: overwrite the oldest slot
    g_helmets[freeSlot] = item;
}

void HelmetRemove(void* item)
{
    std::lock_guard<std::mutex> lock(g_helmetMutex);
    for (int i = 0; i < kMaxHelmets; ++i) {
        if (g_helmets[i] == item) g_helmets[i] = nullptr;
    }
}

bool HelmetKnown(void* item)
{
    std::lock_guard<std::mutex> lock(g_helmetMutex);
    for (int i = 0; i < kMaxHelmets; ++i) {
        if (g_helmets[i] == item) return true;
    }
    return false;
}

// ---- hooks --------------------------------------------------------------------------------

// Player visor toggle (Z key).
uint64_t DetourToggle(void* a, void* b, void* c, void* d)
{
    g_lastToggleMs = NowMs();
    g_armUntilMs = 0;
    g_stickyPose = nullptr;
    t_inToggle = true;
    const uint64_t r = g_origToggle(a, b, c, d);
    t_inToggle = false;
    return r;
}

// SetElementFlag(item, flag, value): during the player's toggle, learn which items carry the visor flag.
uint64_t DetourSetFlag(void* a, void* b, void* c, void* d)
{
    if (t_inToggle && static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(b)) == kVisorFlag) {
        if (static_cast<std::uint8_t>(reinterpret_cast<std::uintptr_t>(c) & 0xFF)) HelmetAdd(a);
        else HelmetRemove(a);
    }
    return g_origSetFlag(a, b, c, d);
}

// Clothing manager AddItem(mgr, item).
uint64_t DetourAdd(void* a, void* b, void* c, void* d)
{
    const uint64_t r = g_origAdd(a, b, c, d);
    if (b != nullptr && HelmetKnown(b)) {
        const long long flags = SafeU32(b, 0x60);
        if (flags >= 0 && (flags & kVisorFlag) && IsHelmetType(b)) {
            g_armUntilMs = NowMs() + kArmWindowMs;
        }
    }
    return r;
}

// C_VisorPoseModifier::Execute(this, ...), runs every frame for every modifier.
uint64_t DetourExec(void* a, void* b, void* c, void* d)
{
    if (a != nullptr) {
        const long long now = NowMs();

        // Keep the forced modifier closed. Ends when it stops running (dialogue over) or on a new Z press.
        if (a == g_stickyPose.load()) {
            if (now - g_stickyLastMs.load() > kIdleMs || g_lastToggleMs.load() > g_stickyStartMs.load()) {
                g_stickyPose = nullptr;
            } else {
                WritePoseClosed(a);
                g_stickyLastMs = now;
            }
        }

        float w = 0.0f;
        int joint = -1, en = 0, tgt = 0;
        if (ReadPose(a, &w, &joint, &en, &tgt) && joint != -1) {
            bool isNew = false;
            {
                std::lock_guard<std::mutex> lock(g_poseMutex);
                PoseSeen* slot = nullptr;
                PoseSeen* oldest = &g_pose[0];
                for (auto& e : g_pose) {
                    if (e.ptr == reinterpret_cast<std::uintptr_t>(a)) { slot = &e; break; }
                    if (e.last < oldest->last) oldest = &e;
                }
                if (!slot) {                       // not seen: take the least recently used slot
                    slot = oldest;
                    slot->ptr = reinterpret_cast<std::uintptr_t>(a);
                    isNew = true;
                } else if (now - slot->last > kIdleMs) {
                    isNew = true;                  // same address, but it has been idle: a new object
                }
                slot->last = now;
            }
            // A brand new modifier right after the player's closed helmet was re-added: make it closed.
            if (isNew && g_armUntilMs.load() > now && en == 0 && tgt == 0) {
                if (WritePoseClosed(a)) {
                    g_armUntilMs = 0;
                    g_stickyPose = a;
                    g_stickyStartMs = now;
                    g_stickyLastMs = now;
                    DVDS_LOG("[DVDs] closed visor kept for dialogue modifier %p (joint %d)", a, joint);
                }
            }
        }
    }
    return g_origExec(a, b, c, d);
}

bool Hook(const char* name, std::size_t rva, void* detour, Fn* orig)
{
    void* target = reinterpret_cast<void*>(REL::Offset(rva).address());
    MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void**>(orig));
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        DVDS_LOG("[DVDs] hook %s failed: %s", name, MH_StatusToString(s));
        return false;
    }
    return true;
}

// Reads the PE header of the loaded WHGame.dll and returns the matching entry of kBuilds, or nullptr.
const BuildInfo* DetectBuild(std::uint32_t* timestamp, std::uint32_t* imageSize)
{
    *timestamp = 0;
    *imageSize = 0;
    __try {
        const auto* base = reinterpret_cast<const std::uint8_t*>(REL::Module::get().base());
        const std::uint32_t nt = *reinterpret_cast<const std::uint32_t*>(base + 0x3C);
        if (*reinterpret_cast<const std::uint16_t*>(base) != 0x5A4D) return nullptr;        // "MZ"
        if (*reinterpret_cast<const std::uint32_t*>(base + nt) != 0x00004550) return nullptr; // "PE\0\0"
        *timestamp = *reinterpret_cast<const std::uint32_t*>(base + nt + 8);
        *imageSize = *reinterpret_cast<const std::uint32_t*>(base + nt + 0x50);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    for (const auto& b : kBuilds) {
        if (b.timestamp == *timestamp && b.imageSize == *imageSize) return &b;
    }
    return nullptr;
}

bool InstallHooks()
{
    if (MH_Initialize() != MH_OK) {
        DVDS_LOG("[DVDs] MH_Initialize failed");
        return false;
    }
    const bool ok =
        Hook("Toggle",   g_build->toggle,     reinterpret_cast<void*>(&DetourToggle),  &g_origToggle) &&
        Hook("SetFlag",  g_build->setFlag,    reinterpret_cast<void*>(&DetourSetFlag), &g_origSetFlag) &&
        Hook("AddItem",  g_build->addItem,    reinterpret_cast<void*>(&DetourAdd),     &g_origAdd) &&
        Hook("PoseExec", g_build->poseExec,   reinterpret_cast<void*>(&DetourExec),    &g_origExec);
    DVDS_LOG(ok ? "[DVDs] all hooks installed" : "[DVDs] not all hooks installed, mod inactive");
    return ok;
}

// Forget everything tied to the previous game session (item pointers are only valid within one load).
void OnMessage(KCSE::Message* msg)
{
    if (msg->type == KCSE::IMessagingInterface::kMessage_LoadGame ||
        msg->type == KCSE::IMessagingInterface::kMessage_NewGame) {
        {
            std::lock_guard<std::mutex> lock(g_helmetMutex);
            for (auto& h : g_helmets) h = nullptr;
        }
        g_armUntilMs = 0;
        g_stickyPose = nullptr;
    }
}

}  // namespace

KCSE_PLUGIN_INFO("DVDs", "Knitby", 16);

KCSE_PLUGIN_LOAD(kcse)
{
    if (static_cast<uint32_t>(kcse->GetGameVersion()) != kGameVersion) {
        DVDS_LOG("[DVDs] game version 0x%X is not 1.5.6, mod inactive",
               static_cast<unsigned>(kcse->GetGameVersion()));
        return true;
    }
    std::uint32_t timestamp = 0, imageSize = 0;
    g_build = DetectBuild(&timestamp, &imageSize);
    if (!g_build) {
        DVDS_LOG("[DVDs] unsupported game build (WHGame.dll timestamp=0x%08X size=0x%08X), mod inactive",
                 timestamp, imageSize);
        return true;
    }
    DVDS_LOG("[DVDs] game build recognised: %s", g_build->name);
    kcse->GetMessagingInterface()->RegisterListener(&OnMessage);
    return InstallHooks();
}
