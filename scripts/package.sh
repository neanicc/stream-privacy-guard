#!/usr/bin/env bash
# Write the installable OBS plugin package for this OS to dist/ (SPG-17).
# scripts/ci/run.sh package calls it after build and test; CI uploads dist/.
#
#   scripts/package.sh [build-dir] [dist-dir] [--with-models]
#
# Stages the plugin's install component (cmake/SpgInstall.cmake), adds the
# per-OS INSTALL.txt, the GPL license and the model fetch script with its
# manifest, checks the result (build-aux/package_tool.py check) and writes
#
#   obs-spg-<version>-macos-arm64.zip     obs-spg.plugin bundle
#   obs-spg-<version>-windows-x64.zip     obs-spg/bin/64bit + obs-spg/data
#   obs-spg-<version>-linux-x64.tar.gz    obs-spg/bin/64bit + obs-spg/data
#   SHA256SUMS.txt
#
# Packages are unsigned (no signing or notarization identity exists) and carry
# no model weights. --with-models (off by default; CI never passes it) copies
# the pinned models from $SPG_CACHE_DIR into the plugin's data/models/, checked
# by SHA-256, with their licenses, for local use. Nothing is published.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="build"
dist_dir="dist"
with_models=0
positional=0
for arg in "$@"; do
  case "$arg" in
    --with-models) with_models=1 ;;
    -*) echo "usage: $0 [build-dir] [dist-dir] [--with-models]" >&2; exit 2 ;;
    *)
      case "$positional" in
        0) build_dir="$arg" ;;
        1) dist_dir="$arg" ;;
        *) echo "usage: $0 [build-dir] [dist-dir] [--with-models]" >&2; exit 2 ;;
      esac
      positional=$((positional + 1)) ;;
  esac
done

export SPG_CACHE_DIR="${SPG_CACHE_DIR:-$HOME/.cache/spg}"
py="$(command -v python3 || command -v python || true)"
if [[ -z "$py" ]]; then
  echo "::error::package: Python 3 is required." >&2
  exit 1
fi

cache_file="$build_dir/CMakeCache.txt"
if [[ ! -f "$cache_file" ]]; then
  echo "::error::package: no $cache_file; run scripts/ci/run.sh build first." >&2
  exit 1
fi
if ! grep -q '^SPG_BUILD_PLUGIN:BOOL=ON$' "$cache_file"; then
  echo "::error::package: $build_dir was configured without the OBS plugin (SPG_BUILD_PLUGIN=0); the package is the plugin." >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    [[ "$(uname -m)" == arm64 ]] || { echo "::error::package: macOS packages are Apple Silicon only." >&2; exit 1; }
    platform=macos-arm64 ;;
  Linux) platform=linux-x64 ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows-x64 ;;
  *) echo "::error::package: unsupported OS $(uname -s)." >&2; exit 1 ;;
esac

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$cache_file")"
name="obs-spg-${version}-${platform}"
if [[ "$with_models" == 1 ]]; then name="${name}-with-models"; fi

stage_parent="$build_dir/package-stage"
stage="$stage_parent/$name"
rm -rf "$stage_parent"
mkdir -p "$stage_parent"

echo "package: staging $name"
cmake --install "$build_dir" --config Release --component obs-spg --prefix "$stage"

# Next to the plugin: how to install it, its license, and the standard-library
# script that fetches the pinned models into the user cache the plugin reads.
cp "build-aux/package/INSTALL-${platform%%-*}.txt" "$stage/INSTALL.txt"
cp LICENSE "$stage/LICENSE.txt"
mkdir -p "$stage/scripts" "$stage/models"
cp scripts/fetch_models.py scripts/fetch_deps.py "$stage/scripts/"
cp models/manifest.json "$stage/models/manifest.json"

if [[ "$with_models" == 1 ]]; then
  "$py" -I build-aux/package_tool.py bundle-models --stage "$stage" --platform "$platform" \
    --cache "$SPG_CACHE_DIR"
fi

# macOS: seal the finished bundle with an ad-hoc signature (no identity, as
# obs-plugintemplate does without one), so its resources and nested libraries
# verify as one unit. This is not a Developer ID signature or notarization.
if [[ "$platform" == macos-arm64 ]]; then
  codesign --force --sign - --timestamp=none "$stage/obs-spg.plugin"
fi

check_args=(--stage "$stage" --platform "$platform")
if [[ "$with_models" == 1 ]]; then check_args+=(--with-models); fi
"$py" -I build-aux/package_tool.py check "${check_args[@]}"

mkdir -p "$dist_dir"
# Only what this script writes; anything else in dist/ is left alone.
rm -f "$dist_dir/$name.zip" "$dist_dir/$name.tar.gz"
"$py" -I build-aux/package_tool.py archive --stage "$stage" --platform "$platform" --out "$dist_dir"
ls -l "$dist_dir"
