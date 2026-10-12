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

#pragma once

// Follows an unmodified MPC-HC from the outside:
//  - the player's main window (class "MediaPlayerClassicW") and its video area,
//  - the playback state, position and file through the built-in web interface (/variables.html),
//  - the current frame through /snapshot.jpg and commands through WM_COMMAND.

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Host
{
    struct PlayerState {
        HWND mainWnd = nullptr;     // nullptr: no player running
        DWORD pid = 0;
        std::wstring exePath;
        bool webAvailable = false;  // the web interface answered
        int state = 0;              // MpcPlayState
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        ULONGLONG positionTick = 0; // GetTickCount64 when positionMs was read
        std::wstring file;          // title / file name
        std::wstring filePath;      // full path or URL

        bool SameExceptPosition(const PlayerState& o) const
        {
            return mainWnd == o.mainWnd && pid == o.pid && webAvailable == o.webAvailable && state == o.state
                   && file == o.file && filePath == o.filePath && durationMs == o.durationMs;
        }
    };

    class PlayerBridge
    {
    public:
        static constexpr wchar_t kPlayerClass[] = L"MediaPlayerClassicW";

        // notifyMsg is posted to notifyWnd whenever the state changes (other than the position)
        PlayerBridge(HWND notifyWnd, UINT notifyMsg);
        ~PlayerBridge();
        PlayerBridge(const PlayerBridge&) = delete;
        PlayerBridge& operator=(const PlayerBridge&) = delete;

        void SetPort(int port);
        int GetPort() const { return m_port; }
        PlayerState GetState() const;
        // Position extrapolated while playing
        int64_t GetPositionMs() const;
        // Polls immediately (e.g. right after launching the player)
        void Wake();

        // Blocking: current frame as a top-down 32 bpp DIB (BITMAPINFOHEADER + pixels)
        bool CaptureFrame(std::vector<BYTE>& dib, std::wstring& error);
        bool SendCommand(int command);

        // Geometry, cheap, for the UI thread
        static HWND FindFullscreenWindow(HWND mainWnd);
        static HWND GetOverlayOwner(HWND mainWnd);
        static bool GetVideoRect(HWND mainWnd, RECT& rc);

    private:
        void Run();
        HWND FindPlayer(HWND previous) const;
        bool Poll(PlayerState& st);
        bool HttpGet(const wchar_t* path, std::string& body, DWORD timeoutMs, int* status = nullptr);

        HWND m_notifyWnd;
        UINT m_notifyMsg;
        std::atomic<int> m_port{ 13579 };
        mutable std::mutex m_lock;
        PlayerState m_state;
        std::mutex m_wakeLock;
        std::condition_variable m_wake;
        bool m_wakeFlag = false;
        std::atomic<bool> m_stop{ false };
        void* m_session = nullptr;
        std::thread m_thread;
    };
}
