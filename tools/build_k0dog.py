#!/usr/bin/env python3
"""Builds the K0DOG game for the RigDeck site. The game is not in the firmware: RigDeck downloads it from
docs/k0dog/ to its microSD card and serves it from there.
  main/web/k0dog.src.html + the Audiowide subset from main/web/index.html
  -> docs/k0dog/k0dog.html.gz (what the boards download), docs/k0dog/k0dog.jpg, docs/k0dog/k0dog.json
     (version and sizes), and docs/k0dog.html + docs/k0dog.jpg to play straight from the site."""
import gzip, hashlib, json, os, re, shutil
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
web = os.path.join(root, 'main', 'web')
src = open(os.path.join(web, 'k0dog.src.html'), encoding='utf-8').read()
idx = open(os.path.join(web, 'index.html'), encoding='utf-8').read()
face = re.search(r'@font-face\{font-family:"Audiowide RD";src:url\(data:font/ttf;base64,[A-Za-z0-9+/=]+\)[^}]*\}', idx).group(0)
page = src.replace('/*AUDIOWIDE*/', face).encode('utf-8')
jpg = open(os.path.join(root, 'assets', 'k0dog.jpg'), 'rb').read()
out = os.path.join(root, 'docs', 'k0dog')
os.makedirs(out, exist_ok=True)
gz = gzip.compress(page, 9, mtime=0)
open(os.path.join(out, 'k0dog.html.gz'), 'wb').write(gz)
open(os.path.join(out, 'k0dog.jpg'), 'wb').write(jpg)
ver = hashlib.sha1(gz + jpg).hexdigest()[:10]
json.dump({'version': ver, 'k0dog.html.gz': len(gz), 'k0dog.jpg': len(jpg)}, open(os.path.join(out, 'k0dog.json'), 'w'))
open(os.path.join(root, 'docs', 'k0dog.html'), 'wb').write(page)
shutil.copy(os.path.join(root, 'assets', 'k0dog.jpg'), os.path.join(root, 'docs', 'k0dog.jpg'))
print('K0DOG', ver, len(page), 'bytes,', len(gz), 'gzipped,', len(jpg), 'jpg')
