#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
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
constexpr UINT WM_GSI_STATUS = WM_APP + 1;
constexpr UINT WM_GSI_EVENT = WM_APP + 2;
constexpr int kPort = 3000;

enum class GsiEvent : WPARAM { Died = 1, Respawned = 2 };

enum ControlId {
    IDC_MODE_PROGRAM = 1001, IDC_MODE_HOTKEY, IDC_TARGET, IDC_BROWSE,
    IDC_HOTKEY, IDC_START, IDC_INSTALL, IDC_STATUS
};

struct Settings {
    bool programMode = false;
    std::wstring target;
    std::wstring cs2Path;
    UINT key = VK_TAB;
    bool ctrl = false, alt = true, shift = false, win = false;
};

struct AppState {
    HWND window{};
    HWND modeProgram{}, modeHotkey{}, target{}, browse{}, hotkey{}, start{}, install{}, status{};
    HFONT font{}, titleFont{};
    HBRUSH background{};
    Settings settings;
    std::atomic<bool> listening{false};
    std::atomic<bool> stopRequested{false};
    std::thread serverThread;
    std::atomic<SOCKET> listenSocket{INVALID_SOCKET};
    int ownPreviousHealth = -1;
    bool waitingForRespawn = false;
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
        if (g.ownPreviousHealth > 0 && !g.waitingForRespawn) {
            g.waitingForRespawn = true;
            PostMessageW(g.window, WM_GSI_EVENT, static_cast<WPARAM>(GsiEvent::Died), 0);
        }
        g.ownPreviousHealth = 0;
    } else {
        if (g.waitingForRespawn) {
            g.waitingForRespawn = false;
            PostMessageW(g.window, WM_GSI_EVENT, static_cast<WPARAM>(GsiEvent::Respawned), 0);
        }
        g.ownPreviousHealth = *health;
    }
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
        if (body != std::string::npos) HandlePayload(request.substr(body + 4));
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
    g.waitingForRespawn = false;
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

void ReturnToGame() {
    const HWND cs2 = FindCs2Window();
    if (cs2) {
        ActivateWindow(cs2);
        SetStatus(L"检测到复活 · 已切换回 CS2");
    } else {
        SetStatus(L"检测到复活，但未找到 CS2 窗口");
    }
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
    const fs::path cfgFile = cfgDir / L"gamestate_integration_csmoyu.cfg";
    std::error_code ec;
    fs::create_directories(cfgDir, ec);
    std::ofstream file(cfgFile, std::ios::binary | std::ios::trunc);
    if (!file) {
        result = L"无法写入：" + cfgFile.wstring() + L"\n请检查目录权限。";
        return false;
    }
    file << "\"CSMoyu Integration\"\n{\n"
            "  \"uri\" \"http://127.0.0.1:3000\"\n"
            "  \"timeout\" \"1.0\"\n"
            "  \"buffer\" \"0.0\"\n"
            "  \"throttle\" \"0.1\"\n"
            "  \"heartbeat\" \"10.0\"\n"
            "  \"data\"\n  {\n"
            "    \"provider\" \"1\"\n"
            "    \"player_id\" \"1\"\n"
            "    \"player_state\" \"1\"\n"
            "  }\n}\n";
    file.close();
    result = L"配置已安装：\n" + cfgFile.wstring() + L"\n\n如果 CS2 正在运行，请重启游戏。";
    return true;
}

void InstallConfig() {
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
    AddControl(L"STATIC", L"当本机玩家死亡时，立即执行选定动作。", SS_LEFT, 29, 60, 500, 22, 0);

    AddControl(L"BUTTON", L" 死亡后的动作 ", BS_GROUPBOX, 22, 94, 536, 178, 0);
    g.modeProgram = AddControl(L"BUTTON", L"切换到程序", BS_AUTORADIOBUTTON | WS_GROUP, 42, 124, 150, 24, IDC_MODE_PROGRAM);
    g.modeHotkey = AddControl(L"BUTTON", L"发送快捷键", BS_AUTORADIOBUTTON, 208, 124, 150, 24, IDC_MODE_HOTKEY);
    g.target = AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 42, 160, 394, 30, IDC_TARGET);
    g.browse = AddControl(L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, 446, 159, 88, 32, IDC_BROWSE);
    AddControl(L"STATIC", L"目标程序已运行时切换窗口，否则启动它。", SS_LEFT, 42, 196, 450, 20, 0);
    g.hotkey = AddControl(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY | WS_BORDER | WS_TABSTOP, 42, 226, 492, 30, IDC_HOTKEY);

    g.install = AddControl(L"BUTTON", L"安装 CS2 监听配置…", BS_PUSHBUTTON | WS_TABSTOP, 22, 291, 202, 38, IDC_INSTALL);
    g.start = AddControl(L"BUTTON", L"开始监听", BS_DEFPUSHBUTTON | WS_TABSTOP, 356, 291, 202, 38, IDC_START);
    g.status = AddControl(L"STATIC", L"尚未开始", SS_LEFT | SS_CENTERIMAGE, 28, 348, 520, 28, IDC_STATUS);

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
            g.settings.programMode = id == IDC_MODE_PROGRAM;
            SaveSettings(); RefreshControls(); return 0;
        }
        if (id == IDC_BROWSE) { BrowseTarget(); return 0; }
        if (id == IDC_INSTALL) { InstallConfig(); return 0; }
        if (id == IDC_START) {
            wchar_t target[32768]{};
            GetWindowTextW(g.target, target, 32768);
            g.settings.target = target;
            SaveSettings();
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
        else if (static_cast<GsiEvent>(wp) == GsiEvent::Respawned) ReturnToGame();
        return 0;
    case WM_GSI_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        SetStatus(*text); delete text;
        if (!wp) SetWindowTextW(g.start, L"开始监听");
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
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

    RECT desired{0, 0, 580, 420};
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
