#!/usr/bin/env bash
# Bundle the pigeon view into one self-contained page.
#
# Everything ends up inside one file: the page has no directory of assets to
# serve alongside it, which is what lets this directory be copied anywhere. esbuild flattens
# three.js, the 3D tiles renderer and the game into one module, which is then
# inlined into the page. Run this after editing main.js.
#
# The externals are optional peers of tile formats this view never opens — vector
# tiles, PMTiles, zipped archives. They are reached only through dynamic imports
# on those code paths, so excluding them drops dead weight rather than breaking
# anything; bundling them would mean shipping three more decoders to render a
# city that arrives as glTF.
set -e
cd "$(dirname "$0")"

# Dependencies are pinned in package.json and fetched on demand rather than
# committed: the game ships as the bundle, so the tree is a build input, not an
# artefact, and keeping ten megabytes of it out of the repo costs one command.
[ -d node_modules ] || npm install --no-audit --no-fund
npx --yes esbuild main.js \
  --bundle --format=esm --minify --platform=browser \
  --external:pbf --external:@mapbox/vector-tile --external:pmtiles --external:zstddec --external:@zip.js/zip.js \
  --outfile=/tmp/pigeon-bundle.js
python3 inline.py
echo "# pigeon/index.html rebuilt ($(wc -c < index.html | tr -d ' ') bytes)"
