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

#include "../sdk/MpcPlugin.h"
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Host
{
    // What the host offers to plugins, implemented by the application
    class HostServices
    {
    public:
        virtual ~HostServices() = default;
        virtual HWND GetMainWindow() = 0;
        virtual HWND GetOverlayOwner() = 0;
        virtual bool GetVideoRect(RECT& rc) = 0;
        virtual int GetPlayState() = 0;
        virtual int64_t GetPositionMs() = 0;
        virtual int64_t GetDurationMs() = 0;
        virtual std::wstring GetMediaTitle() = 0;
        virtual std::wstring GetMediaPath() = 0;
        virtual bool CaptureFrame(std::vector<BYTE>& dib, std::wstring& error) = 0;
        virtual std::wstring GetProgramDir() = 0;
        virtual std::wstring GetDataDir() = 0;
        virtual UINT GetDpi() = 0;
        virtual DWORD GetPlayerProcessId() = 0;
        virtual bool SendCommand(int command) = 0;
        // Events describing the current state, sent to a plugin right after it is loaded
        virtual std::vector<std::pair<int, intptr_t>> GetInitialEvents() = 0;
    };

    class PluginManager
    {
    public:
        struct Info {
            std::wstring key;       // file name without extension, used for enabling / disabling
            std::wstring name, version, description, path, error;
            bool enabled = true;
            bool loaded = false;
        };

        PluginManager(HostServices& services, const std::wstring& pluginsDir, const std::wstring& shadowRoot);
        ~PluginManager();
        PluginManager(const PluginManager&) = delete;
        PluginManager& operator=(const PluginManager&) = delete;

        // Picks up new, changed and removed plugin DLLs (call periodically)
        void Scan();
        void Broadcast(int event, intptr_t param);
        bool PreTranslateMessage(const MSG* msg);
        // A plugin crashed in a window procedure: find it by module and disable it
        void OnCrashInModule(HMODULE module);
        void UnloadAll();
        std::vector<Info> List() const;
        void SetEnabled(const std::wstring& key, bool enabled);
        const std::wstring& PluginsDir() const { return m_pluginsDir; }

    private:
        struct Plugin;
        struct Context;

        bool Load(Plugin& p);
        void Unload(Plugin& p);
        void Fail(Plugin& p, const wchar_t* where);
        std::vector<std::tuple<std::wstring, ULONGLONG, ULONGLONG>> FindCandidates() const; // path, size, write time
        static void CleanupShadowRoot(const std::wstring& root);

        // MpcHostApi thunks
        static HWND __cdecl ApiGetMainWindow(void* host);
        static HWND __cdecl ApiGetOverlayOwner(void* host);
        static BOOL __cdecl ApiGetVideoRect(void* host, RECT* rc);
        static int __cdecl ApiGetPlayState(void* host);
        static int64_t __cdecl ApiGetPositionMs(void* host);
        static int64_t __cdecl ApiGetDurationMs(void* host);
        static int __cdecl ApiGetMediaTitle(void* host, wchar_t* buf, int cch);
        static int __cdecl ApiGetMediaPath(void* host, wchar_t* buf, int cch);
        static BOOL __cdecl ApiCaptureFrame(void* host, MpcFrameSink sink, void* context, wchar_t* error, int errorCch);
        static int __cdecl ApiGetSetting(void* host, const wchar_t* key, const wchar_t* def, wchar_t* buf, int cch);
        static BOOL __cdecl ApiSetSetting(void* host, const wchar_t* key, const wchar_t* value);
        static int __cdecl ApiGetProgramDir(void* host, wchar_t* buf, int cch);
        static int __cdecl ApiGetPluginDir(void* host, wchar_t* buf, int cch);
        static int __cdecl ApiGetDataDir(void* host, wchar_t* buf, int cch);
        static UINT __cdecl ApiGetDpi(void* host);
        static void __cdecl ApiLog(void* host, const wchar_t* message);
        static DWORD __cdecl ApiGetPlayerProcessId(void* host);
        static BOOL __cdecl ApiSendCommand(void* host, int command);

        HostServices& m_services;
        std::wstring m_pluginsDir;
        std::wstring m_shadowDir;
        unsigned m_shadowCounter = 0;
        std::vector<std::wstring> m_pendingDeletes;
        std::map<std::wstring, std::unique_ptr<Plugin>> m_plugins; // by lower case source path
    };
}
