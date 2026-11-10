# Z2 application icon

The icon is the developer-supplied Z logo in two colours. Blue means ZeroTier is online and red means it is offline.

- `app-icon.ico` is the blue logo at 16 to 256 px. It is embedded in `Z2.exe` and used by Explorer, shortcuts and the UAC prompt.
- `app-icon.png` is the blue logo at 1024 px. It is the source for the macOS `.icns`.
- `logo-online.png` and `logo-offline.png` are the blue and red logos at 256 px. They are compiled into the QML module and used for the title-bar logo, the window and taskbar icon, and the tray icon. All of these switch colour with the service state.

All files are cut from the square originals with a rounded-rectangle alpha mask (radius 21 %), so the corners are transparent.
