#!/usr/bin/env bash
# Development workflow for OpenHU in the container.
#
#   ./dev.sh run [args]   build incrementally (aasdk + OpenHU) and start OpenHU in a window
#   ./dev.sh build        build only
#   ./dev.sh test         build, unit tests (ctest), qmllint, translations and the shell headless (like the CI)
#   ./dev.sh package      build and put the .deb into output/
#   ./dev.sh shell        shell in the development environment (sources mounted)
#   ./dev.sh reconnect    reconnect the phone by USB reset (running OpenHU)
#   ./dev.sh image        rebuild the image openhu:dev with current Debian packages (automatic after Dockerfile changes)
#   ./dev.sh clean        delete the build volumes and ccache (the next build starts from scratch)
#
# Start without building: ./dev.sh run --no-build, resolution: OPENHU_RESOLUTION=800x480 ./dev.sh run
# Protocol capture of the Android Auto connection (aasdk AASDK_CAPTURE, average-web-dev/aasdk#14):
#   OPENHU_CAPTURE=1 ./dev.sh run    writes data/capture-<time>.jsonl (with OPENHU_CAPTURE=media also audio/video
#                                    data); evaluate with ../aasdk/tools/capture_summary.py. Contains personal data.
#   AASDK_VERSION=5.1 ./dev.sh run   asks the phone for another protocol version (aasdk#26)
set -euo pipefail

DOCKER_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$DOCKER_DIR")"
# aasdk source (default: aasdk-ref or aasdk next to this repository; in a git worktree next to the main checkout).
# Docker would mount a missing one as an empty folder (owned by root), so it has to contain CMakeLists.txt.
if [ -z "${AASDK_DIR:-}" ]; then
  main_repo="$REPO"
  common_dir="$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
  [ -n "$common_dir" ] && main_repo="$(dirname "$common_dir")"
  for candidate in "$(dirname "$REPO")/aasdk-ref" "$(dirname "$REPO")/aasdk" \
                   "$(dirname "$main_repo")/aasdk-ref" "$(dirname "$main_repo")/aasdk"; do
    if [ -f "$candidate/CMakeLists.txt" ]; then
      AASDK_DIR="$candidate"
      break
    fi
  done
fi
if [ ! -f "${AASDK_DIR:-}/CMakeLists.txt" ]; then
  echo "aasdk source not found (looked next to this repository and next to the main checkout)." >&2
  echo "Set AASDK_DIR=/path/to/aasdk." >&2
  exit 1
fi
IMAGE="openhu:dev"

# Builds, installed aasdk and ccache live in Docker volumes: incremental builds stay fast
# and no root-owned files end up in the source tree.
VOLUMES=(openhu-build openhu-aasdk-build openhu-aasdk openhu-ccache)
BUILD_MOUNTS=(
  -v openhu-build:/build/openhu
  -v openhu-aasdk-build:/build/aasdk
  -v openhu-aasdk:/opt/aasdk
  -v openhu-ccache:/ccache
)

# Rebuilds the image when the Dockerfile or the entrypoint changed (otherwise a no-op),
# extra arguments force a build with them (cmd image)
ensure_image() {
  local hash
  hash="$(cat "$REPO/Dockerfile" "$DOCKER_DIR/entrypoint.sh" | sha256sum | cut -c1-16)"
  if [[ $# -gt 0 || "$(docker image inspect -f '{{index .Config.Labels "openhu.hash"}}' "$IMAGE" 2>/dev/null)" != "$hash" ]]; then
    docker build --target dev --label openhu.hash="$hash" -t "$IMAGE" "$@" "$REPO"
  fi
}

dev_container() {
  local tty=()
  [ -t 0 ] && [ -t 1 ] && tty=(-it)
  docker run --rm "${tty[@]}" \
    -v "$REPO:/src" \
    -v "$AASDK_DIR:/aasdk" \
    "${BUILD_MOUNTS[@]}" \
    -w /src \
    "$IMAGE" "$@"
}

cmd_build() {
  ensure_image
  dev_container bash -euo pipefail -c '
    echo "=== aasdk ==="
    if [ -f /build/aasdk/build.ninja ]; then
      cmake --build /build/aasdk
    else
      cd /aasdk && BUILD_DIR=/build/aasdk BUILD_TESTS=OFF bash ./build.sh release --skip-protobuf --skip-absl
    fi
    cmake --install /build/aasdk --prefix /opt/aasdk >/dev/null

    echo "=== openhu ==="
    # configure only once (Ninja reruns CMake itself), a build directory from another generator
    # or from the Qt 5 times is discarded
    if [ ! -f /build/openhu/build.ninja ] || grep -q "^Qt5_DIR" /build/openhu/CMakeCache.txt 2>/dev/null; then
      find /build/openhu -mindepth 1 -delete
      cmake -S /src -B /build/openhu -DCMAKE_BUILD_TYPE=Release \
            -DCPACK_DEBIAN_PACKAGE_RELEASE="$(bash /src/scripts/distro_release.sh)" \
            -DCPACK_PROJECT_CONFIG_FILE=/src/cmake_modules/CPackProjectConfig.cmake >/dev/null
    fi
    cmake --build /build/openhu
  '
}

cmd_test() {
  cmd_build
  dev_container bash -euo pipefail -c '
    echo "=== qmllint ==="
    bash tests/qmllint.sh
    echo "=== translations ==="
    bash tests/translations.sh
    echo "=== ctest ==="
    ctest --test-dir /build/openhu --output-on-failure
    echo "=== shell ==="
    bash tests/shell-smoke.sh /build/openhu/bin/openhu
  '
}

cmd_package() {
  cmd_build
  mkdir -p "$DOCKER_DIR/output"
  rm -f "$DOCKER_DIR"/output/*.deb
  BUILD_MOUNTS+=(-v "$DOCKER_DIR/output:/output")
  dev_container bash -euo pipefail -c "
    cd /build/openhu
    rm -f ./*.deb
    cpack -G DEB >/dev/null
    cp -v ./*.deb /output/
    chown $(id -u):$(id -g) /output/*.deb
  "
}

cmd_run() {
  if [[ "${1:-}" == "--no-build" ]]; then
    shift
    ensure_image
  else
    cmd_build
  fi
  cd "$DOCKER_DIR"

  # root in the container may use the X server (XWayland)
  xhost +SI:localuser:root >/dev/null

  local pulse_sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native"
  local host_display="${DISPLAY:-:0}"
  local host_xsock="/tmp/.X11-unix/X${host_display#:}"

  # Plugins, store sources, cache and logs survive restarts.
  # The container writes as root, afterwards the files belong to the own user again.
  mkdir -p config data
  trap 'docker run --rm -v "$DOCKER_DIR/data:/data" "$IMAGE" chown -R "$(id -u):$(id -g)" /data' EXIT

  # Local plugin repository (store source "file /opt/openhu-plugins") for plugin development,
  # default: next to this repository
  local plugin_repo_dir="${PLUGIN_REPO_DIR:-$(dirname "$REPO")/openhu-plugins}"
  local extra=()
  [ -t 0 ] && [ -t 1 ] && extra+=(-it)
  [ -d "$plugin_repo_dir" ] && extra+=(-v "$plugin_repo_dir:/opt/openhu-plugins:ro")

  # Protocol capture into data/ (aasdk reads AASDK_CAPTURE)
  if [ -n "${OPENHU_CAPTURE:-}" ]; then
    local capture
    capture="capture-$(date +%Y%m%d-%H%M%S).jsonl"
    extra+=(-e "AASDK_CAPTURE=/root/.openhu/$capture")
    [ "$OPENHU_CAPTURE" = media ] && extra+=(-e AASDK_CAPTURE_MEDIA=1)
    echo "Protocol capture: $DOCKER_DIR/data/$capture"
  fi
  # Protocol version aasdk asks the phone for (aasdk AASDK_VERSION, e.g. 5.1)
  [ -n "${AASDK_VERSION:-}" ] && extra+=(-e "AASDK_VERSION=$AASDK_VERSION")

  # Pass cameras through (reverse camera plugin), if present
  local dev
  for dev in /dev/video*; do
    [ -e "$dev" ] && extra+=(--device "$dev")
  done

  # --network host: needed for udev hotplug events (phone switches to AOA mode).
  #                 The OpenHU API (port 5520) and CAN interfaces (vcan0/can0) are reachable directly as well.
  # Only the host X socket is mounted, so Xephyr's own socket stays inside the container.
  # /run/dbus: BlueZ and NetworkManager of the host (wireless Android Auto).
  # OpenHU and aasdk come read-only from the build volumes.
  docker run --rm --privileged \
    --name openhu \
    --network host \
    -e DISPLAY="$host_display" \
    -e OPENHU_RESOLUTION="${OPENHU_RESOLUTION:-1280x720}" \
    -e QT_QPA_PLATFORM=xcb \
    -v /etc/localtime:/etc/localtime:ro \
    -v "$host_xsock:$host_xsock" \
    -e PULSE_SERVER=unix:/run/pulse/native \
    -v "$pulse_sock:/run/pulse/native" \
    -v /dev/bus/usb:/dev/bus/usb \
    -v /run/udev:/run/udev:ro \
    -v /run/dbus/system_bus_socket:/run/dbus/system_bus_socket \
    --device /dev/snd \
    --device /dev/dri \
    "${extra[@]}" \
    -v openhu-build:/build/openhu:ro \
    -v openhu-aasdk:/opt/aasdk:ro \
    -v "$DOCKER_DIR/config:/config" \
    -v "$DOCKER_DIR/data:/root/.openhu" \
    "$IMAGE" openhu-entrypoint "$@"
}

cmd_shell() {
  ensure_image
  dev_container bash
}

cmd_reconnect() {
  docker exec openhu python3 -c '
import asyncio, json, websockets
async def main():
    async with websockets.connect("ws://127.0.0.1:5520") as ws:
        await ws.send(json.dumps({"type": "request", "id": 1, "method": "projection.reconnect"}))
        print(json.loads(await ws.recv()).get("result"))
asyncio.run(main())'
}

cmd_clean() {
  docker volume rm -f "${VOLUMES[@]}" >/dev/null
  echo "Volumes removed: ${VOLUMES[*]}"
}

case "${1:-}" in
  run) shift; cmd_run "$@" ;;
  build) cmd_build ;;
  test) cmd_test ;;
  package) cmd_package ;;
  shell) cmd_shell ;;
  reconnect) cmd_reconnect ;;
  image) ensure_image --pull --no-cache ;;
  clean) cmd_clean ;;
  *) sed -n '2,13p' "$0"; exit 1 ;;
esac
