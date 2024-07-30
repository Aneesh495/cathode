#!/usr/bin/env bash
# ==========================================================================
# cathode.sh — build-and-run launcher for CATHODE.
#
#   ./cathode.sh                 build (if needed) and run the interactive demo
#   ./cathode.sh <scene>         start on a named scene, e.g. ./cathode.sh plasma
#   ./cathode.sh <scene> -q N    cells to paint (default 2000). Bigger picture
#                                but lower fps; the terminal emulator's per-cell
#                                cost is the limit. Try 800 (fast) to 6000 (big).
#   ./cathode.sh --list          list every scene
#   ./cathode.sh --check         verify the terminal can actually display it
#   ./cathode.sh --shot <scene>  render one PNG of a scene (headless, no TTY)
#   ./cathode.sh --song out.wav  render the built-in chiptune to a WAV
#   ./cathode.sh --test          run the full cross-language test suite
#
# The demo needs a TRUECOLOR terminal (24-bit SGR) and a font with the Unicode
# upper-half-block U+2580. --check tells you if yours qualifies.
# ==========================================================================
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

BIN=build/bin/cathode
CAP=build/bin/capture

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

build_if_needed() {
  # Rebuild whenever a source file is newer than the binary (the Makefile does
  # the real dependency tracking; this just avoids a pointless make on every run).
  if [[ ! -x $BIN ]] || [[ -n "$(find src include rustsrc/src Makefile -newer "$BIN" -type f 2>/dev/null | head -1)" ]]; then
    info "building…"
    make -s all || die "build failed — run 'make' to see the errors"
  fi
}

check_terminal() {
  local ok=0
  printf 'terminal check\n'
  printf '  TERM        : %s\n' "${TERM:-unset}"
  printf '  COLORTERM   : %s\n' "${COLORTERM:-unset}"
  local size
  size="$( (stty size 2>/dev/null || echo '? ?') )"
  printf '  size (rows cols): %s\n' "$size"

  if [[ "${COLORTERM:-}" == "truecolor" || "${COLORTERM:-}" == "24bit" ]]; then
    printf '  truecolor   : \033[32myes\033[0m (COLORTERM)\n'; ok=1
  else
    printf '  truecolor   : \033[33munknown\033[0m — COLORTERM is not truecolor.\n'
    printf '                If the gradient below is smooth, you are fine.\n'
  fi

  # 24-bit gradient probe: if the terminal supports truecolor this is a smooth
  # ramp; on a 256-color terminal it visibly bands.
  printf '  gradient    : '
  local i r g b
  for ((i = 0; i < 60; i++)); do
    r=$((255 * i / 59)); g=$((80 + 100 * i / 59)); b=$((255 - 200 * i / 59))
    printf '\033[48;2;%d;%d;%dm ' "$r" "$g" "$b"
  done
  printf '\033[0m\n'

  # half-block glyph probe
  printf '  half-block  : \033[38;2;255;80;80m\033[48;2;80;80;255m▀▀▀▀▀▀\033[0m'
  printf '  <- should be 6 blocks, red over blue (no boxes/gaps)\n'
  return $((1 - ok))
}

list_scenes() {
  build_if_needed
  "$BIN" --help | sed -n '/^scenes/,$p'
}

main() {
  local arg="${1:-}"
  case "$arg" in
    --list|-l)
      list_scenes
      ;;
    --check)
      check_terminal || true
      ;;
    --test)
      info "running the full suite (C + Rust + C++ + golden + integration)…"
      make -s test-all
      ;;
    --shot)
      build_if_needed
      local scene="${2:-plasma}" out="${3:-/tmp/cathode_${2:-plasma}.png}"
      "$CAP" "$scene" 90 "$out" 320 240
      info "wrote $out"
      ;;
    --song)
      build_if_needed
      local out="${2:-/tmp/cathode_song.wav}"
      "$CAP" --song "$out" 2 44100 0.35
      info "wrote $out — play it with: afplay $out"
      ;;
    --gif)
      build_if_needed
      local scene="${2:-plasma}" out="${3:-/tmp/cathode_${2:-plasma}.gif}"
      "$CAP" --gif "$scene" 120 "$out" 240 180
      info "wrote $out"
      ;;
    -h|--help)
      sed -n '3,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      ;;
    *)
      # interactive run (optionally starting on a named scene)
      if [[ ! -t 1 ]]; then
        die "stdout is not a terminal. The demo needs a real TTY.
       Try:  ./cathode.sh --shot plasma   (renders a PNG instead)"
      fi
      build_if_needed
      local rows cols
      read -r rows cols < <(stty size 2>/dev/null || echo '24 80')
      if (( cols < 40 || rows < 12 )); then
        printf '\033[33mwarning:\033[0m terminal is %sx%s — quite small. Maximize it for the full effect.\n' "$cols" "$rows"
        sleep 1
      fi
      info "launching (q or ESC to quit, ? for help, n/p to change scene)"
      # pass through a -q/--quality N pair if given
      local qual=()
      if [[ "${2:-}" == "-q" || "${2:-}" == "--quality" ]] && [[ -n "${3:-}" ]]; then
        qual=(--quality "$3")
      fi
      exec "$BIN" ${arg:+"$arg"} "${qual[@]+"${qual[@]}"}"
      ;;
  esac
}

main "$@"
