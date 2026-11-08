# QSO One under Wine: what was wrong and how it was fixed

Tested with QSO One 4.1.4 (build 664), Wine 11.19, and PipeWire (pulse) on CachyOS.

There were two separate problems. Both are fixed by replacing or patching DLLs inside the
QSO One folder only. Nothing in Wine itself, the Wine registry, or winetricks was changed.

## Quick start (after every QSO One update)

1. Close QSO One.
2. Run the script:
   ```bash
   ~/qso-one-wine-fix/qso-one-wine-fix.sh
   ```
3. Start QSO One and pick your microphone in the input device list.

Other commands:

| Command | What it does |
|---|---|
| `qso-one-wine-fix.sh --status` | Shows what is and isn't fixed. Changes nothing. |
| `qso-one-wine-fix.sh --revert` | Puts the original DLLs back and removes `MQ.dll`. |
| `qso-one-wine-fix.sh --rebuild` | Recompiles the two DLLs from `src/` first. Needs `mingw-w64-gcc`. |
| `--prefix DIR` / `--app-dir DIR` | Use a different Wine prefix or install folder. |

The script is safe to run repeatedly, and refuses to run while QSO One is open.

---

## Problem 1: the app crashed on startup

**Symptom:** Wine wrote `C:\backtrace.txt`. All of the app's frames in it were inside
`flutter_local_notifications_windows.dll`.

**Cause:** This DLL is the Flutter plugin for Windows desktop notifications. In older QSO One
versions this was the "toast" DLL. On startup its `init` function asks Windows for the toast
notification manager (`ToastNotificationManager`), which Wine doesn't provide. Windows returns
"class not registered" (`0x80040154`), the plugin doesn't catch the error, and the whole app
closes.

Deleting the DLL doesn't work, because the app loads it by name and would fail to start without it.

**Fix:** Replace it with a do-nothing DLL that has the same 14 functions (`createPlugin`, `init`,
`showNotification`, …). Each function just reports success. The source is in
`src/notifications_stub.c`.

**Side effect:** You don't get Windows desktop notifications from QSO One. Everything else works.

## Problem 2: every microphone showed as "unavailable"

**Symptom:** The app lists all your input devices, and speakers work, but no microphone ever
opens. Switching Wine's audio driver (pulse, ALSA, OSS) or using winetricks made no difference.

**Cause:** QSO One records through `record_windows_plugin.dll`, which opens microphones with
Windows **Media Foundation** (`MFCreateDeviceSource`). Wine's Media Foundation can *list*
capture devices but can't *open* them. It returns "not implemented" (`E_NOTIMPL`, `0x80004001`)
for every audio capture device. This happens above the audio driver, which is why changing the
driver didn't help. Playback uses a different API, so speakers were fine.

**Fix:** A small helper DLL, `MQ.dll`, plus a one-word patch to the recording plugin:

1. **`MQ.dll`** goes in the QSO One folder (source: `src/mq.c` plus `src/mq.def`).
   * It passes every Media Foundation function straight through to Wine's own `mf.dll`.
   * The one exception is `MFCreateDeviceSource`. For microphones, it asks Wine first. Only if
     Wine fails does it provide its own capture source, built on **WASAPI**, the lower-level
     Windows audio interface that Wine does support. So if a future Wine adds microphone support
     to Media Foundation, Wine's version takes over automatically.
   * It supplies 16-bit or float audio at common rates (8 kHz to 96 kHz, mono or stereo). Format
     conversion is left to PulseAudio/PipeWire.
2. **`record_windows_plugin.dll`** is patched so it loads `MQ.dll` instead of `MF.dll`. This
   changes exactly one 6-byte name in the DLL's import table (`MF.dll` → `MQ.dll`). Nothing else
   in the app loads `MQ.dll`, so the change affects only the recording plugin.

### Files changed in `C:\Program Files\QSO One\`

| File | Change | Backup |
|---|---|---|
| `flutter_local_notifications_windows.dll` | Replaced with the no-op stub | `…windows.dll.orig` |
| `record_windows_plugin.dll` | Import name `MF.dll` → `MQ.dll` | `record_windows_plugin.dll.orig` |
| `MQ.dll` | New file | (delete to undo) |

## Choosing the right microphone in QSO One

QSO One saves your microphone **by its exact name** and won't substitute another one. If the
name changes, the app says the selected microphone "isn't available", even though capture works.

That happened here. The headset's PipeWire profile changed to **Pro Audio**, so its microphone
is now called `Microphone (Logitech G PRO X Gaming Headset Pro)`. The app still had the old name,
`Microphone (Logitech G PRO X Gaming Headset Mono)`, which no longer exists. The two names
differ only in the last word.

To fix it, pick the exact entry you want in QSO One's input device list. Alternatively, switch
the headset back to the profile that produced the old name (for example in `pavucontrol` → Configuration).

Notes on the entries in the list:
* **`Microphone (PulseAudio Input)`** follows whichever input PipeWire has as its *default*.
  This is the most robust choice: it survives profile changes and unplugging.
* **`Microphone (Monitor of …)`** entries record what is *playing* on that output, not a
  microphone.
* The headset entry works through the fix. During testing, `{4CCBEC43…}` delivered live audio
  at 8, 16 and 48 kHz.

## Troubleshooting

* **See what the capture helper is doing.** Start the app from a terminal with logging on:
  ```bash
  cd ~/.wine/drive_c/Program\ Files/QSO\ One
  QSO_MQ_DEBUG=1 wine qso_one.exe
  ```
  Look for `qso-mq:` lines, for example `capture opened: deliver 8000 Hz/1 ch/16 bit…`.
* **QSO One's own log** is at
  `~/.wine/drive_c/users/$USER/AppData/Roaming/com.qsoone/qso_one/debug.log`. Search it for
  `[Audio] input:` and `startMicSession`.
* **Skip Wine's Media Foundation entirely** with `QSO_MQ_FORCE=1`. This is only useful if a
  future Wine "implements" it badly.
* **The script says a DLL "has N MF.dll import names" or a stub is "missing exports"**: a new
  QSO One version changed that plugin. Close QSO One, run `--status` to see what's affected,
  and look at `src/` before forcing anything.
* **Only one QSO One instance runs at a time.** Starting it again just brings the existing window
  to the front. Close it fully before applying fixes.

## Folder layout

```
qso-one-wine-fix/
├── qso-one-wine-fix.sh        apply / --status / --revert / --rebuild
├── README.md                  this file
├── bin/                       prebuilt DLLs the script installs
│   ├── MQ.dll
│   └── flutter_local_notifications_windows.dll
└── src/
    ├── mq.c                   capture helper (Media Foundation → WASAPI)
    ├── mq.def                 fallback export list (normally regenerated from Wine's mf.dll)
    └── notifications_stub.c   no-op notifications plugin
```

Building by hand (the script does this with `--rebuild`):

```bash
x86_64-w64-mingw32-gcc -shared -O2 -s -static-libgcc -o bin/MQ.dll src/mq.c src/mq.def \
    -lmfplat -lmfuuid -lole32 -luuid
x86_64-w64-mingw32-gcc -shared -O2 -s -static-libgcc \
    -o bin/flutter_local_notifications_windows.dll src/notifications_stub.c
```
