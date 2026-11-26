"""Build the release folder + zip from build\ (run build.bat first).

    python package.py 1.2.0

Output: release\package\ (ready to copy into the game's x64 folder) and release\ShadowOfWarTrainer_<version>.zip,
plus the SHA-256 of the zip and of both DLLs. Zip layout (everything goes next to ShadowOfWar.exe):

    xinput9_1_0.dll            the loader: the game loads it by itself and it loads the trainer
    INSTALL.txt
    ShadowOfWarTrainer\        the trainer: ShadowOfWarTrainer.dll, panel.json, skin\, lang\ ...
"""
import hashlib, os, re, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD, DATA = os.path.join(ROOT, 'build'), os.path.join(ROOT, 'data')
VERSION = sys.argv[1] if len(sys.argv) > 1 else '1.2.0'
OUT = os.path.join(ROOT, 'release', 'package')     # its own folder: a trainer folder in use keeps its settings and logs
TRAINER = os.path.join(OUT, 'ShadowOfWarTrainer')
# words that must never ship in a text file (developer paths, user names, tools)
NEVER = re.compile(r'[A-Z]:\\\\?(Users|ShadowOf|SteamLibrary)|Ripxi\\|Claude|Anthropic|scratchpad', re.I)


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    for b in ('ShadowOfWarTrainer.dll', 'xinput9_1_0.dll'):
        assert os.path.isfile(os.path.join(BUILD, b)), b + ' missing - run build.bat first'
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(TRAINER)
    shutil.copy2(os.path.join(BUILD, 'xinput9_1_0.dll'), OUT)
    shutil.copy2(os.path.join(DATA, 'INSTALL.txt'), OUT)
    shutil.copy2(os.path.join(BUILD, 'ShadowOfWarTrainer.dll'), TRAINER)
    for f in ('panel.json', 'signatures.json', 'config.json', 'README.txt', 'INSTALL.txt'):
        shutil.copy2(os.path.join(DATA, f), TRAINER)
    for d in ('skin', 'scripts', 'data', 'lang'):
        shutil.copytree(os.path.join(DATA, d), os.path.join(TRAINER, d))
    for dp, _, fs in os.walk(OUT):
        for f in fs:
            if f.endswith(('.txt', '.json', '.hks')):
                t = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                m = NEVER.search(t)
                assert not m, '%s contains %r' % (f, m.group(0))
    z = os.path.join(ROOT, 'release', 'ShadowOfWarTrainer_%s.zip' % VERSION)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fs in os.walk(OUT):
            for f in sorted(fs):
                p = os.path.join(dp, f)
                zf.write(p, os.path.relpath(p, OUT))
    print('packaged', z, '%.1f MB' % (os.path.getsize(z) / 1e6))
    for p in (z, os.path.join(TRAINER, 'ShadowOfWarTrainer.dll'), os.path.join(OUT, 'xinput9_1_0.dll')):
        print('SHA-256', sha256(p), os.path.basename(p))


if __name__ == '__main__':
    main()
