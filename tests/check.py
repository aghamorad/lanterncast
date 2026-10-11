#!/usr/bin/env python3
"""Independent host checks for wire integer encoding and SHA512."""
import hashlib,pathlib,plistlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='airplay-check-') as tmp:
    tmp=pathlib.Path(tmp)
    subprocess.run(['clang++','-std=c++11','-I'+str(root/'nacl'),str(root/'tests/plist-integers.cc'),str(root/'nacl/bplist.cc'),'-o',str(tmp/'plist')],check=True)
    actual=plistlib.loads(subprocess.check_output([str(tmp/'plist')]))
    assert actual==[0,255,256,65535,65536,0x3fffffc,0x1e527ffff7,0xffffffffffffffff],actual
    subprocess.run(['clang',str(root/'tests/sha512.c'),str(root/'nacl/sha512.c'),'-o',str(tmp/'hash')],check=True)
    assert subprocess.check_output([str(tmp/'hash')]).decode()==hashlib.sha512(b'abc').hexdigest()
print('PASS binary plist integer boundaries and SHA512 independent checks')
