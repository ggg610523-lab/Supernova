#!/usr/bin/env bash
# Fetches the two bundled assets the shell uses by default and renders them into
# assets/ so win11wm has no runtime dependency on git, rsvg or a font
# installation:
#
#   assets/fonts/MuternVF.ttf            the UI font (variable, wght axis)
#   assets/icons/<name>.png              Reversal icon theme, rasterised flat
#
# Everything here is idempotent and skips work that is already done. Nothing is
# installed outside this checkout and no font cache is touched.
#
# Usage:
#   scripts/fetch-assets.sh              fetch what is missing
#   scripts/fetch-assets.sh --force      re-download and re-render everything
#   scripts/fetch-assets.sh --icons-only / --fonts-only
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS="${WIN11WM_ASSETS:-$REPO_ROOT/assets}"
ICONS="$ASSETS/icons"
FONTS="$ASSETS/fonts"

REVERSAL_REPO="https://github.com/yeyushengfan258/Reversal-icon-theme.git"
REVERSAL_REF="master"
MUTERNVF_REPO="https://github.com/vivescene/MuternVF.git"

# Shell glyphs the WM asks for by name (see draw.cpp). Window buttons are not
# listed: the shell draws those itself, so they only need to resolve when the
# user's own theme has them.
SHELL_ICONS=(
  edit-find system-search user-desktop start-here
  application-x-executable folder folder-open
  preferences-system audio-volume-muted network-wireless
  computer laptop smartphone camera-photo audio-card
  view-list view-grid go-home user-trash document-open
  text-plain
)

# Names the shell or a desktop entry uses that Reversal does not ship verbatim.
# Rendered under the requested name so lookup stays a plain <name>.png hit.
ALIASES=(
  "text-plain:text-x-generic"
  "laptop:computer"
  "smartphone:phone"
)

FORCE=0
WANT_ICONS=1
WANT_FONTS=1
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    --icons-only) WANT_FONTS=0 ;;
    --fonts-only) WANT_ICONS=0 ;;
    -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

say() { printf '[assets] %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# --------------------------------------------------------------------- renderer
# rsvg-convert renders these SVGs correctly; ImageMagick's delegate and inkscape
# are accepted fallbacks. Without any of them the icons are simply skipped and
# the shell falls back to its letter tiles.
pick_renderer() {
  if have rsvg-convert; then echo rsvg-convert
  elif have inkscape; then echo inkscape
  elif have convert && have magick; then echo convert
  elif have convert; then echo convert
  else echo none
  fi
}

render_one() {
  local src="$1" out="$2" size="$3" tool="$4"
  case "$tool" in
    rsvg-convert)
      rsvg-convert --width "$size" --height "$size" -f png -o "$out" "$src" 2>/dev/null ;;
    inkscape)
      inkscape --export-type=png --export-width="$size" --export-height="$size" \
              --export-filename="$out" "$src" >/dev/null 2>&1 ;;
    convert)
      convert -background none -density 384 -resize "${size}x${size}" "$src" "$out" \
              2>/dev/null ;;
  esac
  [[ -s "$out" ]]
}

# Locates the best source SVG for an icon name.
#
# src/ holds the real files; links/ is a symlink tree that only resolves after
# the theme's own install step, so it is searched with -L and last. Within a
# tree the scalable variant wins, then the largest pixel size, because these are
# scaled up to 128px for a 16-24px target.
find_icon_source() {
  local root="$1" name="$2" base
  for base in "$root/src" "$root/alternative" "$root/links" "$root"; do
    [[ -d "$base" ]] || continue
    local hit
    hit="$(find -L "$base" -type f -name "$name.svg" 2>/dev/null | awk -F/ '
      function score(d,   a) {
        if (d == "scalable") return 1000000
        if (d ~ /^[0-9]+$/) return d + 0                # Reversal: 16, 22, 24, 32
        if (d ~ /^[0-9]+x[0-9]+$/) { split(d, a, "x"); return a[1] + 0 }
        return -1
      }
      BEGIN { top = -1 }
      { best = -1
        for (i = 1; i <= NF; i++) { s = score($i); if (s > best) { best = s; line = $0 } }
        if (best > top) { top = best; out = line } }
      END { if (out != "") print out }')"
    [[ -n "$hit" ]] && { printf '%s\n' "$hit"; return 0; }
  done
  return 1
}

# Second source: the desktop's own icon themes. Adwaita, Papirus and breeze ship
# almost nothing as PNG, so this also covers everything the PNG decoder could
# never have read. Reversal still wins, so the default look is unchanged.
find_system_icon_source() {
  local name="$1" root base
  for root in "${XDG_DATA_HOME:-$HOME/.local/share}/icons" "$HOME/.icons" \
              /usr/local/share/icons /usr/share/icons /usr/share/pixmaps; do
    [[ -d "$root" ]] || continue
    for base in "$root" "$root"/*/; do
      [[ -d "$base" ]] || continue
      local hit
      hit="$(find -L "${base%/}" -type f -name "$name.svg" 2>/dev/null | awk -F/ '
        function score(d,   a) {
          if (d == "scalable") return 1000000
          if (d ~ /^[0-9]+x[0-9]+$/) { split(d, a, "x"); return a[1] + 0 }
          if (d ~ /@[0-9]+x$/) return 96
          return -1
        }
        BEGIN { top = -1 }
        { best = -1
          for (i = 1; i <= NF; i++) { s = score($i); if (s > best) { best = s; line = $0 } }
          if (best > top) { top = best; out = line } }
        END { if (out != "") print out }')"
      [[ -n "$hit" ]] && { printf '%s\n' "$hit"; return 0; }
    done
  done
  return 1
}

# ------------------------------------------------------------------ icon render
# Reversal ships monochrome SVGs with a fixed #363636; the shell is dark, so the
# same recolour the theme's own installer does is applied before rasterising.
LIGHTEN='s/#363636/#dedede/g; s/#3b3b3b/#dedede/g'

fetch_icons() {
  mkdir -p "$ICONS"
  # The clone is ~130MB of upstream SVG, so it lives in the user cache rather
  # than in the checkout; --force refreshes it, normal runs never touch the net.
  local src="${XDG_CACHE_HOME:-$HOME/.cache}/win11wm/reversal"
  if [[ $FORCE -eq 1 && -d "$src/.git" ]]; then
    say "updating Reversal icon theme"
    git -C "$src" pull --ff-only >/dev/null 2>&1 || true
  fi
  if [[ ! -d "$src" ]]; then
    mkdir -p "$(dirname "$src")"
    say "cloning Reversal icon theme into $src"
    if ! git clone --depth 1 --branch "$REVERSAL_REF" "$REVERSAL_REPO" "$src" 2>/dev/null; then
      rm -rf "$src"
      say "warning: could not fetch Reversal; the shell will use letter tiles"
      return 1
    fi
  fi

  local tool; tool="$(pick_renderer)"
  if [[ "$tool" == none ]]; then
    say "warning: no SVG rasteriser (rsvg-convert / inkscape / ImageMagick); skipping icons"
    return 1
  fi

  # Collect the sources: the named shell glyphs first (they may live under
  # src/ or links/ at any size), then every desktop entry's Icon= name.
  local list="$WORK/wanted.txt"
  : > "$list"
  local name
  for name in "${SHELL_ICONS[@]}"; do echo "$name" >> "$list"; done
  if have python3; then
    python3 - "$list" <<'PY'
import glob, os, sys
out = open(sys.argv[1], "a")
seen = set()
for pat in ("/usr/share/applications/*.desktop",
            os.path.expanduser("~/.local/share/applications/*.desktop"),
            "/usr/local/share/applications/*.desktop"):
    for path in glob.glob(pat):
        try:
            with open(path, "r", errors="ignore") as f:
                for line in f:
                    if not line.startswith("Icon="):
                        continue
                    name = line[5:].strip()
                    if not name or name in seen or "/" in name:
                        continue
                    seen.add(name)
                    out.write(name + "\n")
        except OSError:
            pass
out.close()
PY
  fi

  say "rendering $(sort -u "$list" | wc -l) icon name(s) with $tool"
  local wanted=0 rendered=0
  while read -r name; do
    [[ -n "$name" ]] || continue
    wanted=$((wanted + 1))
    local out="$ICONS/$name.png"
    if [[ $FORCE -eq 0 && -s "$out" ]]; then
      rendered=$((rendered + 1))
      continue
    fi
    # Aliases render the closest thing Reversal does have.
    local src_name="$name" pair
    for pair in "${ALIASES[@]}"; do
      if [[ "$pair" == "$name:"* ]]; then src_name="${pair#*:}"; fi
    done
    local found
    found="$(find_icon_source "$src" "$src_name" || true)"
    if [[ -z "$found" ]]; then
      found="$(find_system_icon_source "$src_name" || true)"
    fi
    [[ -n "$found" ]] || continue
    # Reversal is monochrome; recolour it for the dark shell the same way the
    # theme's own installer does, then rasterise straight into place.
    local tmp="$out.tmp"
    if sed "$LIGHTEN" "$found" > "$WORK/in.svg" && render_one "$WORK/in.svg" "$tmp" 128 "$tool"; then
      mv -f "$tmp" "$out"
      rendered=$((rendered + 1))
    fi
  done < <(sort -u "$list")

  say "icons: $rendered/$wanted rendered into $ICONS"
  render_local_svgs "$tool"
}

# SVGs the project ships itself (assets/icons/*.svg) are already coloured, so
# they are rasterised verbatim -- no recolouring -- into assets/icons/<name>.png.
render_local_svgs() {
  local tool="$1" n=0 svg png
  for svg in "$ICONS"/*.svg; do
    [[ -f "$svg" ]] || continue
    png="${svg%.svg}.png"
    if [[ $FORCE -eq 0 && -s "$png" ]]; then n=$((n + 1)); continue; fi
    if render_one "$svg" "$png.tmp" 128 "$tool"; then
      mv -f "$png.tmp" "$png"
      n=$((n + 1))
    fi
  done
  [[ $n -gt 0 ]] && say "local svgs: $n rasterised into $ICONS"
  return 0
}

# ---------------------------------------------------------------------- fonts
fetch_fonts() {
  mkdir -p "$FONTS"
  local target="$FONTS/MuternVF.ttf"
  if [[ $FORCE -eq 0 && -s "$target" ]]; then
    say "font: already present"
    return 0
  fi

  # Preferred: the raw file over HTTP, which avoids a full clone. Fall back to a
  # shallow clone when git is unavailable or the CDN path changes.
  local ok=0
  if have curl && curl -fsSL --retry 2 -o "$target.tmp" \
        "https://raw.githubusercontent.com/vivescene/MuternVF/master/MuternVF.ttf" 2>/dev/null &&
     [[ -s "$target.tmp" ]]; then
    ok=1
  elif have wget && wget -q -O "$target.tmp" \
        "https://raw.githubusercontent.com/vivescene/MuternVF/master/MuternVF.ttf" 2>/dev/null &&
       [[ -s "$target.tmp" ]]; then
    ok=1
  fi
  if [[ $ok -eq 0 ]]; then
    say "fetching MuternVF via git"
    if have git && git clone --depth 1 "$MUTERNVF_REPO" "$WORK/muternvf" 2>/dev/null &&
       [[ -s "$WORK/muternvf/MuternVF.ttf" ]]; then
      cp -f "$WORK/muternvf/MuternVF.ttf" "$target.tmp"
      ok=1
    fi
  fi

  if [[ $ok -eq 0 || ! -s "$target.tmp" ]]; then
    rm -f "$target.tmp"
    say "warning: could not fetch MuternVF; the shell falls back to a system font"
    return 1
  fi
  mv -f "$target.tmp" "$target"

  # Static Text instances are the fallback path in text.cpp for builds of
  # FreeType that cannot set variation coordinates. They are cheap to copy, so
  # they ride along when the clone is what we used.
  if [[ -d "$WORK/muternvf/ttf/Text" ]]; then
    for w in Regular Medium Bold; do
      local src="$WORK/muternvf/ttf/Text/MuternVF-Text$w.ttf"
      [[ -s "$src" ]] && cp -f "$src" "$FONTS/MuternVF-Text$w.ttf"
    done
  fi
  say "font: MuternVF.ttf -> $target"
}

if [[ $WANT_FONTS -eq 1 ]]; then fetch_fonts || true; fi
if [[ $WANT_ICONS -eq 1 ]]; then fetch_icons || true; fi

# A stamp lets run.sh skip this entirely on every later start.
if [[ -s "$FONTS/MuternVF.ttf" || -d "$ICONS" ]]; then
  printf 'fonts=%s\nicons=%s\n' \
    "$([[ -s "$FONTS/MuternVF.ttf" ]] && echo ok || echo missing)" \
    "$(find "$ICONS" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l | tr -d ' ')" \
    > "$ASSETS/.stamp"
fi
say "done"