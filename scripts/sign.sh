#!/usr/bin/env bash
# Usage: sign.sh unsigned.wgt SamsungCertificateProfile
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INPUT="${1:?Supply the unsigned release widget}"
PROFILE="${2:?Supply your own Samsung certificate profile name}"
TIZEN="${TIZEN:-tizen}"
mkdir -p "$ROOT/.build" "$ROOT/dist"
STAGE="$(mktemp -d "$ROOT/.build/sign.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
python3 - "$INPUT" "$STAGE" <<'PY'
import pathlib,sys,zipfile
stage=pathlib.Path(sys.argv[2]).resolve()
with zipfile.ZipFile(sys.argv[1]) as z:
    for item in z.infolist():
        target=(stage/item.filename).resolve()
        if not target.is_relative_to(stage):raise SystemExit('Unsafe path in widget')
    z.extractall(stage)
for name in ['author-signature.xml','signature1.xml','signature2.xml']:
    (stage/name).unlink(missing_ok=True)
(stage/'.project').write_text('''<?xml version="1.0" encoding="UTF-8"?><projectDescription><name>LanternCast</name><comment/><projects/><buildSpec/><natures><nature>org.eclipse.wst.jsdt.core.jsNature</nature><nature>org.tizen.web.project.WebNature</nature></natures></projectDescription>''')
(stage/'.tproject').write_text('''<?xml version="1.0" encoding="UTF-8"?><tproject xmlns="http://www.tizen.org/tproject"><platforms><platform><name>tv-samsung-7.0</name></platform></platforms></tproject>''')
PY
"$TIZEN" package -t wgt -s "$PROFILE" -- "$STAGE"
BUILT="$STAGE/LanternCast.wgt"
[ -f "$BUILT" ] || { echo 'Tizen did not produce LanternCast.wgt'; exit 1; }
VERSION="$(python3 - "$ROOT/config.xml" <<'PYXML'
import sys,xml.etree.ElementTree as ET
print(ET.parse(sys.argv[1]).getroot().attrib['version'])
PYXML
)"
cp "$BUILT" "$ROOT/dist/LanternCast-$VERSION-signed.wgt"
echo "$ROOT/dist/LanternCast-$VERSION-signed.wgt"
