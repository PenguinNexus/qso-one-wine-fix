#!/usr/bin/env bash
# qso-one-wine-fix.sh - make QSO One (Windows, Flutter) start and use the microphone under Wine.
#
# Fix 1 (crash at startup): replace flutter_local_notifications_windows.dll with a no-op stub.
# Fix 2 (every microphone "unavailable"): install MQ.dll, a Media Foundation capture shim, and
#        point record_windows_plugin.dll at it by renaming its "MF.dll" import to "MQ.dll".
#
# Originals are kept next to the patched files as *.orig. Run again after every QSO One update.
# See README.md for the details.

set -euo pipefail

FIX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$FIX_DIR/src"
BIN_DIR="$FIX_DIR/bin"

WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
APP_DIR=""
MODE=apply
REBUILD=0

NOTIF_DLL=flutter_local_notifications_windows.dll
RECORD_DLL=record_windows_plugin.dll
NOTIF_MARKER="QSO-WINE-FIX notifications stub"
MQ_MARKER="QSO-WINE-FIX MQ capture shim"
MINGW_CC=x86_64-w64-mingw32-gcc

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  (no option)       apply both fixes (safe to re-run; already-fixed files are left alone)
  --status          show what is and isn't fixed, change nothing
  --revert          restore the original DLLs from *.orig and remove MQ.dll
  --rebuild         recompile the DLLs from src/ before applying (needs $MINGW_CC)
  --prefix DIR      Wine prefix (default: \$WINEPREFIX or ~/.wine)
  --app-dir DIR     QSO One install folder (default: <prefix>/drive_c/Program Files/QSO One)
  -h, --help        this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --status)  MODE=status ;;
        --revert)  MODE=revert ;;
        --rebuild) REBUILD=1 ;;
        --prefix)  WINEPREFIX="${2:?--prefix needs a directory}"; shift ;;
        --app-dir) APP_DIR="${2:?--app-dir needs a directory}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

APP_DIR="${APP_DIR:-$WINEPREFIX/drive_c/Program Files/QSO One}"

info() { printf '  %s\n' "$*"; }
ok()   { printf '  \033[32m✔\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mError:\033[0m %s\n' "$*" >&2; exit 1; }

has_marker() { LC_ALL=C grep -qaF "$2" "$1" 2>/dev/null; }

# Count imports named MF.dll (any case), each preceded by a NUL so "XYZMF.dll" doesn't match.
count_mf_imports() {
    python3 -I -c '
import re, sys
data = open(sys.argv[1], "rb").read()
print(len(re.findall(rb"(?<=\x00)[Mm][Ff]\.[Dd][Ll][Ll]\x00", data)))' "$1"
}

patch_mf_import() {
    python3 -I -c '
import re, sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
hits = [m.start() for m in re.finditer(rb"(?<=\x00)[Mm][Ff]\.[Dd][Ll][Ll]\x00", data)]
if len(hits) != 1:
    sys.exit("expected exactly one MF.dll import name, found %d" % len(hits))
data[hits[0]:hits[0] + 7] = b"MQ.dll\x00"
open(path, "wb").write(data)
print("%#x" % hits[0])' "$1"
}

dll_exports() {
    winedump -j export "$1" 2>/dev/null | awk '$1 ~ /^[0-9A-Fa-f]{8}$/ && $2 ~ /^[0-9]+$/ && NF >= 3 {print $3}' | sort -u
}

find_wine_mf() {
    local wine_bin dir
    for dir in /usr/lib/wine/x86_64-windows /usr/lib64/wine/x86_64-windows /usr/local/lib/wine/x86_64-windows \
               /usr/lib/x86_64-linux-gnu/wine/x86_64-windows /opt/wine*/lib/wine/x86_64-windows /opt/wine*/lib64/wine/x86_64-windows; do
        [ -f "$dir/mf.dll" ] && { echo "$dir/mf.dll"; return; }
    done
    if wine_bin="$(command -v wine 2>/dev/null)"; then
        dir="$(dirname "$(readlink -f "$wine_bin")")/../lib/wine/x86_64-windows"
        [ -f "$dir/mf.dll" ] && { echo "$dir/mf.dll"; return; }
    fi
    return 1
}

build_dlls() {
    command -v "$MINGW_CC" >/dev/null || die "$MINGW_CC not found (install mingw-w64-gcc) - needed to build the DLLs."
    mkdir -p "$BIN_DIR"
    local def="$SRC_DIR/mq.def" mf
    # Forward every export of this Wine's mf.dll, so MQ.dll stays a drop-in for MF.dll.
    if command -v winedump >/dev/null && mf="$(find_wine_mf)"; then
        def="$(mktemp --suffix=.def)"
        {
            echo "LIBRARY MQ.dll"
            echo "EXPORTS"
            echo "    MFCreateDeviceSource"
            dll_exports "$mf" | grep -vE '^(MFCreateDeviceSource|Dll.*)$' | sed 's/.*/    &=mf.&/'
        } > "$def"
        info "MQ.dll forwarders generated from $mf"
    else
        warn "Wine's mf.dll or winedump not found; using the bundled export list (src/mq.def)."
    fi
    "$MINGW_CC" -shared -O2 -s -Wall -Wno-unused-parameter -static-libgcc -o "$BIN_DIR/MQ.dll" \
        "$SRC_DIR/mq.c" "$def" -lmfplat -lmfuuid -lole32 -luuid
    [ "$def" = "$SRC_DIR/mq.def" ] || rm -f "$def"
    "$MINGW_CC" -shared -O2 -s -static-libgcc -o "$BIN_DIR/$NOTIF_DLL" "$SRC_DIR/notifications_stub.c"
    ok "Built bin/MQ.dll and bin/$NOTIF_DLL"
}

# Keep a copy of the vendor file. A fresh copy is taken whenever the current file is unpatched,
# so after a QSO One update the .orig always matches the installed version.
backup() {
    cp -f "$1" "$1.orig"
}

# --- status ---------------------------------------------------------------

status() {
    echo "QSO One folder: $APP_DIR"
    local f="$APP_DIR/$NOTIF_DLL"
    if [ ! -f "$f" ]; then info "Notifications DLL: not present (nothing to fix)"
    elif has_marker "$f" "$NOTIF_MARKER"; then ok "Notifications DLL: stub installed"
    else warn "Notifications DLL: original (app will crash on start under Wine)"; fi

    f="$APP_DIR/$RECORD_DLL"
    if [ ! -f "$f" ]; then info "Record plugin: not present"
    elif [ "$(count_mf_imports "$f")" = 0 ] && LC_ALL=C grep -qa 'MQ\.dll' "$f"; then ok "Record plugin: patched to use MQ.dll"
    else warn "Record plugin: original (microphones will show as unavailable)"; fi

    if [ -f "$APP_DIR/MQ.dll" ] && has_marker "$APP_DIR/MQ.dll" "$MQ_MARKER"; then ok "MQ.dll: installed"
    else warn "MQ.dll: missing"; fi
}

# --- revert ---------------------------------------------------------------

revert() {
    local name f
    for name in "$NOTIF_DLL" "$RECORD_DLL"; do
        f="$APP_DIR/$name"
        if [ -f "$f.orig" ]; then
            mv -f "$f.orig" "$f"
            ok "Restored $name"
        else
            info "No backup for $name, left as is"
        fi
    done
    if [ -f "$APP_DIR/MQ.dll" ]; then rm -f "$APP_DIR/MQ.dll"; ok "Removed MQ.dll"; fi
}

# --- apply ----------------------------------------------------------------

fix_notifications() {
    local f="$APP_DIR/$NOTIF_DLL" missing
    echo "Fix 1: startup crash (notifications plugin)"
    if [ ! -f "$f" ]; then
        info "$NOTIF_DLL not present in this version, skipping."
        return
    fi
    if has_marker "$f" "$NOTIF_MARKER"; then
        ok "Stub already installed."
        return
    fi
    if command -v winedump >/dev/null; then
        missing="$(comm -23 <(dll_exports "$f") <(dll_exports "$BIN_DIR/$NOTIF_DLL"))"
        if [ -n "$missing" ]; then
            warn "This version's $NOTIF_DLL exports functions the stub doesn't have:"
            warn "  $(echo $missing)"
            warn "Add them to src/notifications_stub.c and re-run with --rebuild if the app misbehaves."
        fi
    fi
    backup "$f"
    cp -f "$BIN_DIR/$NOTIF_DLL" "$f"
    ok "Installed no-op $NOTIF_DLL (original saved as $NOTIF_DLL.orig)."
}

fix_microphone() {
    local f="$APP_DIR/$RECORD_DLL" n other
    echo "Fix 2: microphone capture (record plugin -> MQ.dll)"
    if [ ! -f "$f" ]; then
        warn "$RECORD_DLL not found; this QSO One version records audio differently. Skipping."
        return
    fi
    cp -f "$BIN_DIR/MQ.dll" "$APP_DIR/MQ.dll"
    ok "Installed MQ.dll."

    n="$(count_mf_imports "$f")"
    if [ "$n" = 0 ] && LC_ALL=C grep -qa 'MQ\.dll' "$f"; then
        ok "$RECORD_DLL already patched."
    elif [ "$n" = 1 ]; then
        backup "$f"
        info "Patched import name at offset $(patch_mf_import "$f")"
        ok "$RECORD_DLL now loads MQ.dll (original saved as $RECORD_DLL.orig)."
    else
        die "$RECORD_DLL has $n MF.dll import names (expected 1); not patching. See README.md."
    fi

    # Anything else in the folder that captures through Media Foundation would need the same patch.
    for other in "$APP_DIR"/*.dll "$APP_DIR"/*.exe; do
        [ -f "$other" ] || continue
        case "$(basename "$other")" in "$RECORD_DLL"|MQ.dll|*.orig) continue ;; esac
        if [ "$(count_mf_imports "$other")" != 0 ]; then
            warn "$(basename "$other") also imports MF.dll; it is not patched by this script."
        fi
    done
}

# --- main -----------------------------------------------------------------

[ -d "$APP_DIR" ] || die "QSO One folder not found: $APP_DIR (use --app-dir or --prefix)"
command -v python3 >/dev/null || die "python3 is required."

case "$MODE" in
    status) status; exit 0 ;;
esac

# Building only touches bin/, so it is fine while the app runs.
if [ "$MODE" = apply ] && { [ "$REBUILD" = 1 ] || [ ! -f "$BIN_DIR/MQ.dll" ] || [ ! -f "$BIN_DIR/$NOTIF_DLL" ]; }; then
    build_dlls
fi

if pgrep -f 'qso_one\.exe' >/dev/null 2>&1; then
    die "QSO One is running. Close it first; Windows DLLs can't be safely replaced while loaded."
fi

case "$MODE" in
    revert) revert; exit 0 ;;
esac

echo "QSO One folder: $APP_DIR"
fix_notifications
fix_microphone
echo
echo "Done. Start QSO One, then pick your microphone again in the input device list if the"
echo "previously saved one no longer exists (check its exact name)."
