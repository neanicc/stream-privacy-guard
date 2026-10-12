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
# setup fetches the archives pinned in deps/manifest.json (SHA-256 verified);
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

case "$stage" in
  setup)
    if ! has_build; then
      echo "::notice::setup: no CMakeLists.txt yet (SPG-01 not landed)."
      exit 0
    fi
    if [[ -f scripts/fetch_deps.py ]]; then
      "$py" scripts/fetch_deps.py --cache "$SPG_CACHE_DIR"
    fi
    if [[ -f scripts/fetch_models.py ]]; then
      "$py" scripts/fetch_models.py --cache "$SPG_CACHE_DIR"
    fi
    ;;

  build)
    if ! has_build; then
      echo "::notice::build: no CMakeLists.txt yet (SPG-01 not landed)."
      exit 0
    fi
    generator_args=()
    cache_dir="$SPG_CACHE_DIR"
    case "$(uname -s)" in
      MINGW*|MSYS*|CYGWIN*)
        # Git Bash on Windows: Ninja here would pick MinGW gcc, which cannot link
        # MSVC-built ONNX Runtime or libobs. Use Visual Studio.
        generator_args=(-G "Visual Studio 17 2022" -A x64)
        # CMake is a native Windows program; hand it C:/... rather than /c/...
        cache_dir="$(cygpath -m "$SPG_CACHE_DIR")" ;;
      *)
        if command -v ninja >/dev/null 2>&1; then generator_args=(-G Ninja); fi ;;
    esac
    # ccache (pinned in deps/manifest.json) keeps its cache under $SPG_CACHE_DIR,
    # which CI restores between runs. CMakeLists.txt decides whether to use it.
    export CCACHE_DIR="${CCACHE_DIR:-$SPG_CACHE_DIR/ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$root}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-500M}"
    # The +-expansion keeps bash 3.2 (macOS /bin/bash) happy under set -u.
    cmake -S . -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
      -DCMAKE_BUILD_TYPE=Release -DSPG_CACHE_DIR="$cache_dir"
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
    if [[ ! -f eval/run_eval.py ]]; then
      echo "THRESHOLDS: NOT RUN (eval driver not implemented yet; SPG-03 and SPG-11)"
      # CI stays green before the eval exists; local acceptance must not pass.
      [[ "$mode" == ci ]] && exit 0
      exit 1
    fi
    "$py" eval/run_eval.py --mode "$mode" --build-dir "$build_dir" \
      --thresholds eval/thresholds.toml --out eval-out
    ;;

  *)
    echo "usage: $0 {setup|build|test|package|eval [--local]}" >&2
    exit 2
    ;;
esac
