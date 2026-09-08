#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

constexpr wchar_t kClassName[] = L"CSMoyuWindow";
constexpr wchar_t kHelpClassName[] = L"CSMoyuHelpWindow";
constexpr UINT WM_GSI_STATUS = WM_APP + 1;
constexpr UINT WM_GSI_EVENT = WM_APP + 2;
constexpr int kPort = 3000;
constexpr UINT_PTR kReturnToGameTimer = 1;
constexpr UINT kReturnToGameIntervalMs = 400;
constexpr int kReturnToGameAttempts = 3;

enum class GsiEvent : WPARAM { Died = 1, RoundStarted = 2, GameEnded = 3, WarmupEnded = 4 };
enum class ReturnReason { RoundStarted, GameEnded, WarmupEnded };

enum ControlId {
    IDC_MODE_PROGRAM = 1001, IDC_MODE_HOTKEY, IDC_TARGET, IDC_BROWSE,
    IDC_HOTKEY, IDC_START, IDC_INSTALL, IDC_STATUS, IDC_PAUSE_MUSIC, IDC_PAUSE_VIDEO,
    IDC_HELP_BUTTON
};

struct Settings {
    bool programMode = false;
    std::wstring target;
    std::wstring cs2Path;
    UINT key = VK_TAB;
    bool ctrl = false, alt = true, shift = false, win = false;
    bool pauseMusic = false;
    bool pauseVideo = false;
};

struct AppState {
    HWND window{};
    HWND helpWindow{};
    HWND modeProgram{}, modeHotkey{}, target{}, browse{}, hotkey{}, start{}, install{}, status{};
    HWND pauseMusic{}, pauseVideo{};
    HFONT font{}, titleFont{};
    HBRUSH background{};
    Settings settings;
    std::atomic<bool> listening{false};
    std::atomic<bool> stopRequested{false};
    std::thread serverThread;
    std::atomic<SOCKET> listenSocket{INVALID_SOCKET};
    int ownPreviousHealth = -1;
    bool waitingForNextRound = false;
    std::optional<int> deathRound;
    std::optional<int> observedRound;
    std::string previousRoundPhase;
    std::string previousMapPhase;
    std::chrono::steady_clock::time_point lastPayloadHandled{};
    int returnToGameAttemptsRemaining = 0;
    ReturnReason returnReason = ReturnReason::RoundStarted;
    bool capturing = false;
    WNDPROC oldHotkeyProc{};
    std::wstring iniPath;
} g;

void SetStatus(const std::wstring& text) {
    SetWindowTextW(g.status, text.c_str());
}

std::wstring AppDataPath() {
    wchar_t dir[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, dir))) {
        fs::path folder = fs::path(dir) / L"CSMoyu";
        std::error_code ec;
        fs::create_directories(folder, ec);
        return (folder / L"settings.ini").wstring();
    }
    return L".\\CSMoyu.ini";
}

void LoadSettings() {
    g.iniPath = AppDataPath();
    wchar_t path[32768]{};
    GetPrivateProfileStringW(L"action", L"target", L"", path, 32768, g.iniPath.c_str());
    g.settings.target = path;
    GetPrivateProfileStringW(L"cs2", L"path", L"", path, 32768, g.iniPath.c_str());
    g.settings.cs2Path = path;
    g.settings.programMode = GetPrivateProfileIntW(L"action", L"mode", 1, g.iniPath.c_str()) == 0;
    g.settings.key = GetPrivateProfileIntW(L"hotkey", L"key", VK_TAB, g.iniPath.c_str());
    g.settings.ctrl = GetPrivateProfileIntW(L"hotkey", L"ctrl", 0, g.iniPath.c_str()) != 0;
    g.settings.alt = GetPrivateProfileIntW(L"hotkey", L"alt", 1, g.iniPath.c_str()) != 0;
    g.settings.shift = GetPrivateProfileIntW(L"hotkey", L"shift", 0, g.iniPath.c_str()) != 0;
    g.settings.win = GetPrivateProfileIntW(L"hotkey", L"win", 0, g.iniPath.c_str()) != 0;
    g.settings.pauseMusic = GetPrivateProfileIntW(L"media", L"pause_music", 0, g.iniPath.c_str()) != 0;
    g.settings.pauseVideo = GetPrivateProfileIntW(L"media", L"pause_video", 0, g.iniPath.c_str()) != 0;
}

void SaveSettings() {
    auto writeInt = [](const wchar_t* section, const wchar_t* key, int value) {
        const std::wstring str = std::to_wstring(value);
        WritePrivateProfileStringW(section, key, str.c_str(), g.iniPath.c_str());
    };
    WritePrivateProfileStringW(L"action", L"target", g.settings.target.c_str(), g.iniPath.c_str());
    WritePrivateProfileStringW(L"cs2", L"path", g.settings.cs2Path.c_str(), g.iniPath.c_str());
    writeInt(L"action", L"mode", g.settings.programMode ? 0 : 1);
    writeInt(L"hotkey", L"key", static_cast<int>(g.settings.key));
    writeInt(L"hotkey", L"ctrl", g.settings.ctrl);
    writeInt(L"hotkey", L"alt", g.settings.alt);
    writeInt(L"hotkey", L"shift", g.settings.shift);
    writeInt(L"hotkey", L"win", g.settings.win);
    writeInt(L"media", L"pause_music", g.settings.pauseMusic);
    writeInt(L"media", L"pause_video", g.settings.pauseVideo);
}

void SaveUiSettings() {
    if (g.modeProgram && SendMessageW(g.modeProgram, BM_GETCHECK, 0, 0) == BST_CHECKED)
        g.settings.programMode = true;
    else if (g.modeHotkey && SendMessageW(g.modeHotkey, BM_GETCHECK, 0, 0) == BST_CHECKED)
        g.settings.programMode = false;

    if (g.target) {
        std::vector<wchar_t> target(static_cast<size_t>(GetWindowTextLengthW(g.target)) + 1);
        GetWindowTextW(g.target, target.data(), static_cast<int>(target.size()));
        g.settings.target = target.data();
    }
    if (g.pauseMusic)
        g.settings.pauseMusic = SendMessageW(g.pauseMusic, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g.pauseVideo)
        g.settings.pauseVideo = SendMessageW(g.pauseVideo, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SaveSettings();
}

std::wstring KeyName(UINT vk) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_END || vk == VK_HOME ||
        vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_NUMLOCK) scan |= 1 << 24;
    wchar_t name[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan), name, 64)) return name;
    wchar_t fallback[16]{};
    wsprintfW(fallback, L"VK %02X", vk);
    return fallback;
}

std::wstring HotkeyText() {
    std::wstring out;
    if (g.settings.ctrl) out += L"Ctrl + ";
    if (g.settings.alt) out += L"Alt + ";
    if (g.settings.shift) out += L"Shift + ";
    if (g.settings.win) out += L"Win + ";
    out += KeyName(g.settings.key);
    return out;
}

void RefreshControls() {
    CheckRadioButton(g.window, IDC_MODE_PROGRAM, IDC_MODE_HOTKEY,
        g.settings.programMode ? IDC_MODE_PROGRAM : IDC_MODE_HOTKEY);
    SetWindowTextW(g.target, g.settings.target.c_str());
    SetWindowTextW(g.hotkey, g.capturing ? L"请按下组合键（Esc 取消）" : HotkeyText().c_str());
    EnableWindow(g.target, g.settings.programMode);
    EnableWindow(g.browse, g.settings.programMode);
    EnableWindow(g.hotkey, !g.settings.programMode);
    SendMessageW(g.pauseMusic, BM_SETCHECK, g.settings.pauseMusic ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.pauseVideo, BM_SETCHECK, g.settings.pauseVideo ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool IsModifier(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LWIN || vk == VK_RWIN;
}

LRESULT CALLBACK HotkeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN && !g.capturing) {
        g.capturing = true;
        SetFocus(hwnd);
        RefreshControls();
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (!g.capturing) return 0;
        const UINT vk = static_cast<UINT>(wp);
        if (vk == VK_ESCAPE) {
            g.capturing = false;
            RefreshControls();
            return 0;
        }
        if (!IsModifier(vk)) {
            g.settings.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            g.settings.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            g.settings.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            g.settings.win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
            g.settings.key = vk;
            g.capturing = false;
            SaveSettings();
            RefreshControls();
        }
        return 0;
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcW(g.oldHotkeyProc, hwnd, msg, wp, lp);
}

std::optional<std::string> ObjectForKey(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t pos = json.find(token);
    while (pos != std::string::npos) {
        pos = json.find(':', pos + token.size());
        if (pos == std::string::npos) return std::nullopt;
        pos = json.find_first_not_of(" \t\r\n", pos + 1);
        if (pos != std::string::npos && json[pos] == '{') {
            const size_t start = pos;
            int depth = 0;
            bool quoted = false, escaped = false;
            for (; pos < json.size(); ++pos) {
                const char c = json[pos];
                if (quoted) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') quoted = false;
                } else if (c == '"') quoted = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) return json.substr(start, pos - start + 1);
            }
        }
        pos = json.find(token, pos + 1);
    }
    return std::nullopt;
}

std::optional<int> IntForKey(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t pos = json.find(token);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + token.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find_first_of("-0123456789", pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    try { return std::stoi(json.substr(pos)); } catch (...) { return std::nullopt; }
}

std::optional<std::string> StringForKey(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t pos = json.find(token);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + token.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    const size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(pos + 1, end - pos - 1);
}

void HandlePayload(const std::string& json) {
    const auto map = ObjectForKey(json, "map");
    const auto round = ObjectForKey(json, "round");
    const auto mapPhase = map ? StringForKey(*map, "phase") : std::nullopt;
    const auto currentRound = map ? IntForKey(*map, "round") : std::nullopt;
    const auto roundPhase = round ? StringForKey(*round, "phase") : std::nullopt;
    if (currentRound) g.observedRound = currentRound;

    const bool warmupEnded = mapPhase && *mapPhase == "live" && g.previousMapPhase == "warmup";
    if (warmupEnded) {
        g.waitingForNextRound = false;
        g.deathRound.reset();
        PostMessageW(g.window, WM_GSI_EVENT, static_cast<WPARAM>(GsiEvent::WarmupEnded), 0);
    } else if (g.waitingForNextRound) {
        const bool gameEnded = mapPhase && *mapPhase == "gameover";
        const bool enteredFreezeTime = roundPhase && *roundPhase == "freezetime" &&
            g.previousRoundPhase != "freezetime";
        // If freezetime was missed, a higher map.round combined with "live" still
        // proves that play has moved on to the next round.
        const bool nextRoundAlreadyLive = g.deathRound && currentRound &&
            *currentRound > *g.deathRound && roundPhase && *roundPhase == "live";
        if (gameEnded || enteredFreezeTime || nextRoundAlreadyLive) {
            g.waitingForNextRound = false;
            g.deathRound.reset();
            PostMessageW(g.window, WM_GSI_EVENT,
                static_cast<WPARAM>(gameEnded ? GsiEvent::GameEnded : GsiEvent::RoundStarted), 0);
        }
    }
    if (roundPhase) g.previousRoundPhase = *roundPhase;
    if (mapPhase) g.previousMapPhase = *mapPhase;

    const auto provider = ObjectForKey(json, "provider");
    const auto player = ObjectForKey(json, "player");
    if (!provider || !player) return;
    const auto localSteamId = StringForKey(*provider, "steamid");
    const auto playerSteamId = StringForKey(*player, "steamid");
    // While dead, CS2 reports the currently observed teammate as `player`.
    // Only the player whose SteamID matches the local provider is ours.
    if (!localSteamId || !playerSteamId || *localSteamId != *playerSteamId) return;
    const auto activity = StringForKey(*player, "activity");
    const auto state = ObjectForKey(*player, "state");
    if (!state) return;
    const auto health = IntForKey(*state, "health");
    if (!health) return;
    if (activity && *activity != "playing") return;

    if (*health == 0) {
        if (g.ownPreviousHealth > 0 && !g.waitingForNextRound) {
            g.waitingForNextRound = true;
            g.deathRound = currentRound ? currentRound : g.observedRound;
            PostMessageW(g.window, WM_GSI_EVENT, static_cast<WPARAM>(GsiEvent::Died), 0);
        }
        g.ownPreviousHealth = 0;
    } else {
        g.ownPreviousHealth = *health;
    }
}

bool IsZeroHealthPayload(const std::string& json) {
    const auto player = ObjectForKey(json, "player");
    if (!player) return false;
    const auto state = ObjectForKey(*player, "state");
    const auto health = state ? IntForKey(*state, "health") : std::nullopt;
    return health && *health == 0;
}

void HandlePayloadAtAdaptiveRate(const std::string& json) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    constexpr auto aliveInterval = seconds(1);

    // GSI pushes data to us. While alive, avoid repeatedly parsing noisy player
    // state; a death packet is never delayed. Once dead, inspect every update so
    // the next round/game-over transition is noticed promptly.
    const bool phaseOnlyPayload = !ObjectForKey(json, "player");
    if (!phaseOnlyPayload && !g.waitingForNextRound && g.ownPreviousHealth >= 0 &&
        now - g.lastPayloadHandled < aliveInterval && !IsZeroHealthPayload(json)) return;
    g.lastPayloadHandled = now;
    HandlePayload(json);
}

bool SendAll(SOCKET socket, const char* data, int size) {
    while (size > 0) {
        const int sent = send(socket, data, size, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= sent;
    }
    return true;
}

void ServerLoop() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        PostMessageW(g.window, WM_GSI_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(L"网络组件初始化失败")));
        g.listening = false;
        return;
    }
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g.listenSocket.store(server);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    BOOL exclusive = TRUE;
    setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (server == INVALID_SOCKET || bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server, 4) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        const SOCKET ownedSocket = g.listenSocket.exchange(INVALID_SOCKET);
        if (ownedSocket != INVALID_SOCKET) closesocket(ownedSocket);
        g.listening = false;
        PostMessageW(g.window, WM_GSI_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(L"无法监听 127.0.0.1:3000（错误 " + std::to_wstring(error) + L"）")));
        WSACleanup();
        return;
    }
    PostMessageW(g.window, WM_GSI_STATUS, 1, reinterpret_cast<LPARAM>(new std::wstring(L"监听中 · 等待 CS2 数据")));
    while (!g.stopRequested) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        DWORD timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buffer[8192];
        size_t expected = 0;
        for (;;) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            request.append(buffer, count);
            const size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos && expected == 0) {
                size_t p = request.find("Content-Length:");
                if (p == std::string::npos) p = request.find("content-length:");
                if (p != std::string::npos) expected = headerEnd + 4 + std::strtoul(request.c_str() + p + 15, nullptr, 10);
                else expected = headerEnd + 4;
            }
            if (expected && request.size() >= expected) break;
            if (request.size() > 1024 * 1024) break;
        }
        const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        SendAll(client, response, static_cast<int>(sizeof(response) - 1));
        closesocket(client);
        const size_t body = request.find("\r\n\r\n");
        if (body != std::string::npos) HandlePayloadAtAdaptiveRate(request.substr(body + 4));
    }
    const SOCKET ownedSocket = g.listenSocket.exchange(INVALID_SOCKET);
    if (ownedSocket != INVALID_SOCKET) closesocket(ownedSocket);
    WSACleanup();
}

void StartServer() {
    if (g.listening) return;
    if (g.serverThread.joinable()) g.serverThread.join();
    g.stopRequested = false;
    g.ownPreviousHealth = -1;
    g.waitingForNextRound = false;
    g.deathRound.reset();
    g.observedRound.reset();
    g.previousRoundPhase.clear();
    g.previousMapPhase.clear();
    g.lastPayloadHandled = {};
    g.listening = true;
    SetWindowTextW(g.start, L"停止监听");
    g.serverThread = std::thread(ServerLoop);
}

void StopServer() {
    if (!g.listening && !g.serverThread.joinable()) return;
    g.stopRequested = true;
    const SOCKET socket = g.listenSocket.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) closesocket(socket);
    if (g.serverThread.joinable()) g.serverThread.join();
    g.listening = false;
    KillTimer(g.window, kReturnToGameTimer);
    g.returnToGameAttemptsRemaining = 0;
    SetWindowTextW(g.start, L"开始监听");
    SetStatus(L"已停止");
}

struct FindWindowData {
    std::wstring target;
    bool filenameOnly = false;
    HWND result{};
};

BOOL CALLBACK FindTargetWindow(HWND hwnd, LPARAM param) {
    auto* data = reinterpret_cast<FindWindowData*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[32768]{};
    DWORD size = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    const std::wstring candidate = data->filenameOnly ? fs::path(path).filename().wstring() : std::wstring(path);
    if (ok && _wcsicmp(candidate.c_str(), data->target.c_str()) == 0) {
        data->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool ActivateWindow(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    if (foregroundThread != currentThread) AttachThreadInput(currentThread, foregroundThread, TRUE);
    if (targetThread != currentThread) AttachThreadInput(currentThread, targetThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (targetThread != currentThread) AttachThreadInput(currentThread, targetThread, FALSE);
    if (foregroundThread != currentThread) AttachThreadInput(currentThread, foregroundThread, FALSE);
    return GetForegroundWindow() == hwnd;
}

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring WindowProcessName(HWND hwnd) {
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD size = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    return ok ? Lowercase(fs::path(path).filename().wstring()) : std::wstring{};
}

bool IsMusicPlayer(const std::wstring& process) {
    static constexpr const wchar_t* names[] = {
        L"cloudmusic.exe", L"qqmusic.exe", L"kugou.exe", L"kuwo.exe",
        L"spotify.exe", L"music.ui.exe", L"microsoft.media.player.exe",
        L"foobar2000.exe", L"aimp.exe"
    };
    for (const auto* name : names) if (process == name) return true;
    return false;
}

bool IsBrowser(const std::wstring& process) {
    static constexpr const wchar_t* names[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"vivaldi.exe", L"360chrome.exe", L"qqbrowser.exe"
    };
    for (const auto* name : names) if (process == name) return true;
    return false;
}

bool IsSupportedVideoTitle(HWND hwnd) {
    wchar_t title[1024]{};
    GetWindowTextW(hwnd, title, 1024);
    const std::wstring text = Lowercase(title);
    static constexpr const wchar_t* markers[] = {
        L"哔哩哔哩", L"bilibili", L"抖音", L"douyin", L"西瓜视频", L"ixigua"
    };
    for (const auto* marker : markers) {
        if (text.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

struct PauseMediaData {
    bool music;
    bool video;
};

BOOL CALLBACK PauseMediaWindow(HWND hwnd, LPARAM param) {
    auto* data = reinterpret_cast<PauseMediaData*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    const std::wstring process = WindowProcessName(hwnd);
    const bool musicTarget = data->music && IsMusicPlayer(process);
    const bool videoTarget = data->video && IsBrowser(process) && IsSupportedVideoTitle(hwnd);
    if (!musicTarget && !videoTarget) return TRUE;

    // MEDIA_PAUSE is idempotent: unlike the play/pause toggle, it will not
    // accidentally start a session that was already paused.
    DWORD_PTR ignored{};
    SendMessageTimeoutW(hwnd, WM_APPCOMMAND, reinterpret_cast<WPARAM>(g.window),
        MAKELPARAM(0, APPCOMMAND_MEDIA_PAUSE | FAPPCOMMAND_KEY),
        SMTO_ABORTIFHUNG, 250, &ignored);
    return TRUE;
}

void PauseSelectedMedia() {
    if (!g.settings.pauseMusic && !g.settings.pauseVideo) return;
    PauseMediaData data{g.settings.pauseMusic, g.settings.pauseVideo};
    EnumWindows(PauseMediaWindow, reinterpret_cast<LPARAM>(&data));
}

void SendConfiguredHotkey() {
    std::vector<INPUT> inputs;
    auto add = [&](WORD vk, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        inputs.push_back(input);
    };
    std::vector<WORD> modifiers;
    if (g.settings.ctrl) modifiers.push_back(VK_CONTROL);
    if (g.settings.alt) modifiers.push_back(VK_MENU);
    if (g.settings.shift) modifiers.push_back(VK_SHIFT);
    if (g.settings.win) modifiers.push_back(VK_LWIN);
    for (WORD key : modifiers) add(key, false);
    add(static_cast<WORD>(g.settings.key), false);
    add(static_cast<WORD>(g.settings.key), true);
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) add(*it, true);
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void TriggerAction() {
    PauseSelectedMedia();
    if (g.settings.programMode) {
        if (g.settings.target.empty()) {
            SetStatus(L"检测到死亡，但尚未选择目标程序");
            return;
        }
        FindWindowData data{g.settings.target, false};
        EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&data));
        if (data.result) {
            ActivateWindow(data.result);
            SetStatus(L"检测到死亡 · 已切换到目标程序");
        } else {
            const HINSTANCE result = ShellExecuteW(nullptr, L"open", g.settings.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SetStatus(reinterpret_cast<INT_PTR>(result) > 32 ? L"检测到死亡 · 已启动目标程序" : L"检测到死亡 · 目标程序启动失败");
        }
    } else {
        SendConfiguredHotkey();
        SetStatus(L"检测到死亡 · 已发送 " + HotkeyText());
    }
}

HWND FindCs2Window() {
    if (!g.settings.cs2Path.empty()) {
        FindWindowData exact{g.settings.cs2Path, false};
        EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&exact));
        if (exact.result) return exact.result;
    }
    FindWindowData byName{L"cs2.exe", true};
    EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&byName));
    return byName.result;
}

const wchar_t* ReturnReasonText(ReturnReason reason) {
    switch (reason) {
    case ReturnReason::GameEnded: return L"检测到游戏结束";
    case ReturnReason::WarmupEnded: return L"检测到热身结束";
    default: return L"检测到下一回合开始";
    }
}

void AttemptReturnToGame() {
    if (g.returnToGameAttemptsRemaining <= 0) {
        KillTimer(g.window, kReturnToGameTimer);
        return;
    }
    --g.returnToGameAttemptsRemaining;
    const HWND cs2 = FindCs2Window();
    if (cs2) {
        ActivateWindow(cs2);
        SetStatus(std::wstring(ReturnReasonText(g.returnReason)) + L" · 已切换回 CS2");
    } else {
        SetStatus(std::wstring(ReturnReasonText(g.returnReason)) + L"，但未找到 CS2 窗口");
    }
    if (g.returnToGameAttemptsRemaining > 0) {
        SetTimer(g.window, kReturnToGameTimer, kReturnToGameIntervalMs, nullptr);
    } else {
        KillTimer(g.window, kReturnToGameTimer);
    }
}

void ReturnToGame(ReturnReason reason) {
    KillTimer(g.window, kReturnToGameTimer);
    g.returnReason = reason;
    g.returnToGameAttemptsRemaining = kReturnToGameAttempts;
    AttemptReturnToGame();
}

void BrowseTarget() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g.window;
    dialog.lpstrFilter = L"程序文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        g.settings.target = path;
        SaveSettings();
        RefreshControls();
    }
}

bool IsCs2Exe(const fs::path& path) {
    std::error_code ec;
    return _wcsicmp(path.filename().c_str(), L"cs2.exe") == 0 && fs::is_regular_file(path, ec);
}

void AddUniquePath(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty()) return;
    const std::wstring value = path.lexically_normal().wstring();
    for (const auto& existing : paths) {
        if (_wcsicmp(existing.lexically_normal().c_str(), value.c_str()) == 0) return;
    }
    paths.emplace_back(value);
}

std::optional<std::wstring> ReadRegistryString(HKEY root, const wchar_t* subkey,
                                               const wchar_t* value, REGSAM view = 0) {
    HKEY key{};
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return std::nullopt;
    DWORD type = 0, bytes = 0;
    const LONG sizeResult = RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes);
    if (sizeResult != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    const LONG readResult = RegQueryValueExW(key, value, nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (readResult != ERROR_SUCCESS) return std::nullopt;
    buffer.back() = L'\0';
    std::wstring result = buffer.data();
    if (type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
        if (needed) {
            std::vector<wchar_t> expanded(needed);
            if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(), needed)) result = expanded.data();
        }
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::vector<std::wstring> QuotedVdfValues(const std::wstring& line) {
    std::vector<std::wstring> values;
    for (size_t start = 0; start < line.size();) {
        start = line.find(L'"', start);
        if (start == std::wstring::npos) break;
        std::wstring value;
        bool closed = false;
        for (size_t i = start + 1; i < line.size(); ++i) {
            if (line[i] == L'"') { values.push_back(value); start = i + 1; closed = true; break; }
            if (line[i] == L'\\' && i + 1 < line.size() && line[i + 1] == L'\\') ++i;
            value.push_back(line[i]);
        }
        if (!closed) break;
    }
    return values;
}

std::wstring ReadTextFileUtf8(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return Utf8ToWide(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

std::optional<fs::path> RunningCs2Path() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"cs2.exe") != 0) continue;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::vector<wchar_t> path(32768);
            DWORD size = static_cast<DWORD>(path.size());
            const bool found = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
            CloseHandle(process);
            if (found) {
                CloseHandle(snapshot);
                fs::path result(std::wstring(path.data(), size));
                if (IsCs2Exe(result)) return result;
                return std::nullopt;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return std::nullopt;
}

std::optional<fs::path> Cs2InSteamLibrary(const fs::path& library) {
    const fs::path steamapps = library / L"steamapps";
    std::wstring installDir = L"Counter-Strike Global Offensive";
    const std::wstring manifest = ReadTextFileUtf8(steamapps / L"appmanifest_730.acf");
    if (!manifest.empty()) {
        size_t lineStart = 0;
        while (lineStart < manifest.size()) {
            const size_t lineEnd = manifest.find_first_of(L"\r\n", lineStart);
            const auto values = QuotedVdfValues(manifest.substr(lineStart, lineEnd - lineStart));
            if (values.size() >= 2 && _wcsicmp(values[0].c_str(), L"installdir") == 0) {
                installDir = values[1];
                break;
            }
            if (lineEnd == std::wstring::npos) break;
            lineStart = lineEnd + 1;
        }
    }
    const fs::path candidate = steamapps / L"common" / installDir / L"game" / L"bin" / L"win64" / L"cs2.exe";
    if (IsCs2Exe(candidate)) return candidate;
    return std::nullopt;
}

std::optional<fs::path> DetectCs2Path() {
    if (IsCs2Exe(g.settings.cs2Path)) return fs::path(g.settings.cs2Path);
    if (const auto running = RunningCs2Path()) return running;

    std::vector<fs::path> steamRoots;
    for (const auto& entry : {
        ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
        ReadRegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY),
        ReadRegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY)}) {
        if (entry) AddUniquePath(steamRoots, *entry);
    }
    wchar_t programFiles[32768]{};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, 32768))
        AddUniquePath(steamRoots, fs::path(programFiles) / L"Steam");
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, 32768))
        AddUniquePath(steamRoots, fs::path(programFiles) / L"Steam");

    std::vector<fs::path> libraries = steamRoots;
    for (const auto& root : steamRoots) {
        const std::wstring vdf = ReadTextFileUtf8(root / L"steamapps" / L"libraryfolders.vdf");
        size_t lineStart = 0;
        while (lineStart < vdf.size()) {
            const size_t lineEnd = vdf.find_first_of(L"\r\n", lineStart);
            const auto values = QuotedVdfValues(vdf.substr(lineStart, lineEnd - lineStart));
            if (values.size() >= 2) {
                const bool newFormat = _wcsicmp(values[0].c_str(), L"path") == 0;
                const bool oldFormat = !values[0].empty() &&
                    std::all_of(values[0].begin(), values[0].end(), [](wchar_t c) { return iswdigit(c) != 0; }) &&
                    values[1].find_first_of(L"\\/") != std::wstring::npos;
                if (newFormat || oldFormat) AddUniquePath(libraries, values[1]);
            }
            if (lineEnd == std::wstring::npos) break;
            lineStart = lineEnd + 1;
        }
    }
    for (const auto& library : libraries) {
        if (const auto result = Cs2InSteamLibrary(library)) return result;
    }
    return std::nullopt;
}

bool WriteGsiConfig(const fs::path& cs2Exe, std::wstring& result) {
    fs::path current = cs2Exe.parent_path();
    fs::path game;
    while (!current.empty() && current != current.root_path()) {
        if (_wcsicmp(current.filename().c_str(), L"game") == 0) { game = current; break; }
        current = current.parent_path();
    }
    if (game.empty()) {
        result = L"所选 cs2.exe 路径中没有 game 目录。请选择 Steam 安装目录下的 game\\bin\\win64\\cs2.exe。";
        return false;
    }
    const fs::path cfgDir = game / L"csgo" / L"cfg";
    const fs::path playerCfgFile = cfgDir / L"gamestate_integration_csmoyu.cfg";
    const fs::path phaseCfgFile = cfgDir / L"gamestate_integration_csmoyu_phase.cfg";
    std::error_code ec;
    fs::create_directories(cfgDir, ec);
    std::ofstream playerFile(playerCfgFile, std::ios::binary | std::ios::trunc);
    std::ofstream phaseFile(phaseCfgFile, std::ios::binary | std::ios::trunc);
    if (!playerFile || !phaseFile) {
        result = L"无法写入 GSI 配置：\n" + cfgDir.wstring() + L"\n请检查目录权限。";
        return false;
    }
    // Player state can be noisy (for example while flashed), so cap it at 1 Hz.
    // A separate, narrow phase stream keeps round transitions responsive without
    // repeatedly serializing all player state at 10 Hz.
    playerFile << "\"CSMoyu Player Integration\"\n{\n"
            "  \"uri\" \"http://127.0.0.1:3000/player\"\n"
            "  \"timeout\" \"1.0\"\n"
            "  \"buffer\" \"0.0\"\n"
            "  \"throttle\" \"1.0\"\n"
            "  \"heartbeat\" \"30.0\"\n"
            "  \"data\"\n  {\n"
            "    \"provider\" \"1\"\n"
            "    \"player_id\" \"1\"\n"
            "    \"player_state\" \"1\"\n"
            "  }\n}\n";
    phaseFile << "\"CSMoyu Phase Integration\"\n{\n"
            "  \"uri\" \"http://127.0.0.1:3000/phase\"\n"
            "  \"timeout\" \"1.0\"\n"
            "  \"buffer\" \"0.0\"\n"
            "  \"throttle\" \"0.1\"\n"
            "  \"heartbeat\" \"30.0\"\n"
            "  \"data\"\n  {\n"
            "    \"map\" \"1\"\n"
            "    \"round\" \"1\"\n"
            "  }\n}\n";
    playerFile.close();
    phaseFile.close();
    result = L"配置已安装：\n" + playerCfgFile.wstring() + L"\n" + phaseCfgFile.wstring() +
        L"\n\n如果 CS2 正在运行，请重启游戏。";
    return true;
}

void InstallConfig() {
    if (const auto detected = DetectCs2Path()) {
        const std::wstring question = L"已自动检测到 CS2：\n\n" + detected->wstring() +
            L"\n\n是否使用此位置安装监听配置？\n选择“否”可手动选择其他 cs2.exe。";
        const int choice = MessageBoxW(g.window, question.c_str(), L"检测到 CS2", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return;
        if (choice == IDYES) {
            std::wstring message;
            const bool ok = WriteGsiConfig(*detected, message);
            if (ok) {
                g.settings.cs2Path = detected->wstring();
                SaveSettings();
            }
            MessageBoxW(g.window, message.c_str(), ok ? L"安装完成" : L"安装失败",
                ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return;
        }
    } else {
        MessageBoxW(g.window, L"未能从正在运行的进程或 Steam 游戏库自动找到 CS2。\n接下来请手动选择 cs2.exe。",
            L"未检测到 CS2", MB_OK | MB_ICONINFORMATION);
    }

    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g.window;
    dialog.lpstrTitle = L"选择 CS2 的 cs2.exe";
    dialog.lpstrFilter = L"CS2 (cs2.exe)\0cs2.exe\0程序文件 (*.exe)\0*.exe\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        std::wstring message;
        const bool ok = WriteGsiConfig(path, message);
        if (ok) {
            g.settings.cs2Path = path;
            SaveSettings();
        }
        MessageBoxW(g.window, message.c_str(), ok ? L"安装完成" : L"安装失败", ok ? MB_ICONINFORMATION : MB_ICONERROR);
    }
}

LRESULT CALLBACK HelpWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        HWND heading = CreateWindowExW(0, L"STATIC", L"帮助与使用说明", WS_CHILD | WS_VISIBLE,
            24, 20, 500, 32, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g.titleFont), TRUE);
        constexpr wchar_t helpText[] =
            L"合规说明\r\n"
            L"本程序只使用 Valve 公开提供的 Game State Integration（GSI）接收本机游戏状态，"
            L"不会注入或修改 CS2，不读取游戏内存，不自动瞄准、移动或射击，也不绕过 VAC。"
            L"从当前实现看，它不具备外挂功能。\r\n\r\n"
            L"但本程序不是 Valve、Steam、完美世界或任何赛事平台的官方软件，也没有获得官方合规认证。"
            L"平台规则和判定方式可能变化，因此无法承诺在所有平台、赛事或未来版本中一定被允许；"
            L"使用前请自行确认所在平台和赛事规则。\r\n\r\n"
            L"测试阶段提示\r\n"
            L"当前版本仍在测试。死亡检测、切换窗口和下一回合切回可能受网络、全屏模式、系统权限"
            L"或游戏更新影响而失败或延迟。不建议正在排位、冲分或重视比赛结果的玩家使用，"
            L"建议先在休闲模式或练习环境中测试。\r\n\r\n"
            L"隐私与网络\r\n"
            L"监听服务只绑定 127.0.0.1:3000，不接受局域网或互联网连接。"
            L"设置保存在当前用户的 %LOCALAPPDATA%\\CSMoyu\\settings.ini。";
        HWND content = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", helpText,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            24, 62, 532, 298, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(content, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
        HWND close = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            456, 374, 100, 34, hwnd, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_NCDESTROY: g.helpWindow = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void ShowHelp() {
    if (g.helpWindow) {
        ShowWindow(g.helpWindow, SW_RESTORE);
        SetForegroundWindow(g.helpWindow);
        return;
    }
    RECT desired{0, 0, 580, 440};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    g.helpWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, kHelpClassName, L"CS2 摸鱼切换器 - 帮助",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
        g.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g.helpWindow) ShowWindow(g.helpWindow, SW_SHOW);
}

HWND AddControl(const wchar_t* klass, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

void CreateUi(HWND hwnd) {
    g.window = hwnd;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g.font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW title = metrics.lfMessageFont;
    title.lfHeight = -24;
    title.lfWeight = FW_SEMIBOLD;
    g.titleFont = CreateFontIndirectW(&title);
    g.background = CreateSolidBrush(RGB(247, 248, 250));

    HWND heading = AddControl(L"STATIC", L"CS2 摸鱼切换器", SS_LEFT, 28, 22, 400, 34, 0);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g.titleFont), TRUE);
    AddControl(L"BUTTON", L"帮助", BS_PUSHBUTTON | WS_TABSTOP, 476, 22, 80, 30, IDC_HELP_BUTTON);
    AddControl(L"STATIC", L"当本机玩家死亡时，立即执行选定动作。", SS_LEFT, 29, 60, 500, 22, 0);

    AddControl(L"BUTTON", L" 死亡后的动作 ", BS_GROUPBOX, 22, 94, 536, 178, 0);
    g.modeProgram = AddControl(L"BUTTON", L"切换到程序", BS_AUTORADIOBUTTON | WS_GROUP, 42, 124, 150, 24, IDC_MODE_PROGRAM);
    g.modeHotkey = AddControl(L"BUTTON", L"发送快捷键", BS_AUTORADIOBUTTON, 208, 124, 150, 24, IDC_MODE_HOTKEY);
    g.target = AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 42, 160, 394, 30, IDC_TARGET);
    g.browse = AddControl(L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, 446, 159, 88, 32, IDC_BROWSE);
    AddControl(L"STATIC", L"目标程序已运行时切换窗口，否则启动它。", SS_LEFT, 42, 196, 450, 20, 0);
    g.hotkey = AddControl(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY | WS_BORDER | WS_TABSTOP, 42, 226, 492, 30, IDC_HOTKEY);

    AddControl(L"BUTTON", L" 死亡时暂停媒体 ", BS_GROUPBOX, 22, 286, 536, 72, 0);
    g.pauseMusic = AddControl(L"BUTTON", L"暂停音乐播放器", BS_AUTOCHECKBOX | WS_TABSTOP,
        42, 312, 160, 24, IDC_PAUSE_MUSIC);
    g.pauseVideo = AddControl(L"BUTTON", L"暂停网页视频（哔哩哔哩 / 抖音 / 西瓜视频）",
        BS_AUTOCHECKBOX | WS_TABSTOP, 218, 312, 320, 24, IDC_PAUSE_VIDEO);

    g.install = AddControl(L"BUTTON", L"安装 CS2 监听配置…", BS_PUSHBUTTON | WS_TABSTOP, 22, 376, 202, 38, IDC_INSTALL);
    g.start = AddControl(L"BUTTON", L"开始监听", BS_DEFPUSHBUTTON | WS_TABSTOP, 356, 376, 202, 38, IDC_START);
    g.status = AddControl(L"STATIC", L"尚未开始", SS_LEFT | SS_CENTERIMAGE, 28, 433, 520, 28, IDC_STATUS);

    g.oldHotkeyProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.hotkey, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HotkeyProc)));
    RefreshControls();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: CreateUi(hwnd); return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), RGB(247, 248, 250));
        return reinterpret_cast<LRESULT>(g.background);
    case WM_ERASEBKGND: {
        RECT rect{}; GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, g.background);
        return 1;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int notification = HIWORD(wp);
        if (id == IDC_MODE_PROGRAM || id == IDC_MODE_HOTKEY) {
            if (notification == BN_CLICKED) {
                g.settings.programMode = id == IDC_MODE_PROGRAM;
                RefreshControls();
                SaveUiSettings();
            }
            return 0;
        }
        if (id == IDC_PAUSE_MUSIC || id == IDC_PAUSE_VIDEO) {
            g.settings.pauseMusic = SendMessageW(g.pauseMusic, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g.settings.pauseVideo = SendMessageW(g.pauseVideo, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveSettings(); return 0;
        }
        if (id == IDC_BROWSE) { BrowseTarget(); return 0; }
        if (id == IDC_INSTALL) { InstallConfig(); return 0; }
        if (id == IDC_HELP_BUTTON) { ShowHelp(); return 0; }
        if (id == IDC_START) {
            SaveUiSettings();
            if (g.listening) StopServer(); else StartServer();
            return 0;
        }
        if (id == IDC_TARGET && notification == EN_KILLFOCUS) {
            wchar_t target[32768]{}; GetWindowTextW(g.target, target, 32768);
            g.settings.target = target; SaveSettings();
        }
        break;
    }
    case WM_GSI_EVENT:
        if (static_cast<GsiEvent>(wp) == GsiEvent::Died) TriggerAction();
        else if (static_cast<GsiEvent>(wp) == GsiEvent::RoundStarted) ReturnToGame(ReturnReason::RoundStarted);
        else if (static_cast<GsiEvent>(wp) == GsiEvent::GameEnded) ReturnToGame(ReturnReason::GameEnded);
        else if (static_cast<GsiEvent>(wp) == GsiEvent::WarmupEnded) ReturnToGame(ReturnReason::WarmupEnded);
        return 0;
    case WM_TIMER:
        if (wp == kReturnToGameTimer) {
            AttemptReturnToGame();
            return 0;
        }
        break;
    case WM_GSI_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        SetStatus(*text); delete text;
        if (!wp) SetWindowTextW(g.start, L"开始监听");
        return 0;
    }
    case WM_QUERYENDSESSION:
        SaveUiSettings();
        return TRUE;
    case WM_CLOSE:
        SaveUiSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        StopServer();
        if (g.oldHotkeyProc && IsWindow(g.hotkey)) SetWindowLongPtrW(g.hotkey, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.oldHotkeyProc));
        DeleteObject(g.font); DeleteObject(g.titleFont); DeleteObject(g.background);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    LoadSettings();

    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hIconSm = cls.hIcon;
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = kClassName;
    if (!RegisterClassExW(&cls)) return 1;

    WNDCLASSEXW helpCls{sizeof(helpCls)};
    helpCls.lpfnWndProc = HelpWindowProc;
    helpCls.hInstance = instance;
    helpCls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    helpCls.hIcon = cls.hIcon;
    helpCls.hIconSm = cls.hIconSm;
    helpCls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    helpCls.lpszClassName = kHelpClassName;
    if (!RegisterClassExW(&helpCls)) return 1;

    RECT desired{0, 0, 580, 505};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    HWND window = CreateWindowExW(0, kClassName, L"CS2 摸鱼切换器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
