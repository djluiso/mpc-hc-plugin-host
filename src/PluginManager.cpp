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

#include "PluginManager.h"
#include "Common.h"
#include <algorithm>
#include <cstddef>

namespace Host
{
    struct PluginManager::Context {
        MpcHostApi api{};
        PluginManager* manager = nullptr;
        Plugin* plugin = nullptr;
    };

    struct PluginManager::Plugin {
        std::wstring sourcePath, key, dir;
        ULONGLONG size = 0, time = 0;               // version loaded or attempted
        ULONGLONG seenSize = 0, seenTime = 0;       // version seen by the previous scan
        bool attempted = false;
        HMODULE module = nullptr;
        DLL_DIRECTORY_COOKIE cookie = nullptr;
        std::wstring shadowPath;
        const MpcPlugin* plugin = nullptr;
        std::unique_ptr<Context> context;
        std::wstring id, name, version, description, error;
        bool crashed = false;
    };

    namespace
    {
        constexpr wchar_t kPluginsSection[] = L"Plugins";

        // Calls into plugins are guarded: a crashing plugin is disabled instead of taking the host down.
        // (no C++ objects with destructors in these functions, as required by __try)
        bool SafeCreate(MpcPluginCreateFn create, const MpcHostApi* api, const MpcPlugin** out)
        {
            __try {
                *out = create(api);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SafeEvent(const MpcPlugin* p, int event, intptr_t param)
        {
            __try {
                if (p->OnEvent) {
                    p->OnEvent(p->instance, event, param);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SafeDestroy(const MpcPlugin* p)
        {
            __try {
                if (p->Destroy) {
                    p->Destroy(p->instance);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        int SafePreTranslate(const MpcPlugin* p, const MSG* msg)
        {
            __try {
                return p->PreTranslateMessage && p->PreTranslateMessage(p->instance, msg) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        ULONGLONG ToULL(FILETIME ft)
        {
            return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        }

        bool IsProcessAlive(DWORD pid)
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) {
                return GetLastError() == ERROR_ACCESS_DENIED;
            }
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            return alive;
        }

        void DeleteFolder(const std::wstring& dir)
        {
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"*").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        DeleteFileW((dir + fd.cFileName).c_str());
                    }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            RemoveDirectoryW(dir.c_str());
        }
    }

    PluginManager::PluginManager(HostServices& services, const std::wstring& pluginsDir, const std::wstring& shadowRoot)
        : m_services(services)
        , m_pluginsDir(WithSlash(pluginsDir))
    {
        CreateDirectoryW(m_pluginsDir.c_str(), nullptr);
        const std::wstring root = WithSlash(shadowRoot);
        CreateDirectoryW(DirOf(root.substr(0, root.size() - 1)).c_str(), nullptr);
        CreateDirectoryW(root.c_str(), nullptr);
        CleanupShadowRoot(root);
        m_shadowDir = root + std::to_wstring(GetCurrentProcessId()) + L"\\";
        CreateDirectoryW(m_shadowDir.c_str(), nullptr);
    }

    PluginManager::~PluginManager()
    {
        UnloadAll();
        DeleteFolder(m_shadowDir);
    }

    void PluginManager::CleanupShadowRoot(const std::wstring& root)
    {
        // private copies left behind by previous instances (only deletable once that process is gone)
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((root + L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                const DWORD pid = wcstoul(fd.cFileName, nullptr, 10);
                if (pid && pid != GetCurrentProcessId() && !IsProcessAlive(pid)) {
                    DeleteFolder(root + fd.cFileName + L"\\");
                }
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    std::vector<std::tuple<std::wstring, ULONGLONG, ULONGLONG>> PluginManager::FindCandidates() const
    {
        std::vector<std::tuple<std::wstring, ULONGLONG, ULONGLONG>> result;
        auto add = [&](const std::wstring & path) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                result.emplace_back(path, (ULONGLONG(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow, ToULL(fad.ftLastWriteTime));
            }
        };
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((m_pluginsDir + L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) {
            return result;
        }
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Plugins\<Name>\<Name>.dll
                add(m_pluginsDir + name + L"\\" + name + L".dll");
            } else if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".dll") {
                add(m_pluginsDir + name);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        return result;
    }

    void PluginManager::Scan()
    {
        m_pendingDeletes.erase(std::remove_if(m_pendingDeletes.begin(), m_pendingDeletes.end(), [](const std::wstring& path) {
            return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        }), m_pendingDeletes.end());

        const auto candidates = FindCandidates();

        // removed
        for (auto it = m_plugins.begin(); it != m_plugins.end();) {
            bool present = false;
            for (const auto& c : candidates) {
                present = present || Lower(std::get<0>(c)) == it->first;
            }
            if (!present) {
                if (it->second->module) {
                    Log(L"Plugin removed: %s", it->second->sourcePath.c_str());
                }
                Unload(*it->second);
                it = m_plugins.erase(it);
            } else {
                ++it;
            }
        }

        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        const ULONGLONG now = ToULL(nowFt);
        for (const auto& c : candidates) {
            const std::wstring& path = std::get<0>(c);
            const ULONGLONG size = std::get<1>(c), time = std::get<2>(c);
            auto& slot = m_plugins[Lower(path)];
            if (!slot) {
                slot = std::make_unique<Plugin>();
                slot->sourcePath = path;
                slot->key = StemOf(path);
                slot->dir = DirOf(path);
            }
            Plugin& p = *slot;
            // wait until the file stopped changing (being copied) before loading it
            const bool stable = (size == p.seenSize && time == p.seenTime) || now - time > 3ull * 10000000;
            p.seenSize = size;
            p.seenTime = time;
            if (!stable || (p.attempted && size == p.size && time == p.time)) {
                continue;
            }
            if (p.module) {
                Log(L"Plugin changed, reloading: %s", path.c_str());
            }
            Unload(p);
            p.size = size;
            p.time = time;
            p.attempted = true;
            p.error.clear();
            if (Settings::Get().GetInt(kPluginsSection, p.key.c_str(), 1)) {
                Load(p);
            }
        }
    }

    bool PluginManager::Load(Plugin& p)
    {
        p.crashed = false;
        p.shadowPath = m_shadowDir + std::to_wstring(++m_shadowCounter) + L"_" + FileNameOf(p.sourcePath);
        if (!CopyFileW(p.sourcePath.c_str(), p.shadowPath.c_str(), FALSE)) {
            p.error = L"Unable to copy the plugin (error " + std::to_wstring(GetLastError()) + L")";
            Log(L"%s: %s", p.sourcePath.c_str(), p.error.c_str());
            p.shadowPath.clear();
            return false;
        }
        p.cookie = AddDllDirectory(p.dir.c_str());
        p.module = LoadLibraryExW(p.shadowPath.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!p.module) {
            const DWORD err = GetLastError();
            p.error = err == ERROR_BAD_EXE_FORMAT ? L"Wrong bitness (the plugin must match MpcPluginHost.exe)"
                      : L"Unable to load the plugin (error " + std::to_wstring(err) + L")";
            Log(L"%s: %s", p.sourcePath.c_str(), p.error.c_str());
            Unload(p);
            return false;
        }
        auto create = reinterpret_cast<MpcPluginCreateFn>(GetProcAddress(p.module, MPC_PLUGIN_CREATE_NAME));
        if (!create) {
            p.error = L"Not a plugin (MpcPluginCreate is not exported)";
            Log(L"%s: %s", p.sourcePath.c_str(), p.error.c_str());
            Unload(p);
            return false;
        }

        p.context = std::make_unique<Context>();
        p.context->manager = this;
        p.context->plugin = &p;
        MpcHostApi& api = p.context->api;
        api.cbSize = sizeof(MpcHostApi);
        api.apiVersion = MPC_PLUGIN_API_VERSION;
        api.host = p.context.get();
        api.GetMainWindow = ApiGetMainWindow;
        api.GetOverlayOwner = ApiGetOverlayOwner;
        api.GetVideoRect = ApiGetVideoRect;
        api.GetPlayState = ApiGetPlayState;
        api.GetPositionMs = ApiGetPositionMs;
        api.GetDurationMs = ApiGetDurationMs;
        api.GetMediaTitle = ApiGetMediaTitle;
        api.GetMediaPath = ApiGetMediaPath;
        api.CaptureFrame = ApiCaptureFrame;
        api.GetSetting = ApiGetSetting;
        api.SetSetting = ApiSetSetting;
        api.GetProgramDir = ApiGetProgramDir;
        api.GetPluginDir = ApiGetPluginDir;
        api.GetDataDir = ApiGetDataDir;
        api.GetDpi = ApiGetDpi;
        api.Log = ApiLog;
        api.GetPlayerProcessId = ApiGetPlayerProcessId;
        api.SendCommand = ApiSendCommand;

        const MpcPlugin* plugin = nullptr;
        if (!SafeCreate(create, &api, &plugin)) {
            Fail(p, L"MpcPluginCreate");
            return false;
        }
        if (!plugin || plugin->cbSize < offsetof(MpcPlugin, PreTranslateMessage) || plugin->apiVersion == 0
                || plugin->apiVersion > MPC_PLUGIN_API_VERSION) {
            p.error = plugin ? L"Incompatible plugin API version" : L"The plugin refused to load";
            Log(L"%s: %s", p.sourcePath.c_str(), p.error.c_str());
            if (plugin) {
                SafeDestroy(plugin);
            }
            Unload(p);
            return false;
        }
        p.plugin = plugin;
        p.id = plugin->id && *plugin->id ? plugin->id : p.key;
        p.name = plugin->name && *plugin->name ? plugin->name : p.id;
        p.version = plugin->version ? plugin->version : L"";
        p.description = plugin->description ? plugin->description : L"";
        Log(L"Loaded plugin %s %s (%s)", p.name.c_str(), p.version.c_str(), p.sourcePath.c_str());

        for (const auto& e : m_services.GetInitialEvents()) {
            if (!p.plugin || p.crashed) {
                break;
            }
            if (!SafeEvent(p.plugin, e.first, e.second)) {
                Fail(p, L"OnEvent");
            }
        }
        return true;
    }

    void PluginManager::Unload(Plugin& p)
    {
        if (p.plugin && !p.crashed) {
            if (!SafeDestroy(p.plugin)) {
                Fail(p, L"Destroy");
            } else {
                Log(L"Unloaded plugin %s", p.name.c_str());
            }
        }
        p.plugin = nullptr;
        if (p.module && !p.crashed) {
            // threads still running in the plugin keep it loaded until they end
            FreeLibrary(p.module);
        }
        p.module = nullptr;
        if (p.cookie) {
            RemoveDllDirectory(p.cookie);
            p.cookie = nullptr;
        }
        if (!p.shadowPath.empty()) {
            if (!DeleteFileW(p.shadowPath.c_str())) {
                m_pendingDeletes.push_back(p.shadowPath); // still used by a plugin thread, retried by Scan
            }
            p.shadowPath.clear();
        }
        p.context.reset();
    }

    void PluginManager::Fail(Plugin& p, const wchar_t* where)
    {
        p.crashed = true;
        p.error = std::wstring(L"Crashed in ") + where + L", disabled until it is updated";
        Log(L"%s: %s", p.sourcePath.c_str(), p.error.c_str());
        p.plugin = nullptr;
        // the module and the context are deliberately kept: windows of the plugin may still reference them
        p.module = nullptr;
        p.cookie = nullptr;
        p.shadowPath.clear();
        p.context.release();
    }

    void PluginManager::Broadcast(int event, intptr_t param)
    {
        for (auto& kv : m_plugins) {
            Plugin& p = *kv.second;
            if (p.plugin && !p.crashed && !SafeEvent(p.plugin, event, param)) {
                Fail(p, L"OnEvent");
            }
        }
    }

    bool PluginManager::PreTranslateMessage(const MSG* msg)
    {
        for (auto& kv : m_plugins) {
            Plugin& p = *kv.second;
            if (!p.plugin || p.crashed) {
                continue;
            }
            const int r = SafePreTranslate(p.plugin, msg);
            if (r < 0) {
                Fail(p, L"PreTranslateMessage");
            } else if (r > 0) {
                return true;
            }
        }
        return false;
    }

    void PluginManager::OnCrashInModule(HMODULE module)
    {
        for (auto& kv : m_plugins) {
            Plugin& p = *kv.second;
            if (module && p.module == module) {
                Fail(p, L"a window procedure");
            }
        }
    }

    void PluginManager::UnloadAll()
    {
        for (auto& kv : m_plugins) {
            Unload(*kv.second);
        }
        m_plugins.clear();
    }

    std::vector<PluginManager::Info> PluginManager::List() const
    {
        std::vector<Info> list;
        for (const auto& kv : m_plugins) {
            const Plugin& p = *kv.second;
            Info i;
            i.key = p.key;
            i.name = p.name.empty() ? p.key : p.name;
            i.version = p.version;
            i.description = p.description;
            i.path = p.sourcePath;
            i.error = p.error;
            i.loaded = p.plugin != nullptr && !p.crashed;
            i.enabled = Settings::Get().GetInt(kPluginsSection, p.key.c_str(), 1) != 0;
            list.push_back(std::move(i));
        }
        return list;
    }

    void PluginManager::SetEnabled(const std::wstring& key, bool enabled)
    {
        Settings::Get().SetInt(kPluginsSection, key.c_str(), enabled ? 1 : 0);
        for (auto& kv : m_plugins) {
            Plugin& p = *kv.second;
            if (Lower(p.key) != Lower(key)) {
                continue;
            }
            if (!enabled) {
                Unload(p);
            } else if (!p.plugin) {
                p.attempted = false; // loaded by the scan below
                p.seenSize = p.size;
                p.seenTime = p.time;
            }
        }
        if (enabled) {
            Scan();
        }
    }

    // -------------------------------------------------------------------------------------------------------------
    // MpcHostApi

#define CTX(h) (static_cast<Context*>(h))
#define SERVICES(h) (CTX(h)->manager->m_services)

    HWND __cdecl PluginManager::ApiGetMainWindow(void* host)
    {
        return SERVICES(host).GetMainWindow();
    }

    HWND __cdecl PluginManager::ApiGetOverlayOwner(void* host)
    {
        return SERVICES(host).GetOverlayOwner();
    }

    BOOL __cdecl PluginManager::ApiGetVideoRect(void* host, RECT* rc)
    {
        RECT r{};
        const bool ok = SERVICES(host).GetVideoRect(r);
        if (rc) {
            *rc = r;
        }
        return ok;
    }

    int __cdecl PluginManager::ApiGetPlayState(void* host)
    {
        return SERVICES(host).GetPlayState();
    }

    int64_t __cdecl PluginManager::ApiGetPositionMs(void* host)
    {
        return SERVICES(host).GetPositionMs();
    }

    int64_t __cdecl PluginManager::ApiGetDurationMs(void* host)
    {
        return SERVICES(host).GetDurationMs();
    }

    int __cdecl PluginManager::ApiGetMediaTitle(void* host, wchar_t* buf, int cch)
    {
        return CopyOut(SERVICES(host).GetMediaTitle(), buf, cch);
    }

    int __cdecl PluginManager::ApiGetMediaPath(void* host, wchar_t* buf, int cch)
    {
        return CopyOut(SERVICES(host).GetMediaPath(), buf, cch);
    }

    BOOL __cdecl PluginManager::ApiCaptureFrame(void* host, MpcFrameSink sink, void* context, wchar_t* error, int errorCch)
    {
        std::vector<BYTE> dib;
        std::wstring err;
        if (!sink || !SERVICES(host).CaptureFrame(dib, err) || dib.empty()) {
            CopyOut(err.empty() ? std::wstring(L"Unable to capture the current frame") : err, error, errorCch);
            return FALSE;
        }
        sink(context, reinterpret_cast<const BITMAPINFOHEADER*>(dib.data()), dib.size());
        return TRUE;
    }

    int __cdecl PluginManager::ApiGetSetting(void* host, const wchar_t* key, const wchar_t* def, wchar_t* buf, int cch)
    {
        if (!key) {
            return CopyOut(def ? def : L"", buf, cch);
        }
        const std::wstring section = L"Plugin." + CTX(host)->plugin->key;
        return CopyOut(Settings::Get().GetString(section.c_str(), key, def ? def : L""), buf, cch);
    }

    BOOL __cdecl PluginManager::ApiSetSetting(void* host, const wchar_t* key, const wchar_t* value)
    {
        if (!key) {
            return FALSE;
        }
        const std::wstring section = L"Plugin." + CTX(host)->plugin->key;
        return Settings::Get().SetString(section.c_str(), key, value);
    }

    int __cdecl PluginManager::ApiGetProgramDir(void* host, wchar_t* buf, int cch)
    {
        return CopyOut(SERVICES(host).GetProgramDir(), buf, cch);
    }

    int __cdecl PluginManager::ApiGetPluginDir(void* host, wchar_t* buf, int cch)
    {
        return CopyOut(CTX(host)->plugin->dir, buf, cch);
    }

    int __cdecl PluginManager::ApiGetDataDir(void* host, wchar_t* buf, int cch)
    {
        return CopyOut(SERVICES(host).GetDataDir(), buf, cch);
    }

    UINT __cdecl PluginManager::ApiGetDpi(void* host)
    {
        return SERVICES(host).GetDpi();
    }

    void __cdecl PluginManager::ApiLog(void* host, const wchar_t* message)
    {
        Log(L"[%s] %s", CTX(host)->plugin->key.c_str(), message ? message : L"");
    }

    DWORD __cdecl PluginManager::ApiGetPlayerProcessId(void* host)
    {
        return SERVICES(host).GetPlayerProcessId();
    }

    BOOL __cdecl PluginManager::ApiSendCommand(void* host, int command)
    {
        return SERVICES(host).SendCommand(command);
    }

#undef SERVICES
#undef CTX
}
