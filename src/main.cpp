#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <uxtheme.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commctrl.h>
#include <xinput.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kClassName[] = L"RBRDS4RumbleWindow";
constexpr wchar_t kAppName[] = L"RBR DS4 Rumble";
constexpr UINT WM_TELEMETRY = WM_APP + 1;
constexpr COLORREF kBg = RGB(12, 15, 22);
constexpr COLORREF kPanel = RGB(23, 28, 39);
constexpr COLORREF kPanel2 = RGB(31, 38, 52);
constexpr COLORREF kText = RGB(237, 242, 249);
constexpr COLORREF kMuted = RGB(143, 157, 178);
constexpr COLORREF kAccent = RGB(50, 210, 184);
constexpr COLORREF kAccent2 = RGB(83, 130, 255);
constexpr COLORREF kWarning = RGB(255, 183, 77);

enum ControlId {
    ID_START = 100, ID_TEST_HEAVY, ID_TEST_LIGHT, ID_PRESET_SMOOTH,
    ID_PRESET_GRAVEL, ID_PRESET_BRUTAL, ID_CONTROLLER, ID_PORT,
    ID_MASTER, ID_IMPACTS, ID_ROAD, ID_ENGINE, ID_SLIP, ID_SMOOTHING,
    ID_AUTOSTART, ID_MUTE_AIRBORNE
};

struct Settings {
    int master = 85;
    int impacts = 100;
    int road = 72;
    int engine = 24;
    int slip = 42;
    int smoothing = 32;
    int controller = -1;
    int port = 6776;
    bool autoStart = true;
    bool muteAirborne = true;
};

struct LiveData {
    std::atomic<float> speed{0}, rpm{0}, heave{0}, sway{0};
    std::atomic<float> damperLF{0}, damperRF{0}, damperLR{0}, damperRR{0};
    std::atomic<float> springLF{0}, springRF{0}, springLR{0}, springRR{0};
    std::atomic<DWORD> packetTick{0};
    std::atomic<unsigned> packetCount{0};
};

struct Ui {
    HWND main = nullptr, start = nullptr, telemetryDot = nullptr, controllerDot = nullptr;
    HWND statusText = nullptr, liveText = nullptr, outputText = nullptr;
    HWND controller = nullptr, port = nullptr, autoStart = nullptr, muteAirborne = nullptr;
    std::array<HWND, 6> sliders{};
    std::array<HWND, 6> valueLabels{};
};

Settings g_settings;
std::mutex g_settingsMutex;
LiveData g_live;
Ui g_ui;
std::atomic<bool> g_running{false};
std::atomic<bool> g_quit{false};
std::atomic<WORD> g_heavyOut{0}, g_lightOut{0};
std::thread g_worker;
HFONT g_font = nullptr, g_fontSmall = nullptr, g_fontTitle = nullptr, g_fontMetric = nullptr;
HBRUSH g_bgBrush = nullptr, g_panelBrush = nullptr;
HBRUSH g_editBrush = nullptr;

using XInputSetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
XInputSetStateFn pXInputSetState = nullptr;
XInputGetStateFn pXInputGetState = nullptr;
HMODULE g_xinput = nullptr;

void enableDarkControl(HWND control, bool useClassic = false) {
    SetWindowTheme(control, useClassic ? L"" : L"DarkMode_Explorer", nullptr);
}

float readFloat(const uint8_t* p, size_t offset) {
    float value = 0;
    std::memcpy(&value, p + offset, sizeof(value));
    return std::isfinite(value) ? value : 0.0f;
}

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

std::wstring iniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    return std::wstring(path) + L"RBR-DS4-Rumble.ini";
}

int readIniInt(const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"Rumble", key, fallback, iniPath().c_str());
}

void loadSettings() {
    g_settings.master = readIniInt(L"Master", 85);
    g_settings.impacts = readIniInt(L"Impacts", 100);
    g_settings.road = readIniInt(L"Road", 72);
    g_settings.engine = readIniInt(L"Engine", 24);
    g_settings.slip = readIniInt(L"Slip", 42);
    g_settings.smoothing = readIniInt(L"Smoothing", 32);
    g_settings.controller = readIniInt(L"Controller", -1);
    g_settings.port = readIniInt(L"Port", 6776);
    g_settings.autoStart = readIniInt(L"AutoStart", 1) != 0;
    g_settings.muteAirborne = readIniInt(L"MuteAirborne", 1) != 0;
}

void saveSettings() {
    const auto path = iniPath();
    auto writeInt = [&](const wchar_t* key, int value) {
        wchar_t buffer[32];
        _snwprintf_s(buffer, _TRUNCATE, L"%d", value);
        WritePrivateProfileStringW(L"Rumble", key, buffer, path.c_str());
    };
    writeInt(L"Master", g_settings.master); writeInt(L"Impacts", g_settings.impacts);
    writeInt(L"Road", g_settings.road); writeInt(L"Engine", g_settings.engine);
    writeInt(L"Slip", g_settings.slip); writeInt(L"Smoothing", g_settings.smoothing);
    writeInt(L"Controller", g_settings.controller); writeInt(L"Port", g_settings.port);
    writeInt(L"AutoStart", g_settings.autoStart ? 1 : 0);
    writeInt(L"MuteAirborne", g_settings.muteAirborne ? 1 : 0);
}

bool loadXInput() {
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (auto dll : dlls) {
        g_xinput = LoadLibraryW(dll);
        if (!g_xinput) continue;
        pXInputSetState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(g_xinput, "XInputSetState"));
        pXInputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(g_xinput, "XInputGetState"));
        if (pXInputSetState && pXInputGetState) return true;
        FreeLibrary(g_xinput); g_xinput = nullptr;
    }
    return false;
}

int resolveController() {
    if (!pXInputGetState) return -1;
    int selectedController;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        selectedController = g_settings.controller;
    }
    if (selectedController >= 0 && selectedController < 4) {
        XINPUT_STATE state{};
        return pXInputGetState(selectedController, &state) == ERROR_SUCCESS ? selectedController : -1;
    }
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE state{};
        if (pXInputGetState(i, &state) == ERROR_SUCCESS) return static_cast<int>(i);
    }
    return -1;
}

void setRumble(WORD heavy, WORD light) {
    if (!pXInputSetState) return;
    const int slot = resolveController();
    if (slot < 0) return;
    XINPUT_VIBRATION vibration{heavy, light};
    pXInputSetState(static_cast<DWORD>(slot), &vibration);
}

void stopRumble() {
    g_heavyOut = 0; g_lightOut = 0;
    setRumble(0, 0);
}

void parsePacket(const uint8_t* data, int length) {
    if (length < 664) return;
    g_live.speed = std::fabs(readFloat(data, 60));
    g_live.rpm = std::max(0.0f, readFloat(data, 136));
    g_live.heave = readFloat(data, 120);
    g_live.sway = readFloat(data, 116);
    g_live.damperLF = readFloat(data, 180);
    g_live.damperRF = readFloat(data, 308);
    g_live.damperLR = readFloat(data, 436);
    g_live.damperRR = readFloat(data, 564);
    g_live.springLF = readFloat(data, 152);
    g_live.springRF = readFloat(data, 280);
    g_live.springLR = readFloat(data, 408);
    g_live.springRR = readFloat(data, 536);
    g_live.packetTick = GetTickCount();
    ++g_live.packetCount;
}

void workerLoop(int port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_running = false; PostMessageW(g_ui.main, WM_TELEMETRY, 2, 0); return;
    }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup(); g_running = false; PostMessageW(g_ui.main, WM_TELEMETRY, 2, 0); return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const int socketError = WSAGetLastError();
        closesocket(sock); WSACleanup(); g_running = false;
        PostMessageW(g_ui.main, WM_TELEMETRY, 3, socketError); return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    timeBeginPeriod(1);
    PostMessageW(g_ui.main, WM_TELEMETRY, 1, 0);

    std::array<uint8_t, 2048> packet{};
    float previousHeave = 0, smoothedHeavy = 0, smoothedLight = 0;
    std::array<float, 4> loadedSpringReference{};
    int airborneFrames = 0;
    DWORD lastRumbleTick = 0;
    while (g_running && !g_quit) {
        sockaddr_in from{}; int fromLen = sizeof(from);
        int received;
        do {
            fromLen = sizeof(from);
            received = recvfrom(sock, reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received >= 664) parsePacket(packet.data(), received);
        } while (received >= 664);

        const DWORD now = GetTickCount();
        const bool fresh = now - g_live.packetTick.load() < 350;
        if (fresh && now - lastRumbleTick >= 10) {
            Settings settings;
            {
                std::lock_guard<std::mutex> lock(g_settingsMutex);
                settings = g_settings;
            }
            const float rpm = g_live.rpm.load();
            const float speed = g_live.speed.load();
            const float heave = g_live.heave.load();
            const float sway = g_live.sway.load();
            const float d1 = std::fabs(g_live.damperLF.load());
            const float d2 = std::fabs(g_live.damperRF.load());
            const float d3 = std::fabs(g_live.damperLR.load());
            const float d4 = std::fabs(g_live.damperRR.load());
            const std::array<float, 4> springDeflection{
                std::max(0.0f, g_live.springLF.load()),
                std::max(0.0f, g_live.springRF.load()),
                std::max(0.0f, g_live.springLR.load()),
                std::max(0.0f, g_live.springRR.load())
            };
            bool allSuspensionUnloaded = true;
            for (size_t i = 0; i < springDeflection.size(); ++i) {
                // Learn each wheel's current loaded travel. The reference decays so
                // one hard landing cannot make normal driving look airborne later.
                loadedSpringReference[i] = std::max(springDeflection[i], loadedSpringReference[i] * 0.997f);
                const float unloadedThreshold = std::max(0.0015f, loadedSpringReference[i] * 0.25f);
                const bool referenceReady = loadedSpringReference[i] > 0.004f;
                allSuspensionUnloaded &= referenceReady && springDeflection[i] < unloadedThreshold;
            }
            airborneFrames = allSuspensionUnloaded ? airborneFrames + 1 : 0;
            const bool airborne = airborneFrames >= 5; // 50 ms debounce at 100 Hz
            const float leftDamperAvg = (d1 + d3) * 0.5f;
            const float rightDamperAvg = (d2 + d4) * 0.5f;
            const float leftDamperPeak = std::max(d1, d3);
            const float rightDamperPeak = std::max(d2, d4);
            const float heaveJerk = std::fabs(heave - previousHeave);
            previousHeave = heave;

            const float leftRoad = clamp01((leftDamperAvg - 0.015f) / 0.42f);
            const float rightRoad = clamp01((rightDamperAvg - 0.015f) / 0.42f);
            const float leftImpact = clamp01((leftDamperPeak - 0.18f) / 1.10f + heaveJerk / 18.0f);
            const float rightImpact = clamp01((rightDamperPeak - 0.18f) / 1.10f + heaveJerk / 18.0f);
            const float engine = speed > 0.8f ? clamp01((rpm - 850.0f) / 6500.0f) : 0.0f;
            const float lateral = speed > 5.0f ? clamp01((std::fabs(sway) - 2.2f) / 8.0f) : 0.0f;
            const float pulse = 0.68f + 0.32f * std::sin(static_cast<float>(now) * (0.018f + rpm * 0.000018f));

            // The DS4's low-frequency motor is in the left grip and its high-frequency
            // motor is in the right grip. Keep 88% of each suspension cue on its own
            // side and 12% crossfeed so one-sided hits stay directional but substantial.
            const float leftRoadSpatial = leftRoad * 0.88f + rightRoad * 0.12f;
            const float rightRoadSpatial = rightRoad * 0.88f + leftRoad * 0.12f;
            const float leftImpactSpatial = leftImpact * 0.88f + rightImpact * 0.12f;
            const float rightImpactSpatial = rightImpact * 0.88f + leftImpact * 0.12f;

            float targetHeavy = leftImpactSpatial * (settings.impacts / 100.0f) * 0.92f
                              + leftRoadSpatial * (settings.road / 100.0f) * 0.35f
                              + engine * pulse * (settings.engine / 100.0f) * 0.16f
                              + lateral * (settings.slip / 100.0f) * 0.24f;
            float targetLight = rightImpactSpatial * (settings.impacts / 100.0f) * 0.72f
                              + rightRoadSpatial * (settings.road / 100.0f) * 0.92f
                              + engine * pulse * (settings.engine / 100.0f) * 0.45f
                              + lateral * (settings.slip / 100.0f) * 0.32f;
            if (settings.muteAirborne && airborne) {
                // Silence immediately in free flight; do not let smoothing leave a tail.
                targetHeavy = targetLight = 0.0f;
                smoothedHeavy = smoothedLight = 0.0f;
            }
            targetHeavy = clamp01(targetHeavy);
            targetLight = clamp01(targetLight);
            // Fast attack keeps bumps synchronized with the image. Smoothing only
            // controls the release, so it never adds perceptible input latency.
            const float release = 0.06f + (100 - settings.smoothing) * 0.0048f;
            smoothedHeavy = targetHeavy >= smoothedHeavy
                ? targetHeavy : smoothedHeavy + (targetHeavy - smoothedHeavy) * release;
            smoothedLight = targetLight >= smoothedLight
                ? targetLight : smoothedLight + (targetLight - smoothedLight) * std::min(1.0f, release * 1.7f);
            const float master = settings.master / 100.0f;
            const WORD heavy = static_cast<WORD>(clamp01(smoothedHeavy * master) * 65535.0f);
            const WORD light = static_cast<WORD>(clamp01(smoothedLight * master) * 65535.0f);
            g_heavyOut = heavy; g_lightOut = light;
            setRumble(heavy, light);
            lastRumbleTick = now;
        } else if (!fresh && (g_heavyOut || g_lightOut)) {
            smoothedHeavy = smoothedLight = 0;
            stopRumble();
        }
        Sleep(1);
    }
    stopRumble();
    timeEndPeriod(1);
    closesocket(sock); WSACleanup();
}

void startListening() {
    if (g_running) return;
    if (g_worker.joinable()) g_worker.join();
    wchar_t portText[16]{};
    GetWindowTextW(g_ui.port, portText, 16);
    int port = _wtoi(portText);
    if (port < 1 || port > 65535) port = 6776;
    g_settings.port = port;
    g_live.packetTick = 0; g_live.packetCount = 0;
    g_running = true;
    SetWindowTextW(g_ui.start, L"STOP");
    SetWindowTextW(g_ui.statusText, L"Listening for RBR telemetry…");
    g_worker = std::thread(workerLoop, port);
}

void stopListening() {
    g_running = false;
    if (g_worker.joinable()) g_worker.join();
    stopRumble();
    SetWindowTextW(g_ui.start, L"START");
    SetWindowTextW(g_ui.statusText, L"Stopped");
    InvalidateRect(g_ui.main, nullptr, FALSE);
}

HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, g_ui.main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
    return hwnd;
}

void makeSlider(int index, int id, const wchar_t* name, int value, int y) {
    makeControl(L"STATIC", name, SS_LEFT, 48, y, 110, 22, 0, g_font);
    HWND slider = makeControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS,
        165, y - 4, 315, 30, id, g_font);
    enableDarkControl(slider, true);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    g_ui.sliders[index] = slider;
    wchar_t valueText[12]; _snwprintf_s(valueText, _TRUNCATE, L"%d%%", value);
    g_ui.valueLabels[index] = makeControl(L"STATIC", valueText, SS_RIGHT,
        487, y, 52, 22, 0, g_font);
}

void updateSettingsFromUi() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    int* values[] = {&g_settings.master, &g_settings.impacts, &g_settings.road,
                     &g_settings.engine, &g_settings.slip, &g_settings.smoothing};
    for (int i = 0; i < 6; ++i) {
        *values[i] = static_cast<int>(SendMessageW(g_ui.sliders[i], TBM_GETPOS, 0, 0));
        wchar_t text[12]; _snwprintf_s(text, _TRUNCATE, L"%d%%", *values[i]);
        SetWindowTextW(g_ui.valueLabels[i], text);
    }
    g_settings.controller = static_cast<int>(SendMessageW(g_ui.controller, CB_GETCURSEL, 0, 0)) - 1;
    g_settings.autoStart = SendMessageW(g_ui.autoStart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.muteAirborne = SendMessageW(g_ui.muteAirborne, BM_GETCHECK, 0, 0) == BST_CHECKED;
    saveSettings();
}

void setPreset(int master, int impacts, int road, int engine, int slip, int smoothing) {
    int values[] = {master, impacts, road, engine, slip, smoothing};
    for (int i = 0; i < 6; ++i) SendMessageW(g_ui.sliders[i], TBM_SETPOS, TRUE, values[i]);
    updateSettingsFromUi();
}

void rumbleTest(bool heavy) {
    if (resolveController() < 0) {
        MessageBoxW(g_ui.main, L"No XInput controller found. In DS4Windows, set the output controller to Xbox 360 and connect your DS4.", kAppName, MB_ICONWARNING);
        return;
    }
    std::thread([heavy] {
        setRumble(heavy ? 52000 : 0, heavy ? 0 : 43000);
        Sleep(500); stopRumble();
    }).detach();
}

void drawRoundRect(HDC dc, RECT r, COLORREF color, int radius = 14) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}

void drawTextAt(HDC dc, const wchar_t* text, RECT r, HFONT font, COLORREF color, UINT format) {
    auto old = SelectObject(dc, font);
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &r, format);
    SelectObject(dc, old);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC screen = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    FillRect(dc, &client, g_bgBrush);
    drawRoundRect(dc, {24, 86, 566, 196}, kPanel);
    drawRoundRect(dc, {24, 212, 566, 469}, kPanel);
    drawRoundRect(dc, {24, 486, 566, 612}, kPanel);
    drawTextAt(dc, L"RBR", {28, 20, 120, 55}, g_fontTitle, kAccent, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, L"DS4 RUMBLE", {100, 20, 360, 55}, g_fontTitle, kText, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, L"NGP telemetry  →  DS4Windows XInput feedback", {30, 58, 500, 80}, g_fontSmall, kMuted, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, L"LIVE CONNECTION", {43, 101, 230, 122}, g_fontSmall, kMuted, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, L"FEEL", {43, 228, 160, 250}, g_fontSmall, kMuted, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, L"QUICK PRESETS", {43, 501, 180, 522}, g_fontSmall, kMuted, DT_LEFT | DT_SINGLELINE);

    const bool telem = g_running && GetTickCount() - g_live.packetTick.load() < 400;
    const bool controller = resolveController() >= 0;
    HBRUSH green = CreateSolidBrush(telem ? kAccent : RGB(85, 94, 110));
    HBRUSH blue = CreateSolidBrush(controller ? kAccent2 : RGB(85, 94, 110));
    SelectObject(dc, green); Ellipse(dc, 44, 134, 56, 146);
    SelectObject(dc, blue); Ellipse(dc, 44, 160, 56, 172);
    DeleteObject(green); DeleteObject(blue);
    drawTextAt(dc, telem ? L"RBR telemetry live" : L"Waiting for RBR telemetry", {65, 129, 265, 150}, g_font, telem ? kText : kMuted, DT_LEFT | DT_SINGLELINE);
    drawTextAt(dc, controller ? L"DS4Windows controller ready" : L"No XInput controller", {65, 155, 280, 178}, g_font, controller ? kText : kWarning, DT_LEFT | DT_SINGLELINE);

    BitBlt(screen, ps.rcPaint.left, ps.rcPaint.top,
        ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
        dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void setTextIfChanged(HWND control, const wchar_t* text) {
    wchar_t current[160]{};
    GetWindowTextW(control, current, static_cast<int>(std::size(current)));
    if (wcscmp(current, text) != 0) SetWindowTextW(control, text);
}

void createUi(HWND hwnd) {
    g_ui.main = hwnd;
    g_font = CreateFontW(17, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontTitle = CreateFontW(27, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontMetric = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

    g_ui.start = makeControl(L"BUTTON", L"START", BS_PUSHBUTTON | BS_OWNERDRAW, 430, 104, 110, 42, ID_START);
    g_ui.statusText = makeControl(L"STATIC", L"Ready", SS_LEFT, 302, 154, 238, 22, 0, g_fontSmall);
    g_ui.liveText = makeControl(L"STATIC", L"0 km/h     0 rpm", SS_LEFT, 302, 174, 238, 18, 0, g_fontMetric);

    makeSlider(0, ID_MASTER, L"Master", g_settings.master, 260);
    makeSlider(1, ID_IMPACTS, L"Impacts", g_settings.impacts, 295);
    makeSlider(2, ID_ROAD, L"Road texture", g_settings.road, 330);
    makeSlider(3, ID_ENGINE, L"Engine", g_settings.engine, 365);
    makeSlider(4, ID_SLIP, L"Lateral load", g_settings.slip, 400);
    makeSlider(5, ID_SMOOTHING, L"Smoothing", g_settings.smoothing, 435);

    makeControl(L"BUTTON", L"SMOOTH", BS_PUSHBUTTON | BS_OWNERDRAW, 43, 531, 142, 38, ID_PRESET_SMOOTH);
    makeControl(L"BUTTON", L"GRAVEL", BS_PUSHBUTTON | BS_OWNERDRAW, 195, 531, 142, 38, ID_PRESET_GRAVEL);
    makeControl(L"BUTTON", L"BRUTAL", BS_PUSHBUTTON | BS_OWNERDRAW, 347, 531, 142, 38, ID_PRESET_BRUTAL);
    g_ui.outputText = makeControl(L"STATIC", L"Heavy 0%   Light 0%", SS_RIGHT, 350, 582, 188, 20, 0, g_fontMetric);
    makeControl(L"STATIC", L"OUTPUT", SS_LEFT, 43, 584, 70, 18, 0, g_fontSmall);

    makeControl(L"STATIC", L"Controller", SS_LEFT, 32, 632, 85, 23, 0, g_fontSmall);
    g_ui.controller = makeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, 116, 625, 158, 220, ID_CONTROLLER);
    enableDarkControl(g_ui.controller);
    SendMessageW(g_ui.controller, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (first XInput)"));
    SendMessageW(g_ui.controller, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Controller 1"));
    SendMessageW(g_ui.controller, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Controller 2"));
    SendMessageW(g_ui.controller, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Controller 3"));
    SendMessageW(g_ui.controller, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Controller 4"));
    SendMessageW(g_ui.controller, CB_SETCURSEL, g_settings.controller + 1, 0);
    makeControl(L"STATIC", L"UDP port", SS_LEFT, 296, 632, 62, 23, 0, g_fontSmall);
    wchar_t portText[12]; _snwprintf_s(portText, _TRUNCATE, L"%d", g_settings.port);
    g_ui.port = makeControl(L"EDIT", portText, ES_NUMBER | ES_CENTER | WS_BORDER | WS_TABSTOP, 360, 626, 72, 26, ID_PORT);
    enableDarkControl(g_ui.port);
    g_ui.autoStart = makeControl(L"BUTTON", L"Start listening when opened", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 667, 230, 25, ID_AUTOSTART);
    enableDarkControl(g_ui.autoStart);
    SendMessageW(g_ui.autoStart, BM_SETCHECK, g_settings.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    g_ui.muteAirborne = makeControl(L"BUTTON", L"Mute vibration while airborne", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 700, 250, 25, ID_MUTE_AIRBORNE);
    enableDarkControl(g_ui.muteAirborne);
    SendMessageW(g_ui.muteAirborne, BM_SETCHECK, g_settings.muteAirborne ? BST_CHECKED : BST_UNCHECKED, 0);
    makeControl(L"BUTTON", L"TEST HEAVY", BS_PUSHBUTTON | BS_OWNERDRAW, 296, 662, 118, 34, ID_TEST_HEAVY);
    makeControl(L"BUTTON", L"TEST LIGHT", BS_PUSHBUTTON | BS_OWNERDRAW, 422, 662, 118, 34, ID_TEST_LIGHT);

    SetTimer(hwnd, 1, 100, nullptr);
}

LRESULT drawButton(const DRAWITEMSTRUCT* item) {
    const bool down = (item->itemState & ODS_SELECTED) != 0;
    COLORREF color = item->CtlID == ID_START ? kAccent : kPanel2;
    if (item->CtlID == ID_PRESET_BRUTAL) color = RGB(91, 48, 57);
    if (down) color = RGB(GetRValue(color) * 3 / 4, GetGValue(color) * 3 / 4, GetBValue(color) * 3 / 4);
    HBRUSH brush = CreateSolidBrush(color); HPEN pen = CreatePen(PS_SOLID, 1, color);
    auto oldB = SelectObject(item->hDC, brush); auto oldP = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, 12, 12);
    SelectObject(item->hDC, oldB); SelectObject(item->hDC, oldP); DeleteObject(brush); DeleteObject(pen);
    wchar_t text[64]{}; GetWindowTextW(item->hwndItem, text, 64);
    auto oldF = SelectObject(item->hDC, g_fontSmall); SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, item->CtlID == ID_START ? RGB(8, 29, 27) : kText);
    RECT r = item->rcItem; DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, oldF);
    return TRUE;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        createUi(hwnd);
        if (g_settings.autoStart) PostMessageW(hwnd, WM_COMMAND, ID_START, 0);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(hwnd); return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText); SetBkMode(dc, OPAQUE);
        HWND child = reinterpret_cast<HWND>(lParam);
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, TRACKBAR_CLASSW) == 0) {
            SetBkMode(dc, OPAQUE); SetBkColor(dc, kPanel);
            return reinterpret_cast<LRESULT>(g_panelBrush);
        }
        RECT childRect{}; GetWindowRect(child, &childRect);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&childRect), 2);
        const bool overPanel = childRect.top >= 86 && childRect.top < 612;
        SetBkColor(dc, overPanel ? kPanel : kBg);
        return reinterpret_cast<LRESULT>(overPanel ? g_panelBrush : g_bgBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText); SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText); SetBkColor(dc, kPanel2);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_HSCROLL:
        updateSettingsFromUi(); return 0;
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlID == ID_CONTROLLER) { measure->itemHeight = 24; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID == ID_CONTROLLER) {
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            HBRUSH brush = CreateSolidBrush(selected ? RGB(45, 68, 105) : kPanel2);
            FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            if (item->itemID != static_cast<UINT>(-1)) {
                wchar_t text[64]{};
                SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
                RECT textRect = item->rcItem; textRect.left += 8;
                drawTextAt(item->hDC, text, textRect, g_font, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            return TRUE;
        }
        return drawButton(item);
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_START) { if (g_running) stopListening(); else startListening(); }
        else if (id == ID_TEST_HEAVY) rumbleTest(true);
        else if (id == ID_TEST_LIGHT) rumbleTest(false);
        else if (id == ID_PRESET_SMOOTH) setPreset(72, 68, 40, 18, 24, 66);
        else if (id == ID_PRESET_GRAVEL) setPreset(86, 100, 74, 24, 42, 32);
        else if (id == ID_PRESET_BRUTAL) setPreset(100, 100, 94, 42, 65, 12);
        else if (id == ID_CONTROLLER || id == ID_AUTOSTART || id == ID_MUTE_AIRBORNE) updateSettingsFromUi();
        return 0;
    }
    case WM_TIMER: {
        const bool fresh = g_running && GetTickCount() - g_live.packetTick.load() < 400;
        wchar_t live[128];
        _snwprintf_s(live, _TRUNCATE, L"%.0f km/h     %.0f rpm", g_live.speed.load() * 3.6f, g_live.rpm.load());
        setTextIfChanged(g_ui.liveText, live);
        wchar_t output[96];
        _snwprintf_s(output, _TRUNCATE, L"Heavy %d%%   Light %d%%", (g_heavyOut.load() * 100) / 65535, (g_lightOut.load() * 100) / 65535);
        setTextIfChanged(g_ui.outputText, output);
        if (g_running) setTextIfChanged(g_ui.statusText, fresh ? L"Rumbling from live physics" : L"Listening on 127.0.0.1");
        RECT indicators{40, 125, 60, 176};
        InvalidateRect(hwnd, &indicators, FALSE);
        return 0;
    }
    case WM_TELEMETRY:
        if (wParam == 2) MessageBoxW(hwnd, L"Windows networking could not start.", kAppName, MB_ICONERROR);
        if (wParam == 3) {
            wchar_t text[180]; _snwprintf_s(text, _TRUNCATE, L"UDP port %d could not be opened. Another telemetry app may already be using it.\n\nWindows socket error: %lld", g_settings.port, static_cast<long long>(lParam));
            MessageBoxW(hwnd, text, kAppName, MB_ICONERROR); SetWindowTextW(g_ui.start, L"START");
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_quit = true; stopListening(); saveSettings(); KillTimer(hwnd, 1);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    loadSettings(); loadXInput();
    g_bgBrush = CreateSolidBrush(kBg); g_panelBrush = CreateSolidBrush(kPanel);
    g_editBrush = CreateSolidBrush(kPanel2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc; wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    wc.hbrBackground = g_bgBrush; wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, kClassName, kAppName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 606, 790, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (g_xinput) FreeLibrary(g_xinput);
    DeleteObject(g_font); DeleteObject(g_fontSmall); DeleteObject(g_fontTitle); DeleteObject(g_fontMetric);
    DeleteObject(g_bgBrush); DeleteObject(g_panelBrush); DeleteObject(g_editBrush);
    return static_cast<int>(msg.wParam);
}
