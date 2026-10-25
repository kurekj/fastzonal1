#!/usr/bin/env bash
# Fetch the benchmark fixtures into benchmarks/fixtures/ and verify them against manifest.json.
#
#   bash benchmarks/fetch_fixtures.sh            # download the archive named in manifest.json
#   FZ_FIXTURE_ARCHIVE=/path/fixtures-v1.tar.zst bash benchmarks/fetch_fixtures.sh   # local copy
#
# Requires: python3, curl, tar with zstd support (or zstd on PATH).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/fixtures"
manifest="$dest/manifest.json"
[ -f "$manifest" ] || { echo "missing $manifest" >&2; exit 1; }

url=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['archive']['url'])" "$manifest")
want=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['archive']['sha256'] or '')" "$manifest")
archive="${FZ_FIXTURE_ARCHIVE:-}"
if [ -z "$archive" ]; then
    case "$url" in
        *TODO*) echo "manifest.json has no published archive yet; set FZ_FIXTURE_ARCHIVE to a local copy" >&2; exit 1 ;;
    esac
    archive="$dest/fixtures-v1.tar.zst"
    echo "downloading $url"
    curl -sSL -o "$archive" "$url"
fi
if [ -n "$want" ]; then
    got=$(python3 -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$archive")
    [ "$got" = "$want" ] || { echo "sha256 mismatch: $got != $want" >&2; exit 1; }
fi
if tar --help 2>/dev/null | grep -q zstd; then
    tar --zstd -xf "$archive" -C "$dest"
else
    zstd -dc "$archive" | tar -xf - -C "$dest"
fi
# verify every file listed in the manifest
python3 - "$manifest" "$dest" <<'EOF'
import hashlib, json, sys
from pathlib import Path
manifest = json.load(open(sys.argv[1])); dest = Path(sys.argv[2]); bad = 0
for fid, entry in manifest["fixtures"].items():
    for name, info in entry["files"].items():
        p = dest / fid / name
        if not p.exists():
            print(f"missing {p}"); bad += 1; continue
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        if h != info["sha256"]:
            print(f"hash mismatch {p}"); bad += 1
print("fixtures verified" if not bad else f"{bad} problems"); sys.exit(1 if bad else 0)
EOF
