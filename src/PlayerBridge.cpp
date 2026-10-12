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

#include "PlayerBridge.h"
#include "Common.h"
#include "../sdk/MpcPlugin.h"
#include <winhttp.h>
#include <wincodec.h>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace Host
{
    namespace
    {
        constexpr UINT kViewId = 0xE900; // AFX_IDW_PANE_FIRST: the video view of the main frame

        std::wstring HtmlDecode(const std::string& utf8)
        {
            std::wstring s = Utf8ToWide(utf8), out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] != L'&') {
                    out += s[i];
                    continue;
                }
                const size_t semi = s.find(L';', i);
                if (semi == std::wstring::npos || semi - i > 10) {
                    out += s[i];
                    continue;
                }
                const std::wstring ent = s.substr(i + 1, semi - i - 1);
                wchar_t c = 0;
                if (ent == L"amp") {
                    c = L'&';
                } else if (ent == L"lt") {
                    c = L'<';
                } else if (ent == L"gt") {
                    c = L'>';
                } else if (ent == L"quot") {
                    c = L'"';
                } else if (ent == L"apos") {
                    c = L'\'';
                } else if (ent.size() > 1 && ent[0] == L'#') {
                    c = (wchar_t)((ent[1] == L'x' || ent[1] == L'X') ? wcstoul(ent.c_str() + 2, nullptr, 16) : wcstoul(ent.c_str() + 1, nullptr, 10));
                }
                if (c) {
                    out += c;
                    i = semi;
                } else {
                    out += s[i];
                }
            }
            return out;
        }

        // <p id="name">value</p>
        bool Field(const std::string& body, const char* id, std::wstring& value)
        {
            const std::string open = std::string("id=\"") + id + "\">";
            const size_t a = body.find(open);
            if (a == std::string::npos) {
                return false;
            }
            const size_t start = a + open.size();
            const size_t end = body.find("</p>", start);
            if (end == std::string::npos) {
                return false;
            }
            value = HtmlDecode(body.substr(start, end - start));
            return true;
        }

        struct FindData {
            DWORD pid;
            HWND main;
            HWND found;
        };
    }

    PlayerBridge::PlayerBridge(HWND notifyWnd, UINT notifyMsg)
        : m_notifyWnd(notifyWnd)
        , m_notifyMsg(notifyMsg)
    {
        m_session = WinHttpOpen(L"MpcPluginHost/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        m_thread = std::thread([this] { Run(); });
    }

    PlayerBridge::~PlayerBridge()
    {
        m_stop = true;
        Wake();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_session) {
            WinHttpCloseHandle(m_session);
        }
    }

    void PlayerBridge::SetPort(int port)
    {
        if (port > 0 && port <= 65535 && port != m_port) {
            m_port = port;
            Wake();
        }
    }

    PlayerState PlayerBridge::GetState() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_state;
    }

    int64_t PlayerBridge::GetPositionMs() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        int64_t pos = m_state.positionMs;
        if (m_state.state == MPC_PLAYSTATE_PLAYING) {
            pos += (int64_t)(GetTickCount64() - m_state.positionTick);
            if (m_state.durationMs > 0) {
                pos = std::min(pos, m_state.durationMs);
            }
        }
        return pos;
    }

    void PlayerBridge::Wake()
    {
        {
            std::lock_guard<std::mutex> lock(m_wakeLock);
            m_wakeFlag = true;
        }
        m_wake.notify_all();
    }

    HWND PlayerBridge::FindPlayer(HWND previous) const
    {
        HWND fg = GetForegroundWindow();
        if (fg) {
            wchar_t cls[64];
            HWND root = GetAncestor(fg, GA_ROOTOWNER);
            if (root && GetClassNameW(root, cls, _countof(cls)) && !wcscmp(cls, kPlayerClass)) {
                return root;
            }
        }
        if (previous && IsWindow(previous)) {
            return previous;
        }
        return FindWindowW(kPlayerClass, nullptr);
    }

    bool PlayerBridge::HttpGet(const wchar_t* path, std::string& body, DWORD timeoutMs, int* status)
    {
        body.clear();
        if (!m_session) {
            return false;
        }
        bool ok = false;
        HINTERNET connect = WinHttpConnect(m_session, L"127.0.0.1", (INTERNET_PORT)m_port.load(), 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        if (request) {
            WinHttpSetTimeouts(request, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                if (status) {
                    *status = (int)code;
                }
                ok = true;
                for (;;) {
                    char buf[16384];
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buf, sizeof(buf), &read)) {
                        ok = false;
                        break;
                    }
                    if (!read) {
                        break;
                    }
                    body.append(buf, read);
                }
                ok = ok && code == 200;
            }
            WinHttpCloseHandle(request);
        }
        if (connect) {
            WinHttpCloseHandle(connect);
        }
        return ok;
    }

    bool PlayerBridge::Poll(PlayerState& st)
    {
        std::string body;
        if (!HttpGet(L"/variables.html", body, 800)) {
            return false;
        }
        std::wstring state, pos, dur;
        if (!Field(body, "state", state) || !Field(body, "position", pos)) {
            return false;
        }
        Field(body, "file", st.file);
        Field(body, "filepath", st.filePath);
        Field(body, "duration", dur);
        // OAFilterState: -1 nothing loaded, 0 stopped, 1 paused, 2 running
        switch (_wtoi(state.c_str())) {
            case 0:
                st.state = MPC_PLAYSTATE_STOPPED;
                break;
            case 1:
                st.state = MPC_PLAYSTATE_PAUSED;
                break;
            case 2:
                st.state = MPC_PLAYSTATE_PLAYING;
                break;
            default:
                st.state = MPC_PLAYSTATE_NONE;
                break;
        }
        if (st.state == MPC_PLAYSTATE_NONE) {
            st.file.clear();
            st.filePath.clear();
        }
        st.positionMs = _wtoi64(pos.c_str());
        st.durationMs = _wtoi64(dur.c_str());
        st.positionTick = GetTickCount64();
        return true;
    }

    void PlayerBridge::Run()
    {
        HWND previous = nullptr;
        while (!m_stop) {
            PlayerState st;
            st.mainWnd = FindPlayer(previous);
            if (st.mainWnd) {
                GetWindowThreadProcessId(st.mainWnd, &st.pid);
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    if (m_state.pid == st.pid) {
                        st.exePath = m_state.exePath;
                    }
                }
                if (st.exePath.empty()) {
                    st.exePath = ProcessImagePath(st.pid);
                }
                st.webAvailable = Poll(st);
            }
            previous = st.mainWnd;

            bool changed;
            {
                std::lock_guard<std::mutex> lock(m_lock);
                changed = !st.SameExceptPosition(m_state);
                m_state = st;
            }
            if (changed) {
                PostMessageW(m_notifyWnd, m_notifyMsg, 0, 0);
            }

            // fast while a player is there (pause must be noticed quickly), slow otherwise
            const DWORD interval = st.mainWnd ? (st.webAvailable ? 200 : 1000) : 1000;
            std::unique_lock<std::mutex> lock(m_wakeLock);
            m_wake.wait_for(lock, std::chrono::milliseconds(interval), [this] { return m_wakeFlag; });
            m_wakeFlag = false;
        }
    }

    bool PlayerBridge::CaptureFrame(std::vector<BYTE>& dib, std::wstring& error)
    {
        dib.clear();
        std::string jpeg;
        int status = 0;
        if (!HttpGet(L"/snapshot.jpg", jpeg, 3000, &status) || jpeg.empty()) {
            error = status == 403 ? L"The preview of the player's web interface is disabled"
                    : L"Unable to get the current frame from the player";
            return false;
        }

        bool ok = false;
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
                && SUCCEEDED(factory->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory((BYTE*)jpeg.data(), (DWORD)jpeg.size()))
                && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            converter->GetSize(&w, &h);
            if (w && h) {
                dib.resize(sizeof(BITMAPINFOHEADER) + size_t(w) * h * 4);
                auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
                *bih = {};
                bih->biSize = sizeof(BITMAPINFOHEADER);
                bih->biWidth = (LONG)w;
                bih->biHeight = -(LONG)h;
                bih->biPlanes = 1;
                bih->biBitCount = 32;
                bih->biCompression = BI_RGB;
                ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * h * 4, dib.data() + sizeof(BITMAPINFOHEADER)));
            }
        }
        for (IUnknown* p : std::initializer_list<IUnknown*> { converter, frame, decoder, stream, factory }) {
            if (p) {
                p->Release();
            }
        }
        if (!ok) {
            dib.clear();
            error = L"Unable to decode the current frame";
        }
        return ok;
    }

    bool PlayerBridge::SendCommand(int command)
    {
        HWND wnd = GetState().mainWnd;
        return wnd && command > 0 && PostMessageW(wnd, WM_COMMAND, (WPARAM)command, 0);
    }

    HWND PlayerBridge::FindFullscreenWindow(HWND mainWnd)
    {
        // the dedicated fullscreen window (fullscreen on another monitor / D3D fullscreen): a visible, topmost
        // tool window of the player process covering a whole monitor
        FindData data{ 0, mainWnd, nullptr };
        GetWindowThreadProcessId(mainWnd, &data.pid);
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* d = reinterpret_cast<FindData*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != d->pid || hwnd == d->main || !IsWindowVisible(hwnd)) {
                return TRUE;
            }
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (!(ex & WS_EX_TOPMOST) || !(ex & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }
            RECT rc;
            MONITORINFO mi{ sizeof(mi) };
            if (GetWindowRect(hwnd, &rc) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)
                    && EqualRect(&rc, &mi.rcMonitor)) {
                d->found = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&data));
        return data.found;
    }

    HWND PlayerBridge::GetOverlayOwner(HWND mainWnd)
    {
        if (!mainWnd || !IsWindow(mainWnd)) {
            return nullptr;
        }
        HWND fs = FindFullscreenWindow(mainWnd);
        return fs ? fs : mainWnd;
    }

    bool PlayerBridge::GetVideoRect(HWND mainWnd, RECT& rc)
    {
        SetRectEmpty(&rc);
        if (!mainWnd || !IsWindow(mainWnd)) {
            return false;
        }
        if (HWND fs = FindFullscreenWindow(mainWnd)) {
            return GetWindowRect(fs, &rc) && !IsRectEmpty(&rc);
        }
        if (!IsWindowVisible(mainWnd) || IsIconic(mainWnd)) {
            return false;
        }
        HWND view = GetDlgItem(mainWnd, kViewId);
        if (!view || !IsWindowVisible(view)) {
            return false;
        }
        return GetWindowRect(view, &rc) && !IsRectEmpty(&rc);
    }
}
