# MPC-HC Plugin Host

Plugins for an **unmodified** MPC-HC. The player's source code and executable are not touched: a small companion
program, **MpcPluginHost**, runs next to the player, follows it and loads the plugins. Plugins are DLLs that can be
added, updated or removed while the host and the player are running.

```
sdk\MpcPlugin.h      plugin API (plain C ABI), the only file a plugin needs
src\                 MpcPluginHost.exe
build.bat            builds the host
```

Available plugins:

* [X-Ray](https://github.com/djluiso/mpc-hc-xray) - who is on screen, in the style of Prime Video X-Ray.

## Build

```
build.bat [x64|x86]
```

Needs Visual Studio 2022 (or its Build Tools) with the C++ workload. Output: `bin\x64\MpcPluginHost.exe` and an
empty `bin\x64\Plugins` folder. Copy the `bin\x64` folder anywhere, e.g. next to an installed MPC-HC.
The host and the plugins must have the same bitness, the player may be 32 or 64-bit.

## Installing plugins

Put each plugin in the `Plugins` folder next to `MpcPluginHost.exe`, as `Plugins\<Name>.dll` or in its own
folder `Plugins\<Name>\<Name>.dll` (with its extra DLLs). For example, X-Ray installs itself with
`build.bat x64 <path of the Plugins folder>`.

## Usage

Start `MpcPluginHost.exe`. It shows a tray icon, and then:

* **Start MPC-HC** from its menu (or run `MpcPluginHost.exe <file>` / `MpcPluginHost.exe --launch [player arguments]`)
  starts the player with its web interface enabled for that session (`/webport`). The player is found next to the
  host, in its parent folder, in `Program Files\MPC-HC`, or chosen once with a file dialog (`--player <exe>`).
* An MPC-HC that is already running is picked up automatically when its web interface is enabled
  (*Options > Player > Web Interface > Listen on port*). **Always enable MPC-HC's web interface** in the tray menu
  does that for you (MPC-HC must be closed, it saves its options on exit).
* **Start with Windows** starts the host at logon, so it's always ready when MPC-HC opens.
* The plugins can be enabled or disabled from the tray menu. **Open plugins folder**, **Open log** and **Exit**
  (`MpcPluginHost.exe --exit`) are there too.

### Why the web interface

The host has to know what the player is doing without changing it. MPC-HC's built-in web interface provides the
state, position, duration and file path (`variables.html`) and the current frame (`snapshot.jpg`, needs
*Enable preview*). Window positions are read from the player's windows and commands are sent as `WM_COMMAND`
(the same IDs as `command.html`). When the host enables the web interface it also sets
**Allow access from localhost only**, because MPC-HC listens on all network interfaces by default.

## Hot reload

The host scans the `Plugins` folder every second for `Plugins\<Name>.dll` and `Plugins\<Name>\<Name>.dll`.
Plugins are loaded from a copy in `%TEMP%\MpcPluginHost`, so the original file is never locked:

* drop a DLL to install a plugin,
* overwrite it to update it (it is unloaded and the new version loaded within a few seconds),
* delete it to unload it.

A plugin that crashes is disabled until it changes or the host restarts; the player is never affected
(it runs in another process).

## Files

* Portable: when `MpcPluginHost.ini` exists next to `MpcPluginHost.exe`, settings, log and plugin data stay there.
* Otherwise `%LOCALAPPDATA%\MPC-HC\`: `MpcPluginHost.ini`, `MpcPluginHost.log` and the data folders of the plugins.

`MpcPluginHost.ini`:

```ini
[Host]
PlayerPath=C:\Program Files\MPC-HC\mpc-hc64.exe
WebPort=13579        ; used when the player's web interface is disabled

[Plugins]
XRay=1               ; 0 disables the plugin

[Plugin.XRay]        ; settings of each plugin
TMDbApiKey=...
```

## Writing a plugin

Include `sdk\MpcPlugin.h` and export `MpcPluginCreate`. The host calls it on its UI thread; every
function of `MpcHostApi` and of the returned `MpcPlugin` is called on that thread too.

```cpp
#include <windows.h>
#include "MpcPlugin.h"

static const MpcHostApi* g_api;

static void __cdecl OnEvent(void*, int event, intptr_t param)
{
    if (event == MPC_EVENT_PLAYSTATE_CHANGED && param == MPC_PLAYSTATE_PAUSED) {
        wchar_t path[MAX_PATH];
        g_api->GetMediaPath(g_api->host, path, MAX_PATH);
        g_api->Log(g_api->host, path);
    }
}
static void __cdecl Destroy(void*) {}

static const MpcPlugin g_plugin = { sizeof(MpcPlugin), MPC_PLUGIN_API_VERSION, L"Hello", L"Hello",
                                    L"1.0", L"Logs the paused file", nullptr, Destroy, OnEvent, nullptr };

extern "C" __declspec(dllexport) const MpcPlugin* __cdecl MpcPluginCreate(const MpcHostApi* api)
{
    if (api->apiVersion < MPC_PLUGIN_API_VERSION) {
        return nullptr;
    }
    g_api = api;
    return &g_plugin;
}
```

```
cl /LD /MT /I sdk hello.cpp /Fe:bin\x64\Plugins\Hello.dll user32.lib
```

Rules, so that a plugin can be unloaded at any time (see the comments in `MpcPlugin.h`):

* `Destroy` must close the plugin's windows, unregister its window classes and remove its hooks.
* Threads must be joined in `Destroy`, or keep a reference on the module and end with `FreeLibraryAndExitThread`
  (see `XRayThread.h` in the X-Ray repository).
* Windows over the video are top-level popups owned by `GetOverlayOwner()` and placed with `GetVideoRect()`
  (refresh them on `MPC_EVENT_LAYOUT_CHANGED`). They belong to another process than the player, so they can't
  be its child windows and keyboard input of the player isn't seen by the plugin.
* Link the CRT statically (`/MT`) and put extra DLLs next to the plugin: the plugin's folder is searched for
  its dependencies.

## Limitations

* Requires MPC-HC's web interface (localhost). The state is polled 5 times per second.
* Overlays can't be drawn over exclusive D3D fullscreen.
