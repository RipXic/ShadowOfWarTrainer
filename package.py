"""Build the release folder + zip from build\ (run build.bat first).

    python package.py 1.0.0

Output: release\ShadowOfWarTrainer\ (ready-to-run folder) and release\ShadowOfWarTrainer_<version>.zip,
plus the SHA-256 of the zip and of both binaries.
"""
import hashlib, os, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD, DATA = os.path.join(ROOT, 'build'), os.path.join(ROOT, 'data')
VERSION = sys.argv[1] if len(sys.argv) > 1 else '1.0.0'
OUT = os.path.join(ROOT, 'release', 'ShadowOfWarTrainer')


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    bins = ('ShadowOfWarTrainer.dll', 'ShadowOfWarTrainer.exe')
    for b in bins:
        assert os.path.isfile(os.path.join(BUILD, b)), b + ' missing - run build.bat first'
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    for b in bins:
        shutil.copy2(os.path.join(BUILD, b), OUT)
    for f in ('panel.json', 'signatures.json', 'config.json', 'README.txt'):
        shutil.copy2(os.path.join(DATA, f), OUT)
    for d in ('skin', 'scripts', 'data'):
        shutil.copytree(os.path.join(DATA, d), os.path.join(OUT, d))
    z = os.path.join(ROOT, 'release', 'ShadowOfWarTrainer_%s.zip' % VERSION)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fs in os.walk(OUT):
            for f in sorted(fs):
                p = os.path.join(dp, f)
                zf.write(p, os.path.join('ShadowOfWarTrainer', os.path.relpath(p, OUT)))
    print('packaged', z, '%.1f MB' % (os.path.getsize(z) / 1e6))
    for p in [z] + [os.path.join(OUT, b) for b in bins]:
        print('SHA-256', sha256(p), os.path.basename(p))


if __name__ == '__main__':
    main()
