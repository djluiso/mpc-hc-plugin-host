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

#include "Common.h"
#include <algorithm>
#include <cstdarg>
#include <cwctype>
#include <mutex>
#include <vector>

namespace Host
{
    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) {
            return std::wstring();
        }
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    std::string WideToUtf8(const std::wstring& s)
    {
        if (s.empty()) {
            return std::string();
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string a(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &a[0], n, nullptr, nullptr);
        return a;
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        return s;
    }

    std::wstring WithSlash(std::wstring dir)
    {
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
            dir += L'\\';
        }
        return dir;
    }

    std::wstring DirOf(const std::wstring& path)
    {
        const size_t p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? std::wstring() : path.substr(0, p + 1);
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        const size_t p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? path : path.substr(p + 1);
    }

    std::wstring StemOf(const std::wstring& path)
    {
        std::wstring name = FileNameOf(path);
        const size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos ? name : name.substr(0, dot);
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool DirExists(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring ModulePath(HMODULE module)
    {
        std::vector<wchar_t> buf(MAX_PATH);
        for (;;) {
            const DWORD n = GetModuleFileNameW(module, buf.data(), (DWORD)buf.size());
            if (n < buf.size()) {
                return std::wstring(buf.data(), n);
            }
            buf.resize(buf.size() * 2);
        }
    }

    std::wstring ProcessImagePath(DWORD pid)
    {
        std::wstring result;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t buf[32768];
            DWORD n = _countof(buf);
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
                result.assign(buf, n);
            }
            CloseHandle(h);
        }
        return result;
    }

    int CopyOut(const std::wstring& s, wchar_t* buf, int cch)
    {
        if (buf && cch > 0) {
            const size_t n = std::min(s.size(), size_t(cch - 1));
            std::copy_n(s.data(), n, buf);
            buf[n] = L'\0';
        }
        return (int)s.size();
    }

    namespace
    {
        std::mutex g_logLock;
        std::wstring g_logPath;
    }

    void InitLog(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        g_logPath = path;
        // keep the log small: start over when it grew past 1 MB
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 1024 * 1024)) {
            DeleteFileW(path.c_str());
        }
    }

    void Log(const wchar_t* format, ...)
    {
        wchar_t text[2048];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(text, _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t line[2200];
        swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                   st.wSecond, st.wMilliseconds, text);
        OutputDebugStringW(line);

        std::lock_guard<std::mutex> lock(g_logLock);
        if (g_logPath.empty()) {
            return;
        }
        HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const std::string utf8 = WideToUtf8(line);
            DWORD written;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            CloseHandle(f);
        }
    }

    // ---------------------------------------------------------------------------------------------------------

    Settings& Settings::Get()
    {
        static Settings s;
        return s;
    }

    void Settings::Init(const std::wstring& iniPath)
    {
        m_path = iniPath;
        CreateDirectoryW(DirOf(iniPath).c_str(), nullptr);
    }

    std::wstring Settings::GetString(const wchar_t* section, const wchar_t* key, const wchar_t* def) const
    {
        std::vector<wchar_t> buf(4096);
        for (;;) {
            const DWORD n = GetPrivateProfileStringW(section, key, def, buf.data(), (DWORD)buf.size(), m_path.c_str());
            if (n < buf.size() - 2) {
                return std::wstring(buf.data(), n);
            }
            buf.resize(buf.size() * 4);
        }
    }

    int Settings::GetInt(const wchar_t* section, const wchar_t* key, int def) const
    {
        const std::wstring s = GetString(section, key, L"");
        return s.empty() ? def : _wtoi(s.c_str());
    }

    bool Settings::SetString(const wchar_t* section, const wchar_t* key, const wchar_t* value)
    {
        return !!WritePrivateProfileStringW(section, key, value, m_path.c_str());
    }

    bool Settings::SetInt(const wchar_t* section, const wchar_t* key, int value)
    {
        return SetString(section, key, std::to_wstring(value).c_str());
    }

    // ---------------------------------------------------------------------------------------------------------

    namespace
    {
        constexpr wchar_t kPlayerSettingsKey[] = L"Software\\MPC-HC\\MPC-HC\\Settings";
    }

    PlayerConfig::PlayerConfig(const std::wstring& exePath)
    {
        if (!exePath.empty()) {
            const std::wstring ini = DirOf(exePath) + StemOf(exePath) + L".ini";
            if (FileExists(ini)) {
                m_ini = ini;
            }
        }
    }

    int PlayerConfig::GetInt(const wchar_t* name, int def) const
    {
        if (!m_ini.empty()) {
            return (int)GetPrivateProfileIntW(L"Settings", name, def, m_ini.c_str());
        }
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kPlayerSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
            return (int)value;
        }
        return def;
    }

    bool PlayerConfig::SetInt(const wchar_t* name, int value)
    {
        if (!m_ini.empty()) {
            return !!WritePrivateProfileStringW(L"Settings", name, std::to_wstring(value).c_str(), m_ini.c_str());
        }
        const DWORD v = (DWORD)value;
        return RegSetKeyValueW(HKEY_CURRENT_USER, kPlayerSettingsKey, name, REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
    }
}
