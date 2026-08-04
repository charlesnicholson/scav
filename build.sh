#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

./bin/envy sync
exec "$(./bin/envy product python3)" tools/build.py --no-sync "$@"
