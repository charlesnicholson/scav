#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The cache lives in out/ by default, so `rm -rf out` removes every tool,
# package and build artifact. Redirecting it elsewhere is a power-user move and
# stays opt-in.
#
# `SCAV_ENVY_CACHE_ROOT` rather than envy's own `ENVY_CACHE_ROOT`, because that
# one is global: left in a shell profile it would retarget every envy project on
# the machine, including ones that chose a project-local sandbox on purpose.
# A gitignored `.env` is the place to write it once -- Conductor copies `.env*`
# into every new workspace, so a worktree inherits it without a second step.
#
# Parsed, never sourced: a stray line in `.env` must not be able to run. This
# repeats in build.bat and tools/build.py because envy is invoked below, before
# there is a python to do it in.
cache_from_dotenv() {
  [[ -f .env ]] || return 0
  local value
  value="$(sed -n 's/^[[:space:]]*SCAV_ENVY_CACHE_ROOT[[:space:]]*=[[:space:]]*//p' .env |
    tail -n 1 | tr -d "\"'" )"
  [[ -n "$value" ]] || return 0
  [[ "$value" == "~" || "$value" == "~/"* ]] && value="$HOME${value:1}"
  printf '%s' "$value"
}

if [[ -z "${ENVY_CACHE_ROOT:-}" ]]; then
  scoped="${SCAV_ENVY_CACHE_ROOT:-$(cache_from_dotenv)}"
  [[ -n "$scoped" ]] && export ENVY_CACHE_ROOT="$scoped" SCAV_ENVY_CACHE_ROOT="$scoped"
fi

./bin/envy sync
exec "$(./bin/envy product python3)" tools/build.py --no-sync "$@"
