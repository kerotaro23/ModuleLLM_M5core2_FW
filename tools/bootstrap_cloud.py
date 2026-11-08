#!/usr/bin/env python3
"""Assemble pinned upstream tools for Linux x86_64 where the PIO registry is unavailable.
TLS stays enabled; Espressif toolchain is checked against its published SHA-256.
Local package metadata adapts upstream version numbers to PlatformIO's encoding.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(os.environ.get('PLATFORMIO_CORE_DIR', '/workspace/.platformio'))
ROOT.mkdir(parents=True, exist_ok=True)
assert platform.system() == 'Linux' and platform.machine() == 'x86_64'

def run(*args):
    subprocess.run(args, check=True)

def metadata(path, name, version, owner, manifest=False):
    if manifest:
        (path / 'package.json').write_text(json.dumps({'name': name, 'version': version,
            'description': 'Locally packaged pinned upstream release; see tools/bootstrap_cloud.py'}))
    (path / '.piopm').write_text(json.dumps({'type': 'tool' if path.parent.name == 'packages' else 'platform',
        'name': name, 'version': version, 'spec': {'owner': owner, 'name': name, 'requirements': version}}))

def clone(repo, tag, sha, dest):
    if dest.exists():
        actual = subprocess.check_output(['git', '-C', str(dest), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != sha: raise RuntimeError(f'Unexpected checkout at {dest}')
        return
    run('git', 'clone', '--depth', '1', '--branch', tag, 'https://github.com/' + repo + '.git', str(dest))
    assert subprocess.check_output(['git', '-C', str(dest), 'rev-parse', 'HEAD'], text=True).strip() == sha

packages = ROOT / 'packages'
packages.mkdir(exist_ok=True)
platform_dir = ROOT / 'platforms' / 'espressif32'
clone('platformio/platform-espressif32', 'v6.10.0', '4d72e44ebe60c2fde58496f9a72a0e0dd4861cf7', platform_dir)
metadata(platform_dir, 'espressif32', '6.10.0', 'platformio')
framework = packages / 'framework-arduinoespressif32'
clone('espressif/arduino-esp32', '2.0.17', '5e19e086c43d0fa5e5a596497ff8f11a0a43f6c2', framework)
metadata(framework, 'framework-arduinoespressif32', '3.20017.0', 'platformio')

toolchain = packages / 'toolchain-xtensa-esp32'
if not toolchain.exists():
    url = ('https://github.com/espressif/crosstool-NG/releases/download/esp-2021r2-patch5/'
           'xtensa-esp32-elf-gcc8_4_0-esp-2021r2-patch5-linux-amd64.tar.gz')
    digest = '8ef14e0409c2011b41e504a30f70d3e35287313a795d1f2462ad2cd0e2052d37'
    with tempfile.TemporaryDirectory(dir=packages) as tmp:
        archive = Path(tmp) / 'toolchain.tar.gz'
        urllib.request.urlretrieve(url, archive)
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, 'Toolchain checksum mismatch'
        with tarfile.open(archive) as tar: tar.extractall(tmp, filter='data')
        (Path(tmp) / 'xtensa-esp32-elf').rename(toolchain)
metadata(toolchain, 'toolchain-xtensa-esp32', '8.4.0+2021r2-patch5', 'espressif', True)

# PyPI distributes official SCons/esptool Python releases; no registry mirror is used.
run(sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check', 'scons==4.8.1', 'esptool==4.5.1')
for name, version, launcher in [
    ('tool-scons', '4.40801.0', 'from SCons.Script import main\nmain()\n'),
    ('tool-esptoolpy', '1.40501.0', 'import sys\nsys.path.pop(0)\nimport esptool\nesptool._main()\n')]:
    dest = packages / name
    dest.mkdir(exist_ok=True)
    (dest / ('scons.py' if name == 'tool-scons' else 'esptool.py')).write_text(launcher)
    metadata(dest, name, version, 'platformio', True)
print('Pinned upstream build tools prepared:', ROOT)
