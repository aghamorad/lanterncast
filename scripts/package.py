#!/usr/bin/env python3
"""Make an unsigned widget from the current native binary; no SDK signing keys."""
from pathlib import Path
import zipfile
root=Path(__file__).resolve().parents[1]
native=root/'nacl/airplay-arm.nexe'
if not native.is_file():raise SystemExit('Build the ARM module first, or use the prebuilt release widget.')
sources=[p for p in (root/'nacl').rglob('*') if p.suffix in ('.c','.cc','.h')]
if any(p.stat().st_mtime>native.stat().st_mtime for p in sources):raise SystemExit('Native binary is older than source. Rebuild before packaging.')
files=['config.xml','index.html','res/icon.png','nacl/airplay.nmf','nacl/airplay-arm.nexe']
out=root/'dist/LanternCast-0.2.0-unsigned.wgt';out.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
 for name in files:z.write(root/name,name)
print(out)
