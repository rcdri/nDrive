#!/usr/bin/env python3
"""Write build_id.h: a SHA-256 over the sources and build settings.

The resident framework refuses to mix two different builds of the same
program in one boot, so every change must change this ID."""
import hashlib, sys
from pathlib import Path

out, flags, *sources = sys.argv[1:]
h = hashlib.sha256(flags.encode())
for p in sorted(sources):
    h.update(Path(p).name.encode() + b'\0' + Path(p).read_bytes() + b'\0')
text = '#define NDRIVE_BUILD_ID "%s"\n' % h.hexdigest()
o = Path(out)
if not o.exists() or o.read_text() != text:
    o.write_text(text)
