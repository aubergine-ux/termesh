#!/bin/sh
# Download a few CC0 models from the Khronos glTF Sample Assets into ./samples.
#
#   scripts/fetch-samples.sh
#   ./build/termesh samples/BoomBox.glb
set -eu

base="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"
dest="${1:-samples}"
mkdir -p "$dest"

# Single-file binary models.
for name in BoomBox Avocado Lantern BarramundiFish WaterBottle Corset; do
    if [ ! -f "$dest/$name.glb" ]; then
        echo "fetching $name.glb"
        curl -fsSL -o "$dest/$name.glb" "$base/$name/glTF-Binary/$name.glb"
    fi
done

# Flight Helmet ships as a .gltf with external buffers and textures: fetch
# the .gltf, then every file it refers to.
helmet="$dest/FlightHelmet"
mkdir -p "$helmet"
if [ ! -f "$helmet/FlightHelmet.gltf" ]; then
    echo "fetching FlightHelmet"
    curl -fsSL -o "$helmet/FlightHelmet.gltf" "$base/FlightHelmet/glTF/FlightHelmet.gltf"
fi
grep -oE '"uri"[[:space:]]*:[[:space:]]*"[^"]+"' "$helmet/FlightHelmet.gltf" |
    sed -E 's/.*"([^"]+)"$/\1/' |
    while read -r file; do
        [ -f "$helmet/$file" ] || curl -fsSL -o "$helmet/$file" "$base/FlightHelmet/glTF/$file"
    done

echo "done - try: termesh $helmet/FlightHelmet.gltf"
