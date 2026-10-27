// ==WindhawkMod==
// @id              cursor-idle-tolerance
// @name            Cursor Auto-Hide with Movement Tolerance
// @description     Hide Windows system cursors after mouse inactivity; reveal by movement, mouse-button or wheel input, with recovery safeguards
// @version         1.0.4
// @author          xcloudx01
// @github          https://github.com/xcloudx01
// @license         GPL-3.0-or-later
// @include         windhawk.exe
// @compilerOptions -luser32 -lshell32
// ==/WindhawkMod==

// SPDX-License-Identifier: GPL-3.0-or-later
// Uses the dedicated-process Windhawk tool bootstrap and system-cursor approach
// documented by ramensoftware, and inspired by cursor-auto-hide (nikitor32).
// Original: https://github.com/ramensoftware/windhawk-mods/blob/main/mods/cursor-auto-hide.wh.cpp
// Tool bootstrap: https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process

// ==WindhawkModReadme==
/*
# Cursor Auto-Hide with Movement Tolerance

Automatically hides Windows system cursors after a configurable period without
mouse movement or button/wheel activity. This is a system-wide cursor replacement,
not a per-application overlay.

## Behavior

- The cursor is hidden after the configured inactivity delay.
- Once hidden, it reappears when its physical screen position moves **farther
  than** the configured distance from where it was hidden.
- As a non-disableable safety fallback, it also reappears after **100 physical
  pixels of cumulative pointer travel**, even if the movement ends back where
  it started. A separate **512 raw-device-unit** fallback covers relative-input
  mice while the pointer is locked or repeatedly recentered by a game.
- Mouse buttons and the mouse wheel reveal it without needing pointer movement.
- Keyboard input alone does not reset the timer or reveal the cursor (except
  for the emergency restore shortcut).

## Settings

- **Hide delay (milliseconds):** Default **1000** (one second). Allowed range:
  1 to 3600000. A value of zero is invalid; the mod corrects legacy zero
  values to the 1000 ms default.
- **Reveal distance (physical pixels):** Default **8**, allowed range 0 to 500.
  Distance is measured from the position at which the cursor was hidden; it
  is not a per-event movement threshold. Setting zero reveals on any detected
  position change. **Above 100**, the cumulative movement safety fallback can
  reveal the cursor before the configured displacement is reached.
- **Extra emergency function key:** Default **0** (disabled). Set 1 to 11 for
  another shortcut **Ctrl+Alt+Shift+F1** through **Ctrl+Alt+Shift+F11**. The
  built-in Ctrl+Alt+Shift+F12 shortcut is always retained (where available).

## Compatibility

This mod replaces Windows-provided system cursor shapes. It **cannot guarantee
hiding** cursors drawn by applications themselves, including custom game
cursors. Some games lock or recenter the pointer, so position-based revealing
may not work as expected while they capture mouse input. Raw relative mouse
movement can still reveal the Windows cursor after sufficient raw device travel,
but it cannot force an application-drawn game cursor to appear or disappear.

## Recovery

Press **Ctrl+Alt+Shift+F12** for an emergency restore, or use the configured
additional shortcut. After that, the cursor stays visible until the next
detected mouse movement or mouse-button/wheel activity; ordinary auto-hide then
resumes. A registered emergency shortcut is required for hiding: if Windows
rejects both shortcut registrations, the mod leaves the cursor visible. These
shortcuts cannot operate on the secure desktop.

Stopping or disabling the mod attempts to restore the currently configured
Windows cursor scheme. An independent launcher also attempts recovery if the
worker process exits unexpectedly. These are best-effort protections, not an
absolute guarantee against every process, system, or desktop failure.

The mod does not intentionally modify saved cursor-scheme registry settings.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- idleMilliseconds: 1000
  $name: Hide delay (milliseconds)
  $description: >-
    Mouse inactivity before hiding. Default: 1000 ms (one second).
    Allowed range: 1 to 3600000 ms. A value of zero is invalid.
  #! $min: 1
  #! $max: 3600000
- movementTolerancePx: 8
  $name: Reveal distance (physical pixels)
  $description: >-
    Distance from the hidden position needed to reveal the cursor.
    Set 0 to reveal on any detected position change. Mouse-button
    and wheel input always reveal. Above 100 px, the automatic
    100-pixel cumulative-travel safety limit can reveal it sooner.
    Allowed range: 0 to 500.
  #! $min: 0
  #! $max: 500
- extraRescueFunctionKey: 0
  $name: Additional emergency shortcut (function key)
  $description: >-
    0 = disabled (default). 1-11 = Ctrl+Alt+Shift+F1 through
    Ctrl+Alt+Shift+F11. The original Ctrl+Alt+Shift+F12 recovery
    shortcut always remains active when Windows permits registration.
    If neither shortcut registers, the mod refuses to hide cursors.
  #! $min: 0
  #! $max: 11
*/
// ==/WindhawkModSettings==

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cwchar>
#include <vector>
// Embedded so this source can be pasted into Windhawk as ONE file.


// This file has no Windows dependency; its transitions are exercised by tests.
namespace cursor_idle {

enum class Action { None, Hide, Restore };

// Safety limits are intentionally not user-configurable.
static constexpr double kMaxCumulativePhysicalPx = 100.0;
static constexpr std::uint64_t kMaxCumulativeRawUnits = 512;

struct Settings {
    std::uint64_t idleMs = 1000;
    std::int64_t tolerancePx = 8;
};

struct Input {
    std::uint64_t now = 0;
    bool positionValid = true;
    std::int64_t x = 0;
    std::int64_t y = 0;
    bool mouseButtonOrWheel = false;
    bool mouseButtonHeld = false;
    bool emergencyRestore = false;
    bool rescueAvailable = true;
    std::uint64_t rawMotionUnits = 0;
};

struct State {
    bool hidden = false;
    bool suspended = false;
    bool havePosition = false;
    std::int64_t lastX = 0;
    std::int64_t lastY = 0;
    std::int64_t hideX = 0;
    std::int64_t hideY = 0;
    double cumulativePhysicalPx = 0.0;
    std::uint64_t cumulativeRawUnits = 0;
    std::uint64_t lastActivity = 0;
};

inline Action Step(State& s, const Settings& cfg, const Input& in) {
    // Fail open if cursor observation becomes unreliable (locked/UAC desktop).
    if (!in.positionValid) {
        const bool wasHidden = s.hidden;
        s.hidden = false;
        s.suspended = true;
        s.havePosition = false;
        s.lastActivity = in.now;
        return wasHidden ? Action::Restore : Action::None;
    }

    if (!s.havePosition) {
        s.havePosition = true;
        s.lastX = in.x;
        s.lastY = in.y;
        s.lastActivity = in.now;
    }

    const long double stepX = (long double)in.x - (long double)s.lastX;
    const long double stepY = (long double)in.y - (long double)s.lastY;
    const bool moved = stepX != 0 || stepY != 0;
    // Add each observed segment, not the net distance from the hide point.
    // Also count unaccelerated raw mouse motion for captured/recentered cursors.
    if (s.hidden) {
        s.cumulativePhysicalPx += (double)std::sqrt(stepX * stepX + stepY * stepY);
        const auto remaining = kMaxCumulativeRawUnits > s.cumulativeRawUnits
            ? kMaxCumulativeRawUnits - s.cumulativeRawUnits : 0;
        s.cumulativeRawUnits += in.rawMotionUnits > remaining ? remaining : in.rawMotionUnits;
    }
    s.lastX = in.x;
    s.lastY = in.y;

    // No recoverable global hotkey: never leave the system cursors hidden.
    if (!in.rescueAvailable) {
        const bool wasHidden = s.hidden;
        s.hidden = false;
        s.suspended = true;
        s.lastActivity = in.now;
        return wasHidden ? Action::Restore : Action::None;
    }

    if (in.emergencyRestore) {
        const bool wasHidden = s.hidden;
        s.hidden = false;
        s.suspended = true; // Remain visible until next physical mouse input.
        s.lastActivity = in.now;
        return wasHidden ? Action::Restore : Action::None;
    }

    // Clicks/wheel always restore, independently of displacement threshold.
    if (in.mouseButtonOrWheel || in.mouseButtonHeld) {
        const bool wasHidden = s.hidden;
        s.hidden = false;
        s.suspended = false;
        s.lastActivity = in.now;
        return wasHidden ? Action::Restore : Action::None;
    }

    // Re-evaluate displacement even without a fresh movement, so lowering
    // the configured threshold reveals on the next poll.
    if (s.hidden) {
        const long double dx = (long double)in.x - (long double)s.hideX;
        const long double dy = (long double)in.y - (long double)s.hideY;
        const long double t = (long double)cfg.tolerancePx;
        if (dx * dx + dy * dy > t * t ||
            s.cumulativePhysicalPx >= kMaxCumulativePhysicalPx ||
            s.cumulativeRawUnits >= kMaxCumulativeRawUnits) {
            s.hidden = false;
            s.lastActivity = in.now;
            return Action::Restore;
        }
    }

    if (moved || in.rawMotionUnits != 0) {
        if (s.suspended) s.suspended = false;
        if (!s.hidden) s.lastActivity = in.now;
    }

    if (s.hidden || s.suspended) {
        return Action::None;
    }

    // Monotonic tick count; reset on unexpected backwards observation.
    if (in.now < s.lastActivity) {
        s.lastActivity = in.now;
        return Action::None;
    }
    if (in.now - s.lastActivity >= (cfg.idleMs == 0 ? 1 : cfg.idleMs)) {
        s.hidden = true;
        s.hideX = in.x;
        s.hideY = in.y;
        s.cumulativePhysicalPx = 0;
        s.cumulativeRawUnits = 0;
        return Action::Hide;
    }
    return Action::None;
}

} // namespace cursor_idle

// Persisted by Windhawk per-mod storage (shared by launcher/tool process).
static constexpr PCWSTR kModifiedKey = L"modifiedV1";
static constexpr PCWSTR kOwnerPidKey = L"ownerPidV1";
static constexpr int kRescueHotkeyId = 0x7341;
static constexpr int kExtraRescueHotkeyId = 0x7342;

static std::atomic<int> g_idleMilliseconds{1000};
static std::atomic<int> g_tolerancePx{8};
static std::atomic<int> g_extraRescueFunctionKey{0};
static HANDLE g_workerStop = nullptr;
static HANDLE g_workerThread = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevExceptionHandler = nullptr;

static cursor_idle::State g_state; // worker-thread-owned

static const DWORD kCursorIds[] = {
    32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644,
    32645, 32646, 32648, 32649, 32650, 32651, 32631, 32671, 32672
};

static void ReadSettings() {
    int delayMs = Wh_GetIntSetting(L"idleMilliseconds");
    int tolerance = Wh_GetIntSetting(L"movementTolerancePx");
    int rescueKey = Wh_GetIntSetting(L"extraRescueFunctionKey");
    // Windhawk 1.x ignores the #! $min/$max editor hints. Validate at runtime.
    // Older installations without a settings block may have saved zero;
    // interpret that invalid value as the documented 1000 ms default.
    if (delayMs == 0) delayMs = 1000;
    if (delayMs < 0) delayMs = 1;
    if (delayMs > 3600000) delayMs = 3600000;
    if (tolerance < 0) tolerance = 0;
    if (tolerance > 500) tolerance = 500;
    if (rescueKey < 0 || rescueKey > 11) rescueKey = 0;
    if (tolerance > 100) {
        Wh_Log(L"Reveal distance %d px: 100 px cumulative and 512 raw-unit safety fallbacks remain enabled", tolerance);
    }
    g_idleMilliseconds.store(delayMs);
    g_tolerancePx.store(tolerance);
    g_extraRescueFunctionKey.store(rescueKey);
}

// Reloads the current user cursor scheme; never changes registry cursor values.
// If restoration cannot run (e.g. secure desktop), leave marker set for retry.
struct CursorBackup { DWORD id; HCURSOR cursor; };
static std::vector<CursorBackup> g_originalCursors;

static void FreeCursorBackups() {
    for (auto& b : g_originalCursors) if (b.cursor) DestroyCursor(b.cursor);
    g_originalCursors.clear();
}

// Save the current scheme before altering it. Used by the worker for an
// immediate on-screen restore, and independently by the guardian on child death.
static bool SaveOriginalCursors() {
    FreeCursorBackups();
    for (DWORD id : kCursorIds) {
        HCURSOR loaded = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        HCURSOR copy = loaded ? CopyCursor(loaded) : nullptr;
        if (!copy) {
            Wh_Log(L"Cannot save system cursor id %lu", id);
            FreeCursorBackups();
            return false; // Incomplete rollback means we must not hide.
        }
        g_originalCursors.push_back({id, copy});
    }
    return true;
}

static bool RestoreSystemCursors() {
    if (!Wh_GetIntValue(kModifiedKey, 0) && g_originalCursors.empty()) {
        return true;
    }
    // SPI_SETCURSORS alone may not immediately update a cursor held in a
    // window's active SetCursor slot. Restore a copy first, then reload the
    // user's saved scheme for animated/multi-size cursor fidelity.
    for (auto& b : g_originalCursors) {
        HCURSOR copy = CopyCursor(b.cursor);
        if (copy && !SetSystemCursor(copy, b.id)) DestroyCursor(copy);
    }
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) {
        Wh_Log(L"SPI_SETCURSORS failed: %lu; recovery remains pending", GetLastError());
        return false;
    }
    FreeCursorBackups();
    if (!Wh_SetIntValue(kModifiedKey, 0)) {
        Wh_Log(L"Recovered cursors but unable to clear durable marker");
        return false;
    }
    Wh_Log(L"System cursors restored");
    return true;
}

// Blank monochrome 32x32 cursor (AND=all 1, XOR=all 0).
static HCURSOR MakeBlankCursor() {
    BYTE andMask[128];
    BYTE xorMask[128]{};
    for (BYTE& b : andMask) b = 0xff;
    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32,
                        andMask, xorMask);
}

static bool HideSystemCursors() {
    // Never take another snapshot while a previous recovery is pending.
    if (Wh_GetIntValue(kModifiedKey, 0)) return false;
    if (!SaveOriginalCursors()) return false;
    // If we cannot remember a successful mutation, we must not mutate.
    if (!Wh_SetIntValue(kModifiedKey, 1)) {
        Wh_Log(L"Could not set recovery marker; refusing to hide");
        FreeCursorBackups();
        return false;
    }

    HCURSOR blank = MakeBlankCursor();
    if (!blank) {
        Wh_Log(L"Failed to construct blank cursor");
        RestoreSystemCursors();
        return false;
    }

    bool allOk = true;
    for (DWORD id : kCursorIds) {
        // SetSystemCursor consumes its argument only on success.
        HCURSOR copy = CopyCursor(blank);
        if (!copy || !SetSystemCursor(copy, id)) {
            if (copy) DestroyCursor(copy);
            allOk = false;
            Wh_Log(L"SetSystemCursor failed for cursor id %lu: %lu", id, GetLastError());
            break;
        }
    }
    DestroyCursor(blank);

    if (!allOk) {
        RestoreSystemCursors(); // Undo even a partially completed replacement.
        return false;
    }
    Wh_Log(L"System cursors hidden");
    return true;
}

static LONG WINAPI CrashRecovery(EXCEPTION_POINTERS* info) {
    RestoreSystemCursors(); // Best effort; launcher is an independent fallback.
    return g_prevExceptionHandler ? g_prevExceptionHandler(info)
                                  : EXCEPTION_CONTINUE_SEARCH;
}

static bool MouseHeld() {
    static const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                VK_XBUTTON1, VK_XBUTTON2 };
    for (int vk : keys) if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

// Secure desktop detection: compare the name of the input desktop to the
// desktop owning this thread. If access/name lookup fails, fail open.
static bool OnCurrentInputDesktop() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    HDESK self = GetThreadDesktop(GetCurrentThreadId());
    WCHAR inputName[256]{};
    WCHAR selfName[256]{};
    DWORD ignored = 0;
    const bool valid = self &&
        GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &ignored) &&
        GetUserObjectInformationW(self, UOI_NAME, selfName, sizeof(selfName), &ignored) &&
        _wcsicmp(inputName, selfName) == 0;
    CloseDesktop(input);
    return valid;
}

static bool ReadMouseEvents(MSG* msg, bool* hadButtonOrWheel,
                            std::uint64_t* rawMotionUnits) {
    if (msg->message != WM_INPUT) return false;
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT,
                        nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
        size < sizeof(RAWINPUTHEADER) || size > 4096) {
        // Treat a malformed event as actionable input: reveal rather than
        // leave an inaccessible cursor hidden.
        *hadButtonOrWheel = true;
        return true;
    }
    std::vector<BYTE> buffer(size);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT,
                        buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        *hadButtonOrWheel = true;
        return true;
    }
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
    if (raw->header.dwType == RIM_TYPEMOUSE &&
        size < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        // A truncated mouse record cannot be trusted; reveal fail-open.
        *hadButtonOrWheel = true;
        return true;
    }
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        if (raw->data.mouse.usButtonFlags) *hadButtonOrWheel = true;
        // Absolute raw coordinates aren't physical movement deltas; only
        // accumulate relative device counts. Screen positions still work.
        if (!(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            const auto dx = (std::int64_t)raw->data.mouse.lLastX;
            const auto dy = (std::int64_t)raw->data.mouse.lLastY;
            const auto ax = (std::uint64_t)(dx < 0 ? -dx : dx);
            const auto ay = (std::uint64_t)(dy < 0 ? -dy : dy);
            const auto motion = ax > ay ? ax : ay;
            const auto left = cursor_idle::kMaxCumulativeRawUnits > *rawMotionUnits
                ? cursor_idle::kMaxCumulativeRawUnits - *rawMotionUnits : 0;
            *rawMotionUnits += motion > left ? left : motion;
        }
    }
    return true;
}

static DWORD WINAPI WorkerThread(LPVOID) {
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) {
        Wh_Log(L"Worker window creation failed: %lu", GetLastError());
        ExitProcess(1); // Guardian notices worker death and restores if necessary.
    }
    RAWINPUTDEVICE dev{};
    dev.usUsagePage = 1;
    dev.usUsage = 2;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = window;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(dev))) {
        Wh_Log(L"Raw input registration failed: %lu", GetLastError());
        DestroyWindow(window);
        ExitProcess(1);
    }

    bool rescueRegistered = RegisterHotKey(window, kRescueHotkeyId,
                                            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                            VK_F12) != 0;
    if (!rescueRegistered) {
        Wh_Log(L"Built-in emergency shortcut unavailable: %lu", GetLastError());
    }
    // The built-in F12 rescue can never be disabled by settings. An optional
    // alternative F1-F11 can be registered alongside it.
    int attemptedExtraKey = -1;
    bool extraRescueRegistered = false;
    g_state.lastActivity = GetTickCount64();
    for (;;) {
        DWORD waitMs = g_state.hidden ? 20 : 80;
        if (!g_state.hidden && !g_state.suspended && g_state.havePosition) {
            ULONGLONG now = GetTickCount64();
            ULONGLONG elapsed = now >= g_state.lastActivity
                ? now - g_state.lastActivity : 0;
            ULONGLONG idleMs = (ULONGLONG)g_idleMilliseconds.load();
            if (idleMs < 1 || idleMs > 3600000) idleMs = 1;
            ULONGLONG left = elapsed >= idleMs ? 1 : idleMs - elapsed;
            if (left < waitMs) waitMs = (DWORD)left;
        }
        DWORD wait = MsgWaitForMultipleObjects(1, &g_workerStop, FALSE,
                                              waitMs, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;

        const int wantedExtra = g_extraRescueFunctionKey.load();
        if (wantedExtra != attemptedExtraKey) {
            if (extraRescueRegistered) {
                UnregisterHotKey(window, kExtraRescueHotkeyId);
                extraRescueRegistered = false;
            }
            attemptedExtraKey = wantedExtra;
            if (wantedExtra >= 1 && wantedExtra <= 11) {
                extraRescueRegistered = RegisterHotKey(
                    window, kExtraRescueHotkeyId,
                    MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                    VK_F1 + wantedExtra - 1) != 0;
                if (!extraRescueRegistered) {
                    Wh_Log(L"Additional rescue F%d registration failed: %lu",
                           wantedExtra, GetLastError());
                }
            }
        }

        bool clickedOrScrolled = false;
        std::uint64_t rawMotionUnits = 0;
        bool emergency = false;
        MSG message{};
        // Bound the batch so a high-polling-rate mouse cannot indefinitely
        // postpone state updates, stop signals, or emergency restoration.
        for (int count = 0;
             count < 512 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
             ++count) {
            if (message.message == WM_INPUT) {
                ReadMouseEvents(&message, &clickedOrScrolled, &rawMotionUnits);
            }
            if (message.message == WM_HOTKEY &&
                (message.wParam == kRescueHotkeyId ||
                 message.wParam == kExtraRescueHotkeyId)) {
                emergency = true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        POINT pt{};
        bool valid = OnCurrentInputDesktop() && GetPhysicalCursorPos(&pt);
        cursor_idle::Input in{};
        in.now = GetTickCount64();
        in.positionValid = valid;
        in.x = pt.x;
        in.y = pt.y;
        in.mouseButtonOrWheel = clickedOrScrolled;
        in.mouseButtonHeld = MouseHeld();
        in.emergencyRestore = emergency;
        in.rescueAvailable = rescueRegistered || extraRescueRegistered;
        in.rawMotionUnits = rawMotionUnits;

        cursor_idle::Settings settings{};
        settings.idleMs = (std::uint64_t)g_idleMilliseconds.load();
        settings.tolerancePx = g_tolerancePx.load();

        // Even if the pure state says 'visible', a previous restoration
        // may have failed. Retry until the durable marker can be cleared.
        if (!g_state.hidden && Wh_GetIntValue(kModifiedKey, 0)) {
            RestoreSystemCursors();
        }

        cursor_idle::Action action = cursor_idle::Step(g_state, settings, in);
        if (action == cursor_idle::Action::Hide) {
            if (!HideSystemCursors()) {
                g_state.hidden = false;
                g_state.suspended = true; // Never spin retrying failed hiding.
            }
        } else if (action == cursor_idle::Action::Restore) {
            RestoreSystemCursors();
        }
    }
    if (rescueRegistered) UnregisterHotKey(window, kRescueHotkeyId);
    if (extraRescueRegistered) UnregisterHotKey(window, kExtraRescueHotkeyId);
    RAWINPUTDEVICE removeDevice = dev;
    removeDevice.dwFlags = RIDEV_REMOVE;
    removeDevice.hwndTarget = nullptr;
    RegisterRawInputDevices(&removeDevice, 1, sizeof(removeDevice));
    RestoreSystemCursors();
    DestroyWindow(window);
    return 0;
}

static BOOL WhTool_ModInit() {
    ReadSettings();
    if (Wh_GetIntValue(kModifiedKey, 0)) {
        // Mutex below ensures an earlier worker is not still running.
        if (!RestoreSystemCursors()) {
            Wh_Log(L"Old cursors could not be recovered yet; refusing start");
            return FALSE;
        }
    }
    if (!Wh_SetIntValue(kOwnerPidKey, static_cast<int>(GetCurrentProcessId()))) {
        return FALSE;
    }
    g_prevExceptionHandler = SetUnhandledExceptionFilter(CrashRecovery);
    g_workerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_workerStop) {
        Wh_SetIntValue(kOwnerPidKey, 0);
        SetUnhandledExceptionFilter(g_prevExceptionHandler);
        return FALSE;
    }
    g_workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!g_workerThread) {
        CloseHandle(g_workerStop);
        g_workerStop = nullptr;
        Wh_SetIntValue(kOwnerPidKey, 0);
        SetUnhandledExceptionFilter(g_prevExceptionHandler);
        return FALSE;
    }
    return TRUE;
}

static void WhTool_ModSettingsChanged() { ReadSettings(); }

static void WhTool_ModUninit() {
    if (g_workerStop) SetEvent(g_workerStop);
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, INFINITE);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_workerStop) {
        CloseHandle(g_workerStop);
        g_workerStop = nullptr;
    }
    bool restored = RestoreSystemCursors();
    // Keep the original PID if restoration was unsuccessful; the independent
    // launcher can then retry instead of losing track of the failed owner.
    if (restored) Wh_SetIntValue(kOwnerPidKey, 0);
    SetUnhandledExceptionFilter(g_prevExceptionHandler);
}

// ---- Windhawk tool-mod bootstrap, with independent launcher-side guardian. ----
// Source pattern: ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
static bool g_launcher = false;
static HANDLE g_toolMutex = nullptr;
static HANDLE g_guardStop = nullptr;
static HANDLE g_guardThread = nullptr;
static HANDLE g_childProcess = nullptr;
static DWORD g_childPid = 0;

static DWORD WINAPI GuardThread(LPVOID) {
    HANDLE waits[] = { g_guardStop, g_childProcess };
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
        return 0; // Launcher is being unloaded normally.
    }
    // A forcibly terminated child cannot run C++ clean-up. The independent
    // launcher restores its cursors. Only touch the matching child PID:
    // another tool instance must never be reset by a failed duplicate launch.
    Wh_Log(L"Cursor tool process %lu exited; checking recovery marker", g_childPid);
    while (WaitForSingleObject(g_guardStop, 1000) == WAIT_TIMEOUT) {
        if (Wh_GetIntValue(kOwnerPidKey, 0) != (int)g_childPid ||
            !Wh_GetIntValue(kModifiedKey, 0)) {
            break;
        }
        if (RestoreSystemCursors()) break;
    }
    return 0;
}

static void WINAPI EntryPoint_Hook() { ExitThread(0); }

BOOL Wh_ModInit() {
    DWORD sessionId = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) && sessionId == 0) {
        return FALSE;
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return FALSE;
    bool excluded = false, toolProcess = false, thisTool = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"-service") ||
            !wcscmp(argv[i], L"-service-start") ||
            !wcscmp(argv[i], L"-service-stop")) {
            excluded = true;
            break;
        }
        if (!wcscmp(argv[i], L"-tool-mod") && i + 1 < argc) {
            toolProcess = true;
            thisTool = !wcscmp(argv[i + 1], WH_MOD_ID);
        }
    }
    LocalFree(argv);
    if (excluded) return FALSE;

    if (thisTool) {
        g_toolMutex = CreateMutexW(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolMutex) ExitProcess(1);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(g_toolMutex);
            g_toolMutex = nullptr;
            ExitProcess(1); // Never start a second instance or a stray UI.
        }
        if (!WhTool_ModInit()) ExitProcess(1);
        // Prevent the Windhawk command-line tool process from starting a UI.
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(dos) + dos->e_lfanew);
        void* entry = reinterpret_cast<BYTE*>(dos) + nt->OptionalHeader.AddressOfEntryPoint;
        if (!Wh_SetFunctionHook(entry, reinterpret_cast<void*>(EntryPoint_Hook), nullptr)) {
            WhTool_ModUninit();
            ExitProcess(1);
        }
        return TRUE;
    }

    if (toolProcess) return FALSE; // Another mod's dedicated tool process.
    g_launcher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_launcher) return;
    WCHAR exe[MAX_PATH]{};
    DWORD count = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (!count || count >= ARRAYSIZE(exe)) return;
    WCHAR command[MAX_PATH + 128]{};
    swprintf_s(command, L"\"%s\" -tool-mod \"%s\"", exe, WH_MOD_ID);

    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase) kernelbase = GetModuleHandleW(L"kernel32.dll");
    if (!kernelbase) return;
    using CreateProcessInternalW_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, WINBOOL, DWORD, LPVOID,
        LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);
    auto createInternal = reinterpret_cast<CreateProcessInternalW_t>(
        GetProcAddress(kernelbase, "CreateProcessInternalW"));
    if (!createInternal) return;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi{};
    // The guardian can restore its own saved copies even if the worker dies
    // before it can release its cursor handles. Scheme reload remains fallback.
    if (!SaveOriginalCursors()) {
        Wh_Log(L"Guardian cursor snapshot unavailable; relying on scheme reload");
    }
    if (!createInternal(nullptr, exe, command, nullptr, nullptr, FALSE,
                        NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"Unable to start tool: %lu", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    g_childProcess = pi.hProcess;
    g_childPid = pi.dwProcessId;
    g_guardStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_guardStop) {
        g_guardThread = CreateThread(nullptr, 0, GuardThread, nullptr, 0, nullptr);
        if (!g_guardThread) {
            Wh_Log(L"Guardian thread unavailable; worker still has recovery");
        }
    }
}

void Wh_ModSettingsChanged() {
    if (!g_launcher) WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_launcher) {
        if (g_guardStop) SetEvent(g_guardStop);
        if (g_guardThread) {
            WaitForSingleObject(g_guardThread, INFINITE);
            CloseHandle(g_guardThread);
            g_guardThread = nullptr;
        }
        if (g_guardStop) CloseHandle(g_guardStop);
        if (g_childProcess) CloseHandle(g_childProcess);
        g_guardStop = g_childProcess = nullptr;
        FreeCursorBackups();
        return;
    }
    WhTool_ModUninit();
    ExitProcess(0);
}