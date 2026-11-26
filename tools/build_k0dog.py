#!/usr/bin/env python3
"""Builds the K0DOG game page: main/web/k0dog.src.html with the Audiowide subset from index.html inlined
-> main/web/k0dog.html (also copied to docs/k0dog.html, playable from the site) and main/web/k0dog.html.gz
(what the board serves, gzip-compressed to save flash)."""
import gzip, os, re, shutil
here = os.path.dirname(os.path.abspath(__file__))
web = os.path.join(here, '..', 'main', 'web')
src = open(os.path.join(web, 'k0dog.src.html'), encoding='utf-8').read()
idx = open(os.path.join(web, 'index.html'), encoding='utf-8').read()
face = re.search(r'@font-face\{font-family:"Audiowide RD";src:url\(data:font/ttf;base64,[A-Za-z0-9+/=]+\)[^}]*\}', idx).group(0)
out = src.replace('/*AUDIOWIDE*/', face)
open(os.path.join(web, 'k0dog.html'), 'w', encoding='utf-8').write(out)
with open(os.path.join(web, 'k0dog.html.gz'), 'wb') as f:
    f.write(gzip.compress(out.encode('utf-8'), 9, mtime=0))
shutil.copy(os.path.join(web, 'k0dog.html'), os.path.join(here, '..', 'docs', 'k0dog.html'))
print(len(out), 'bytes,', os.path.getsize(os.path.join(web, 'k0dog.html.gz')), 'gzipped')
