#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# SCAV_ENVY_CACHE_ROOT is scav's own spelling of envy's ENVY_CACHE_ROOT, and it
# exists because that one is global: exported from a shell profile it would
# retarget the cache of every other envy project on the machine, including ones
# that chose a project-local sandbox on purpose. A scav-scoped name can live in a
# profile safely, and translating it here -- rather than declaring it in the
# manifest -- is what keeps envy's launcher and its binary agreeing on one path.
# An explicit ENVY_CACHE_ROOT still wins; it is envy's documented override.
if [[ -z "${ENVY_CACHE_ROOT:-}" && -n "${SCAV_ENVY_CACHE_ROOT:-}" ]]; then
  export ENVY_CACHE_ROOT="$SCAV_ENVY_CACHE_ROOT"
fi

./bin/envy sync
exec "$(./bin/envy product python3)" tools/build.py --no-sync "$@"
