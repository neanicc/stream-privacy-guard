#!/usr/bin/env bash
# The single entry point CI calls (.github/workflows/check.yml). Owned by lane:build.
#
#   scripts/ci/run.sh setup            fetch pinned deps/models into $SPG_CACHE_DIR
#   scripts/ci/run.sh build            configure and build everything
#   scripts/ci/run.sh test             run unit and integration tests
#   scripts/ci/run.sh package          write installable artifacts to dist/ (CI uploads them)
#   scripts/ci/run.sh eval [--local]   setup + build, then run eval suites and score them
#                                      against eval/thresholds.toml
#
# setup fetches the archives pinned in deps/manifest.json (SHA-256 verified)
# and, for the OBS plugin, builds libobs from the pinned OBS sources on macOS and
# Windows (build-aux/build_libobs.py; cached after the first run);
# build configures CMake against them and uses the pinned ccache; test runs
# ctest, which fails when no tests are registered. A stage whose inputs do not
# exist yet reports that and succeeds; once they exist it must do its real work.
# Never make a stage succeed without exercising what it claims to check.
set -euo pipefail

stage="${1:-}"
shift || true

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

export SPG_CACHE_DIR="${SPG_CACHE_DIR:-$HOME/.cache/spg}"
build_dir="${SPG_BUILD_DIR:-build}"
mkdir -p "$SPG_CACHE_DIR"

has_build() { [[ -f CMakeLists.txt ]]; }

# Windows runners expose `python`, not always `python3`.
py="$(command -v python3 || command -v python || true)"

# The OBS plugin (plugin/) builds by default and CI always builds it. Set
# SPG_BUILD_PLUGIN=0 to skip its inputs (libobs, obs-deps) in setup and build.
build_plugin="${SPG_BUILD_PLUGIN:-1}"
case "$build_plugin" in
  1|ON|on|true) build_plugin=1 ;;
  0|OFF|off|false) build_plugin=0 ;;
  *) echo "::error::SPG_BUILD_PLUGIN must be 0 or 1 (got '$build_plugin')." >&2; exit 2 ;;
esac

is_windows() { case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac; }

# Git Bash on Windows: Ninja there would pick MinGW gcc, which cannot link
# MSVC-built ONNX Runtime or libobs. Use the newest installed Visual Studio
# (runner images move between 2022 and 2026); SPG_CMAKE_GENERATOR overrides.
windows_generator() {
  if [[ -n "${SPG_CMAKE_GENERATOR:-}" ]]; then
    echo "$SPG_CMAKE_GENERATOR"
    return
  fi
  # vswhere's documented fixed location.
  local vswhere vs_major=""
  vswhere="$(cygpath -u "C:/Program Files (x86)")/Microsoft Visual Studio/Installer/vswhere.exe"
  if [[ -x "$vswhere" ]]; then
    vs_major="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationVersion | tr -d '\r' | cut -d. -f1)"
  fi
  case "$vs_major" in
    18) echo "Visual Studio 18 2026" ;;
    17) echo "Visual Studio 17 2022" ;;
    *) echo "::error::no Visual Studio with C++ tools found (vswhere: '${vs_major}')." >&2; return 1 ;;
  esac
}

case "$stage" in
  setup)
    if ! has_build; then
      echo "::notice::setup: no CMakeLists.txt yet (SPG-01 not landed)."
      exit 0
    fi
    fetch_args=(--cache "$SPG_CACHE_DIR")
    if [[ "$build_plugin" == 0 ]]; then fetch_args+=(--skip-group plugin); fi
    if [[ -f scripts/fetch_deps.py ]]; then
      "$py" scripts/fetch_deps.py "${fetch_args[@]}"
    fi
    if [[ -f scripts/fetch_models.py ]]; then
      "$py" scripts/fetch_models.py --cache "$SPG_CACHE_DIR"
    fi
    # libobs for the plugin: built from the pinned OBS sources on macOS and
    # Windows (cached in $SPG_CACHE_DIR/deps/libobs); Linux uses the pinned
    # official Ubuntu package that fetch_deps.py unpacked.
    if [[ "$build_plugin" == 1 ]]; then
      libobs_args=(--cache "$SPG_CACHE_DIR")
      if is_windows; then
        generator="$(windows_generator)"
        libobs_args=(--cache "$(cygpath -m "$SPG_CACHE_DIR")" --generator "$generator")
      fi
      "$py" build-aux/build_libobs.py "${libobs_args[@]}"
    fi
    ;;

  build)
    if ! has_build; then
      echo "::notice::build: no CMakeLists.txt yet (SPG-01 not landed)."
      exit 0
    fi
    generator_args=()
    cache_dir="$SPG_CACHE_DIR"
    if is_windows; then
      generator="$(windows_generator)"
      echo "build: using CMake generator '$generator'"
      generator_args=(-G "$generator" -A x64)
      # CMake is a native Windows program; hand it C:/... rather than /c/...
      cache_dir="$(cygpath -m "$SPG_CACHE_DIR")"
    elif command -v ninja >/dev/null 2>&1; then
      generator_args=(-G Ninja)
    fi
    plugin_flag=OFF
    if [[ "$build_plugin" == 1 ]]; then plugin_flag=ON; fi
    # ccache (pinned in deps/manifest.json) keeps its cache under $SPG_CACHE_DIR,
    # which CI restores between runs. CMakeLists.txt decides whether to use it.
    export CCACHE_DIR="${CCACHE_DIR:-$SPG_CACHE_DIR/ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$root}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-500M}"
    # The +-expansion keeps bash 3.2 (macOS /bin/bash) happy under set -u.
    cmake -S . -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
      -DCMAKE_BUILD_TYPE=Release -DSPG_CACHE_DIR="$cache_dir" -DSPG_BUILD_PLUGIN="$plugin_flag"
    ccache_bin="$(sed -n 's/^SPG_CCACHE_PROGRAM:FILEPATH=//p' "$build_dir/CMakeCache.txt")"
    if [[ -n "$ccache_bin" && "$ccache_bin" != *NOTFOUND ]]; then
      "$ccache_bin" --zero-stats >/dev/null
    else
      ccache_bin=""
    fi
    cmake --build "$build_dir" --config Release --parallel
    if [[ -n "$ccache_bin" ]]; then
      echo "ccache statistics for this build:"
      "$ccache_bin" --show-stats
    fi
    ;;

  test)
    if ! has_build; then
      echo "::notice::test: no CMakeLists.txt yet (SPG-01 not landed)."
      exit 0
    fi
    ctest --test-dir "$build_dir" -C Release --output-on-failure --no-tests=error
    ;;

  package)
    if [[ ! -f scripts/package.sh ]]; then
      echo "::notice::package: scripts/package.sh does not exist yet (SPG-17)."
      exit 0
    fi
    bash scripts/package.sh "$build_dir" dist
    ;;

  eval)
    mode=ci
    if [[ "${1:-}" == "--local" ]]; then mode=local; fi
    # Always score a fresh build of the current tree, never a stale one.
    if has_build; then
      bash "$root/scripts/ci/run.sh" setup
      bash "$root/scripts/ci/run.sh" build
    fi
    # run_eval.py runs spg-cli from $build_dir; without it the verdict is FAIL.
    "$py" eval/run_eval.py --mode "$mode" --build-dir "$build_dir" \
      --thresholds eval/thresholds.toml --out eval-out
    ;;

  *)
    echo "usage: $0 {setup|build|test|package|eval [--local]}" >&2
    exit 2
    ;;
esac
