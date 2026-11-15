#!/usr/bin/env bash
# authorize.sh: once per machine (and again if the project moves): KWin gives its monitor cast (zkde_screencast_unstable_v1) only to a program whose .desktop file asks for it and whose Exec is that program's own path.
# Writes ~/.local/share/applications/ofgl.desktop for out/ofgl and refreshes KDE's cache of such files. Asks for that one interface and nothing else. authorize.sh off removes the file.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
file=${XDG_DATA_HOME:-$HOME/.local/share}/applications/ofgl.desktop
if [[ ${1:-} == off ]]; then
    rm -f "$file"
else
    mkdir -p "$(dirname "$file")"
    printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=ofgl' 'Comment=Optical flow frame doubling: the right to cast a monitor' "Exec=$root/out/ofgl" 'Icon=video-display' 'NoDisplay=true' 'X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1' >"$file"
    echo "wrote $file for $root/out/ofgl"
fi
if command -v kbuildsycoca6 >/dev/null; then kbuildsycoca6 >/dev/null 2>&1 || true; fi
