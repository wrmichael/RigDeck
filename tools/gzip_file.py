"""Build step: gzip one file (level 9, no timestamp, so the same input gives the same bytes).
Usage: gzip_file.py <input> <output>. main/CMakeLists.txt uses it for the web pages kept in flash."""
import gzip
import sys

data = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(gzip.compress(data, 9, mtime=0))
