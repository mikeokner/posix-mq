#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)

DEFAULT_VERSIONS=(0.10 0.12 4 6 8 10 12 14 16 18)
VERSIONS=()
TEST_SCRIPT="test.js"
IMAGE="docker.io/library/ubuntu:22.04"
PLATFORM=""
TIMEOUT_SECONDS=30

usage() {
    cat <<'EOF'
Usage: scripts/test-container-matrix.sh [options]

Run the posix-mq build and a Node.js test script across a version matrix in
an Ubuntu container. Docker is preferred; Podman is used as a fallback.

Options:
  -n, --node VERSION       Test one Node version; may be repeated
  -v, --versions LIST      Test comma- or space-separated Node versions
  -s, --script PATH        JavaScript test path relative to the repository
                           (default: test.js)
      --image IMAGE        Ubuntu image (default: ubuntu:22.04)
      --platform PLATFORM  Container platform, e.g. linux/amd64
      --timeout SECONDS    Per-test timeout after build (default: 30)
  -h, --help               Show this help

Without --node or --versions, the Node matrix matches build-test.yml:
  0.10 0.12 4 6 8 10 12 14 16 18

Examples:
  scripts/test-container-matrix.sh
  scripts/test-container-matrix.sh --node 18
  scripts/test-container-matrix.sh --versions 16,18 --script repro.js
  scripts/test-container-matrix.sh --node 0.10 --platform linux/amd64
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

append_versions() {
    local value=$1
    value=${value//,/ }
    # Intentional word splitting accepts both comma- and space-separated lists.
    # shellcheck disable=SC2206
    local parsed=($value)
    VERSIONS+=("${parsed[@]}")
}

while (($# > 0)); do
    case "$1" in
        -n|--node)
            (($# >= 2)) || die "$1 requires a version"
            VERSIONS+=("$2")
            shift 2
            ;;
        -v|--versions)
            (($# >= 2)) || die "$1 requires a version list"
            append_versions "$2"
            shift 2
            ;;
        -s|--script)
            (($# >= 2)) || die "$1 requires a path"
            TEST_SCRIPT=$2
            shift 2
            ;;
        --image)
            (($# >= 2)) || die "$1 requires an image"
            IMAGE=$2
            shift 2
            ;;
        --platform)
            (($# >= 2)) || die "$1 requires a platform"
            PLATFORM=$2
            shift 2
            ;;
        --timeout)
            (($# >= 2)) || die "$1 requires a number of seconds"
            TIMEOUT_SECONDS=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

((${#VERSIONS[@]} > 0)) || VERSIONS=("${DEFAULT_VERSIONS[@]}")
[[ $TEST_SCRIPT != /* ]] || die "--script must be relative to the repository"
case "/$TEST_SCRIPT/" in
    */../*) die "--script must not leave the repository" ;;
esac
[[ -f $REPO_ROOT/$TEST_SCRIPT ]] || die "test script not found: $TEST_SCRIPT"
[[ $TIMEOUT_SECONDS =~ ^[1-9][0-9]*$ ]] || die "--timeout must be a positive integer"
CONTAINER_RUNTIME=""
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    CONTAINER_RUNTIME=docker
elif command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
    CONTAINER_RUNTIME=podman
fi

if [[ -z $CONTAINER_RUNTIME ]]; then
    die "no usable container runtime found; start Docker or run: podman machine start"
fi

VERSION_LIST=$(IFS=,; printf '%s' "${VERSIONS[*]}")
CONTAINER_ARGS=(run --rm -i --mount "type=bind,src=$REPO_ROOT,dst=/src,readonly" -w /tmp)
[[ -z $PLATFORM ]] || CONTAINER_ARGS+=(--platform "$PLATFORM")

printf 'Using container runtime: %s\n' "$CONTAINER_RUNTIME"

"$CONTAINER_RUNTIME" "${CONTAINER_ARGS[@]}" \
    -e "NODE_VERSIONS=$VERSION_LIST" \
    -e "TEST_SCRIPT=$TEST_SCRIPT" \
    -e "TIMEOUT_SECONDS=$TIMEOUT_SECONDS" \
    "$IMAGE" bash -s <<'CONTAINER_SCRIPT'
set -u

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential ca-certificates curl python3 xz-utils >/dev/null

export NVM_DIR=/opt/nvm
mkdir -p "$NVM_DIR"
curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.3/install.sh | bash >/dev/null
# shellcheck disable=SC1091
. "$NVM_DIR/nvm.sh"

IFS=',' read -r -a versions <<< "$NODE_VERSIONS"
passed=()
failed=()

for version in "${versions[@]}"; do
    printf '\n===== Node %s =====\n' "$version"
    workdir="/tmp/posix-mq-${version//[^a-zA-Z0-9._-]/_}"
    rm -rf "$workdir"
    mkdir -p "$workdir"
    cp -a /src/. "$workdir/"
    rm -rf "$workdir/node_modules" "$workdir/build"

    if ! nvm install "$version"; then
        failed+=("$version (install)")
        continue
    fi
    nvm use "$version" >/dev/null

    if (
        cd "$workdir" &&
        npm install &&
        timeout "$TIMEOUT_SECONDS" node "$TEST_SCRIPT"
    ); then
        passed+=("$version")
    else
        failed+=("$version")
    fi
done

printf '\n===== Summary =====\n'
printf 'Passed:'
((${#passed[@]} == 0)) || printf ' %s' "${passed[@]}"
printf '\nFailed:'
((${#failed[@]} == 0)) || printf ' %s' "${failed[@]}"
printf '\n'

((${#failed[@]} == 0))
CONTAINER_SCRIPT
