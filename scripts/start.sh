#!/bin/sh
# Puts `bide` on your PATH. Run once:  sh scripts/start.sh
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bindir=${BINDIR:-$HOME/.local/bin}

make -C "$root"
mkdir -p "$bindir"

# A two-line wrapper rather than a symlink: MSYS/Git Bash silently copies on
# `ln -s` unless it is configured for native links, and a copy goes stale the
# next time you `make`. This always runs the binary that is there now.
printf '#!/bin/sh\nexec "%s/bin/main" "$@"\n' "$root" > "$bindir/bide"
chmod +x "$bindir/bide"
echo "installed $bindir/bide -> $root/bin/main"

case ":$PATH:" in
    *":$bindir:"*) ;;
    *) echo
       echo "note: $bindir is not on your PATH. Add this to your ~/.bashrc or ~/.zshrc:"
       echo "      export PATH=\"\$PATH:$bindir\"" ;;
esac
