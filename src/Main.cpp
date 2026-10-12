/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// MpcPluginHost: runs plugins (Plugins\*.dll) alongside an unmodified MPC-HC.
//
//   MpcPluginHost.exe                      start the host (tray icon), it attaches to any running player
//   MpcPluginHost.exe --launch [args...]   start MPC-HC with its web interface enabled for this session,
//                                          passing args (files, switches), and start the host if needed
//   MpcPluginHost.exe --player <exe>       remember the player executable to launch
//   MpcPluginHost.exe --exit               close the running host

#include "Common.h"
#include "PlayerBridge.h"
#include "PluginManager.h"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <memory>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Host;

namespace
{
    constexpr wchar_t kWindowClass[] = L"MpcPluginHost";
    constexpr wchar_t kMutexName[] = L"Local\\MpcPluginHost.7C1E2B52";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValue[] = L"MpcPluginHost";
    constexpr wchar_t kHostSection[] = L"Host";
    constexpr int kDefaultPort = 13579;

    constexpr UINT WM_BRIDGE_STATE = WM_APP + 1;
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT_PTR kScanTimer = 1;
    constexpr UINT_PTR kLayoutTimer = 2;
    constexpr UINT_PTR kWebCheckTimer = 3;

    enum MenuId : UINT {
        ID_STATUS = 1,
        ID_OPEN_FOLDER,
        ID_LAUNCH,
        ID_ENABLE_WEB,
        ID_AUTOSTART,
        ID_OPEN_LOG,
        ID_EXIT,
        ID_PLUGIN_FIRST = 100,
    };

    std::wstring g_hostDir, g_dataDir;

    std::wstring QuoteArg(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
            return arg;
        }
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') {
                slashes++;
            } else if (c == L'"') {
                out.append(slashes * 2 + 1, L'\\');
                slashes = 0;
            } else {
                slashes = 0;
            }
            out += c;
        }
        out.append(slashes, L'\\');
        out += L'"';
        return out;
    }

    bool IsAutostart()
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = sizeof(buf);
        return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
    }

    void SetAutostart(bool on)
    {
        if (on) {
            const std::wstring cmd = QuoteArg(ModulePath(nullptr));
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            HKEY key;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, kRunValue);
                RegCloseKey(key);
            }
        }
    }

    // The player executable: remembered, running, next to the host or in the usual install folders
    std::wstring FindPlayerExe()
    {
        std::wstring exe = Settings::Get().GetString(kHostSection, L"PlayerPath");
        if (FileExists(exe)) {
            return exe;
        }
        if (HWND wnd = FindWindowW(PlayerBridge::kPlayerClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(wnd, &pid);
            exe = ProcessImagePath(pid);
            if (FileExists(exe)) {
                return exe;
            }
        }
        std::vector<std::wstring> candidates = { g_hostDir, g_hostDir + L"..\\" };
        for (auto folder : { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 }) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &path))) {
                candidates.push_back(WithSlash(path) + L"MPC-HC\\");
                CoTaskMemFree(path);
            }
        }
        for (const auto& dir : candidates) {
            for (const wchar_t* name : { L"mpc-hc64.exe", L"mpc-hc.exe" }) {
                if (FileExists(dir + name)) {
                    wchar_t full[MAX_PATH * 2];
                    GetFullPathNameW((dir + name).c_str(), _countof(full), full, nullptr);
                    return full;
                }
            }
        }
        return std::wstring();
    }

    std::wstring AskPlayerExe(HWND owner)
    {
        wchar_t file[MAX_PATH * 2] = L"mpc-hc64.exe";
        OPENFILENAMEW ofn{ sizeof(ofn) };
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"MPC-HC (mpc-hc*.exe)\0mpc-hc*.exe\0Programs (*.exe)\0*.exe\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = _countof(file);
        ofn.lpstrTitle = L"Where is MPC-HC?";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&ofn)) {
            return std::wstring();
        }
        Settings::Get().SetString(kHostSection, L"PlayerPath", file);
        return file;
    }

    // Port of the web interface: the player's own setting when enabled, otherwise the one the host enables
    // with /webport when it launches the player
    int WebPortFor(const std::wstring& exe, bool* enabledInPlayer = nullptr)
    {
        PlayerConfig cfg(exe);
        const bool enabled = cfg.GetInt(L"EnableWebServer", 0) != 0;
        if (enabledInPlayer) {
            *enabledInPlayer = enabled;
        }
        return enabled ? cfg.GetInt(L"WebServerPort", kDefaultPort) : Settings::Get().GetInt(kHostSection, L"WebPort", kDefaultPort);
    }

    bool LaunchPlayer(HWND owner, const std::wstring& args)
    {
        std::wstring exe = FindPlayerExe();
        if (exe.empty()) {
            exe = AskPlayerExe(owner);
            if (exe.empty()) {
                return false;
            }
        }
        bool webEnabled = false;
        const int port = WebPortFor(exe, &webEnabled);
        const bool running = FindWindowW(PlayerBridge::kPlayerClass, nullptr) != nullptr;
        std::wstring cmd = QuoteArg(exe);
        if (!webEnabled) {
            PlayerConfig cfg(exe);
            if (!running && !cfg.GetInt(L"WebServerLocalhostOnly", 0)) {
                // the interface is only enabled for the host: don't expose it to the network
                cfg.SetInt(L"WebServerLocalhostOnly", 1);
                Log(L"Restricted the player's web interface to localhost");
            }
            cmd += L" /webport " + std::to_wstring(port);
        }
        if (!args.empty()) {
            cmd += L" " + args;
        }
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) {
            Log(L"Unable to start %s (error %lu)", exe.c_str(), GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"Started %s", cmd.c_str());
        return true;
    }

    bool SafeDispatch(const MSG* msg)
    {
        __try {
            DispatchMessageW(msg);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // -------------------------------------------------------------------------------------------------------------

    class App : public HostServices
    {
    public:
        bool Init(HINSTANCE inst);
        int Run();

        // HostServices
        HWND GetMainWindow() override { return m_state.mainWnd; }
        HWND GetOverlayOwner() override { return PlayerBridge::GetOverlayOwner(m_state.mainWnd); }
        bool GetVideoRect(RECT& rc) override { return HasPlayer() && PlayerBridge::GetVideoRect(m_state.mainWnd, rc); }
        int GetPlayState() override { return HasPlayer() ? m_state.state : MPC_PLAYSTATE_NONE; }
        int64_t GetPositionMs() override { return m_bridge ? m_bridge->GetPositionMs() : 0; }
        int64_t GetDurationMs() override { return m_state.durationMs; }
        std::wstring GetMediaTitle() override { return m_state.filePath.empty() ? m_state.file : m_state.filePath; }
        std::wstring GetMediaPath() override;
        bool CaptureFrame(std::vector<BYTE>& dib, std::wstring& error) override { return m_bridge && m_bridge->CaptureFrame(dib, error); }
        std::wstring GetProgramDir() override { return DirOf(m_state.exePath); }
        std::wstring GetDataDir() override { return g_dataDir; }
        UINT GetDpi() override;
        DWORD GetPlayerProcessId() override { return m_state.pid; }
        bool SendCommand(int command) override { return m_bridge && m_bridge->SendCommand(command); }
        std::vector<std::pair<int, intptr_t>> GetInitialEvents() override;

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
        bool HasPlayer() const { return m_state.mainWnd && m_state.webAvailable; }
        static bool HasMedia(const PlayerState& s) { return s.mainWnd && s.webAvailable && s.state != MPC_PLAYSTATE_NONE; }
        void OnBridgeState();
        void CheckLayout();
        void UpdateTray(bool add = false);
        void Notify(const wchar_t* title, const wchar_t* text);
        void ShowMenu();
        void OnCommand(UINT id);

        HWND m_wnd = nullptr;
        UINT m_taskbarCreated = 0;
        std::unique_ptr<PlayerBridge> m_bridge;
        std::unique_ptr<PluginManager> m_plugins;
        PlayerState m_state;
        HWND m_layoutOwner = nullptr;
        RECT m_layoutRect{};
        bool m_layoutVisible = false;
        DWORD m_webNoticePid = 0;
        HICON m_icon = nullptr;
        std::wstring m_iconSource;
        std::vector<std::wstring> m_menuPlugins;
    };

    bool App::Init(HINSTANCE inst)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        // a hidden top level window (not message-only: it must receive the TaskbarCreated broadcast)
        m_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"MPC-HC Plugin Host", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!m_wnd) {
            return false;
        }
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        m_bridge = std::make_unique<PlayerBridge>(m_wnd, WM_BRIDGE_STATE);
        const std::wstring exe = FindPlayerExe();
        if (!exe.empty()) {
            m_bridge->SetPort(WebPortFor(exe));
        }

        wchar_t temp[MAX_PATH];
        GetTempPathW(_countof(temp), temp);
        m_plugins = std::make_unique<PluginManager>(*this, g_hostDir + L"Plugins", WithSlash(temp) + L"MpcPluginHost");
        m_plugins->Scan();

        UpdateTray(true);
        SetTimer(m_wnd, kScanTimer, 1000, nullptr);
        SetTimer(m_wnd, kLayoutTimer, 100, nullptr);
        Log(L"Host started (%s), plugins folder %s", ModulePath(nullptr).c_str(), m_plugins->PluginsDir().c_str());
        return true;
    }

    int App::Run()
    {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (m_plugins && m_plugins->PreTranslateMessage(&msg)) {
                continue;
            }
            TranslateMessage(&msg);
            if (!SafeDispatch(&msg)) {
                HMODULE module = msg.hwnd ? (HMODULE)GetClassLongPtrW(msg.hwnd, GCLP_HMODULE) : nullptr;
                Log(L"Exception while dispatching message 0x%04X", msg.message);
                if (m_plugins) {
                    m_plugins->OnCrashInModule(module);
                }
            }
        }
        return (int)msg.wParam;
    }

    std::wstring App::GetMediaPath()
    {
        const std::wstring& path = m_state.filePath;
        if (path.empty() || path.find(L"://") != std::wstring::npos || !FileExists(path)) {
            return std::wstring();
        }
        return path;
    }

    UINT App::GetDpi()
    {
        HWND owner = GetOverlayOwner();
        UINT x = 96, y = 96;
        if (owner && SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y))) {
            return x;
        }
        return 96;
    }

    std::vector<std::pair<int, intptr_t>> App::GetInitialEvents()
    {
        std::vector<std::pair<int, intptr_t>> events;
        if (HasPlayer()) {
            events.emplace_back(MPC_EVENT_PLAYER_CONNECTED, 0);
            if (HasMedia(m_state)) {
                events.emplace_back(MPC_EVENT_MEDIA_OPENED, 0);
                events.emplace_back(MPC_EVENT_PLAYSTATE_CHANGED, m_state.state);
            }
        }
        return events;
    }

    void App::OnBridgeState()
    {
        PlayerState cur = m_bridge->GetState();
        PlayerState prev = m_state;
        const bool hadPlayer = prev.mainWnd && prev.webAvailable;
        const bool hasPlayer = cur.mainWnd && cur.webAvailable;
        auto mediaKey = [](const PlayerState & s) {
            return s.filePath.empty() ? s.file : s.filePath;
        };

        if (cur.pid && cur.pid != prev.pid && !cur.exePath.empty()) {
            Settings::Get().SetString(kHostSection, L"PlayerPath", cur.exePath.c_str());
            m_bridge->SetPort(WebPortFor(cur.exePath));
        }

        // a different player instance: everything about the previous one ends first
        if (hadPlayer && (!hasPlayer || cur.pid != prev.pid)) {
            if (HasMedia(prev)) {
                m_state = prev;
                m_state.state = MPC_PLAYSTATE_NONE;
                m_plugins->Broadcast(MPC_EVENT_PLAYSTATE_CHANGED, MPC_PLAYSTATE_NONE);
                m_plugins->Broadcast(MPC_EVENT_MEDIA_CLOSED, 0);
            }
            m_state = PlayerState();
            m_plugins->Broadcast(MPC_EVENT_PLAYER_DISCONNECTED, 0);
            prev = PlayerState();
            Log(L"Player disconnected");
        }

        m_state = cur;
        if (hasPlayer && !(prev.mainWnd && prev.webAvailable)) {
            Log(L"Connected to the player (pid %lu, port %d)", cur.pid, m_bridge->GetPort());
            m_plugins->Broadcast(MPC_EVENT_PLAYER_CONNECTED, 0);
        }
        if (hasPlayer) {
            const bool hadMedia = HasMedia(prev), hasMedia = HasMedia(cur);
            const bool changed = mediaKey(prev) != mediaKey(cur);
            if (hadMedia && (!hasMedia || changed)) {
                m_plugins->Broadcast(MPC_EVENT_MEDIA_CLOSED, 0);
            }
            if (hasMedia && (!hadMedia || changed)) {
                m_plugins->Broadcast(MPC_EVENT_MEDIA_OPENED, 0);
            }
            const int prevState = (prev.mainWnd && prev.webAvailable) ? prev.state : MPC_PLAYSTATE_NONE;
            if (cur.state != prevState || (hasMedia && changed)) {
                m_plugins->Broadcast(MPC_EVENT_PLAYSTATE_CHANGED, cur.state);
            }
        }

        if (cur.mainWnd && !cur.webAvailable && cur.pid != m_webNoticePid) {
            // give the player a moment to start its web server before complaining
            SetTimer(m_wnd, kWebCheckTimer, 5000, nullptr);
        }
        UpdateTray();
        CheckLayout();
    }

    void App::CheckLayout()
    {
        HWND owner = nullptr;
        RECT rc{};
        bool visible = false;
        if (HasPlayer()) {
            owner = GetOverlayOwner();
            visible = PlayerBridge::GetVideoRect(m_state.mainWnd, rc);
        }
        if (owner != m_layoutOwner || visible != m_layoutVisible || !EqualRect(&rc, &m_layoutRect)) {
            m_layoutOwner = owner;
            m_layoutVisible = visible;
            m_layoutRect = rc;
            m_plugins->Broadcast(MPC_EVENT_LAYOUT_CHANGED, 0);
        }
    }

    void App::UpdateTray(bool add)
    {
        // the player's icon once it is known
        const std::wstring source = !m_state.exePath.empty() ? m_state.exePath : FindPlayerExe();
        if (source != m_iconSource || !m_icon) {
            HICON icon = nullptr;
            if (!source.empty()) {
                ExtractIconExW(source.c_str(), 0, nullptr, &icon, 1);
            }
            if (icon || !m_icon) {
                if (m_icon) {
                    DestroyIcon(m_icon);
                }
                m_icon = icon ? icon : CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
                m_iconSource = source;
            }
        }

        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = m_wnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = m_icon;
        std::wstring tip = L"MPC-HC plugins: ";
        if (HasPlayer()) {
            tip += L"connected";
        } else if (m_state.mainWnd) {
            tip += L"web interface unavailable";
        } else {
            tip += L"waiting for MPC-HC";
        }
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        if (add) {
            Shell_NotifyIconW(NIM_ADD, &nid);
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        } else {
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
    }

    void App::Notify(const wchar_t* title, const wchar_t* text)
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = m_wnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
        wcsncpy_s(nid.szInfo, text, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void App::ShowMenu()
    {
        HMENU menu = CreatePopupMenu();
        std::wstring status;
        if (HasPlayer()) {
            status = L"Connected to MPC-HC";
        } else if (m_state.mainWnd) {
            status = L"MPC-HC found, its web interface is off";
        } else {
            status = L"Waiting for MPC-HC";
        }
        AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        m_menuPlugins.clear();
        const auto plugins = m_plugins->List();
        if (plugins.empty()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No plugins installed");
        }
        for (const auto& p : plugins) {
            std::wstring label = p.name;
            if (!p.version.empty()) {
                label += L" " + p.version;
            }
            if (!p.error.empty()) {
                label += L"  (" + p.error + L")";
            }
            UINT flags = MF_STRING | (p.enabled ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuW(menu, flags, ID_PLUGIN_FIRST + (UINT)m_menuPlugins.size(), label.c_str());
            m_menuPlugins.push_back(p.key);
        }
        AppendMenuW(menu, MF_STRING, ID_OPEN_FOLDER, L"Open plugins folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (m_state.mainWnd ? MF_GRAYED : 0), ID_LAUNCH, L"Start MPC-HC");
        const std::wstring exe = !m_state.exePath.empty() ? m_state.exePath : FindPlayerExe();
        bool webEnabled = false;
        if (!exe.empty()) {
            WebPortFor(exe, &webEnabled);
        }
        AppendMenuW(menu, MF_STRING | (webEnabled ? MF_CHECKED | MF_GRAYED : 0), ID_ENABLE_WEB,
                    L"Always enable MPC-HC's web interface (localhost)");
        AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, ID_OPEN_LOG, L"Open log");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_wnd);
        const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_wnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_wnd, WM_NULL, 0, 0);
        if (id) {
            OnCommand(id);
        }
    }

    void App::OnCommand(UINT id)
    {
        if (id >= ID_PLUGIN_FIRST && id - ID_PLUGIN_FIRST < m_menuPlugins.size()) {
            const std::wstring key = m_menuPlugins[id - ID_PLUGIN_FIRST];
            bool enabled = true;
            for (const auto& p : m_plugins->List()) {
                if (p.key == key) {
                    enabled = p.enabled;
                }
            }
            m_plugins->SetEnabled(key, !enabled);
            return;
        }
        switch (id) {
            case ID_OPEN_FOLDER:
                ShellExecuteW(m_wnd, L"open", m_plugins->PluginsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case ID_LAUNCH:
                if (LaunchPlayer(m_wnd, L"")) {
                    m_bridge->Wake();
                }
                break;
            case ID_ENABLE_WEB: {
                std::wstring exe = !m_state.exePath.empty() ? m_state.exePath : FindPlayerExe();
                if (exe.empty()) {
                    exe = AskPlayerExe(m_wnd);
                }
                if (exe.empty()) {
                    break;
                }
                if (m_state.mainWnd) {
                    MessageBoxW(m_wnd, L"Close MPC-HC first: it saves its settings when it exits.", L"MPC-HC Plugin Host", MB_ICONINFORMATION);
                    break;
                }
                PlayerConfig cfg(exe);
                cfg.SetInt(L"EnableWebServer", 1);
                cfg.SetInt(L"WebServerLocalhostOnly", 1);
                m_bridge->SetPort(WebPortFor(exe));
                Log(L"Enabled the player's web interface (localhost only)");
                Notify(L"MPC-HC plugins", L"The web interface of MPC-HC is now enabled for this computer only. Plugins work however MPC-HC is started.");
                break;
            }
            case ID_AUTOSTART:
                SetAutostart(!IsAutostart());
                break;
            case ID_OPEN_LOG:
                ShellExecuteW(m_wnd, L"open", (g_dataDir + L"MpcPluginHost.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case ID_EXIT:
                DestroyWindow(m_wnd);
                break;
        }
    }

    LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app && app->m_wnd == hwnd) {
            return app->HandleMessage(msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
            case WM_BRIDGE_STATE:
                OnBridgeState();
                return 0;
            case WM_TIMER:
                if (wp == kScanTimer) {
                    m_plugins->Scan();
                } else if (wp == kLayoutTimer) {
                    CheckLayout();
                } else if (wp == kWebCheckTimer) {
                    KillTimer(m_wnd, kWebCheckTimer);
                    if (m_state.mainWnd && !m_state.webAvailable && m_state.pid != m_webNoticePid) {
                        m_webNoticePid = m_state.pid;
                        Log(L"The player's web interface doesn't answer on port %d", m_bridge->GetPort());
                        Notify(L"MPC-HC plugins are waiting",
                               L"Plugins need MPC-HC's web interface. Start MPC-HC from this icon's menu, "
                               L"or choose \"Always enable MPC-HC's web interface\" there.");
                    }
                }
                return 0;
            case WM_TRAY:
                switch (LOWORD(lp)) {
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                        ShowMenu();
                        break;
                }
                return 0;
            case WM_COPYDATA: {
                // forwarded --launch of a second instance
                auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
                if (cds && cds->dwData == 0x4C41554E && cds->cbData % sizeof(wchar_t) == 0) {
                    std::wstring args(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
                    LaunchPlayer(m_wnd, args);
                    m_bridge->Wake();
                }
                return TRUE;
            }
            case WM_DESTROY: {
                KillTimer(m_wnd, kScanTimer);
                KillTimer(m_wnd, kLayoutTimer);
                if (HasMedia(m_state)) {
                    m_plugins->Broadcast(MPC_EVENT_MEDIA_CLOSED, 0);
                }
                m_plugins->UnloadAll();
                m_plugins.reset();
                m_bridge.reset();
                NOTIFYICONDATAW nid{ sizeof(nid) };
                nid.hWnd = m_wnd;
                nid.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                if (m_icon) {
                    DestroyIcon(m_icon);
                }
                Log(L"Host stopped");
                PostQuitMessage(0);
                return 0;
            }
            default:
                if (msg == m_taskbarCreated && m_taskbarCreated) {
                    UpdateTray(true);
                    return 0;
                }
                break;
        }
        return DefWindowProcW(m_wnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_hostDir = DirOf(ModulePath(nullptr));
    if (FileExists(g_hostDir + L"MpcPluginHost.ini")) {
        g_dataDir = g_hostDir; // portable
    } else {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            g_dataDir = WithSlash(local) + L"MPC-HC\\";
            CoTaskMemFree(local);
        } else {
            g_dataDir = g_hostDir;
        }
    }
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    Settings::Get().Init(g_dataDir + L"MpcPluginHost.ini");
    InitLog(g_dataDir + L"MpcPluginHost.log");

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool launch = false, exitHost = false;
    std::wstring launchArgs;
    for (int i = 1; i < argc; i++) {
        const std::wstring a = argv[i];
        if (launch) {
            launchArgs += (launchArgs.empty() ? L"" : L" ") + QuoteArg(a);
        } else if (a == L"--launch") {
            launch = true;
        } else if (a == L"--exit") {
            exitHost = true;
        } else if (a == L"--player" && i + 1 < argc) {
            Settings::Get().SetString(kHostSection, L"PlayerPath", argv[++i]);
        } else {
            // anything else (e.g. a file from an "Open with" association) is passed to the player
            launch = true;
            launchArgs = QuoteArg(a);
        }
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    if (exitHost) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            PostMessageW(other, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (alreadyRunning) {
        if (launch) {
            HWND other = FindWindowW(kWindowClass, nullptr);
            COPYDATASTRUCT cds{ 0x4C41554E, DWORD(launchArgs.size() * sizeof(wchar_t)), (void*)launchArgs.data() };
            DWORD_PTR result = 0;
            if (!other || !SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result)) {
                LaunchPlayer(nullptr, launchArgs);
            }
        }
        return 0;
    }

    App app;
    if (!app.Init(inst)) {
        return 1;
    }
    if (launch) {
        LaunchPlayer(nullptr, launchArgs);
    }
    const int code = app.Run();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CoUninitialize();
    return code;
}
