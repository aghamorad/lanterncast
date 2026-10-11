#!/usr/bin/env bash
# Usage: install.sh TV_IP signed.wgt
set -euo pipefail
TV="${1:?Supply the TV IPv4 address}:26101"
WGT="${2:?Supply a widget signed for this TV}"
SDB="${SDB:-sdb}"
[ -f "$WGT" ] || { echo "Widget missing: $WGT"; exit 1; }
"$SDB" connect "$TV"
# Fixed safe remote filename, independent of local whitespace.
REMOTE="/home/owner/share/tmp/sdk_tools/LanternCast-upload.wgt"
"$SDB" -s "$TV" push "$WGT" "$REMOTE"
"$SDB" -s "$TV" shell 0 vd_appinstall MoradAP001 "$REMOTE"
echo 'Open LanternCast from the TV Apps panel. Check the installer output for success.'
