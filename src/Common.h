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

#include <windows.h>
#include <string>

namespace Host
{
    std::wstring Utf8ToWide(const std::string& s);
    std::string WideToUtf8(const std::wstring& s);
    std::wstring Lower(std::wstring s);
    std::wstring WithSlash(std::wstring dir);
    std::wstring DirOf(const std::wstring& path);
    std::wstring FileNameOf(const std::wstring& path);
    std::wstring StemOf(const std::wstring& path);
    bool FileExists(const std::wstring& path);
    bool DirExists(const std::wstring& path);
    std::wstring ModulePath(HMODULE module);
    std::wstring ProcessImagePath(DWORD pid);

    // Copies s (with terminator, truncated) into buf of cch characters, returns the full length of s
    int CopyOut(const std::wstring& s, wchar_t* buf, int cch);

    void Log(const wchar_t* format, ...);
    void InitLog(const std::wstring& path);

    // Settings file of the host (ini)
    class Settings
    {
    public:
        static Settings& Get();
        void Init(const std::wstring& iniPath);
        const std::wstring& Path() const { return m_path; }
        std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* def = L"") const;
        int GetInt(const wchar_t* section, const wchar_t* key, int def) const;
        bool SetString(const wchar_t* section, const wchar_t* key, const wchar_t* value);
        bool SetInt(const wchar_t* section, const wchar_t* key, int value);

    private:
        std::wstring m_path;
    };

    // Settings of the player itself (registry, or <exe name>.ini next to the executable in portable mode)
    class PlayerConfig
    {
    public:
        explicit PlayerConfig(const std::wstring& exePath);
        bool IsPortable() const { return !m_ini.empty(); }
        int GetInt(const wchar_t* name, int def) const;
        bool SetInt(const wchar_t* name, int value);

    private:
        std::wstring m_ini;
    };
}
