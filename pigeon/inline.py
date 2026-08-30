#!/usr/bin/env python3
"""Drop the bundle into the page template. Kept separate from build.sh so the
substitution is a real parser and not a sed expression that breaks on the first
ampersand in the minified JavaScript."""
from pathlib import Path

here = Path(__file__).parent
page = (here / "page.html").read_text()
bundle = Path("/tmp/pigeon-bundle.js").read_text()
marker = "/*BUNDLE*/"
assert page.count(marker) == 1, "page.html must contain exactly one /*BUNDLE*/"
(here / "index.html").write_text(page.replace(marker, bundle))
