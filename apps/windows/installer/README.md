# Hop Windows x64 release candidate installer

The NSIS recipe installs Hop 2.0.0-rc.1 for the current user on x64 Windows 10
(build 15063 or later) and Windows 11. It places the tray and uninstaller in
`%LOCALAPPDATA%\Programs\Hop`, creates a Start menu shortcut and registers
the application for uninstall. Upgrades and uninstall ask the user to exit
Hop if the application is running. The uninstaller preserves trusted peers,
settings and other files the user put in the directory.

Build a TLS-enabled x64 Release tray and run its CTest suite before packaging.
The installer includes the project license, the pinned mbedTLS license
and the repository third-party notices.

```powershell
makensis /WX `
  /DHOP_EXECUTABLE=C:\absolute\path\hop_windows_tray.exe `
  /DHOP_LICENSE=C:\absolute\path\mouse_transfer\LICENSE `
  /DHOP_TLS_LICENSE=C:\absolute\path\mouse_transfer\third_party\mbedtls-3.6.7-LICENSE.txt `
  /DHOP_NOTICES=C:\absolute\path\mouse_transfer\THIRD_PARTY_NOTICES.md `
  /DHOP_INSTALLER=C:\absolute\path\Hop-Setup-2.0.0-rc.1-windows-x64.exe `
  apps\windows\installer\Hop.nsi
```

The 2026-11-10 candidate was compiled with NSIS 3.13 with warnings treated as
errors. A separate fixture build used the same installer script and final x64
tray payload with isolated install, shortcut, registry and mutex names. On
native Windows it passed silent clean installation, in-place upgrade and
uninstall. The fixture verified executable hashes, version registration,
shortcut targets, and installation and removal of packaged notices. It also
verified protection against a busy lifecycle mutex,
a locked executable and a relocated uninstaller, as well as preservation of
unrelated files and settings. This exercised the installer without opening the
Hop GUI or running its hooks, clipboard or discovery.

The production-named candidate is unsigned and may trigger Windows security
prompts. Signing, SmartScreen reputation, interactive setup and operation of
the installed application on real desktops are pending.
