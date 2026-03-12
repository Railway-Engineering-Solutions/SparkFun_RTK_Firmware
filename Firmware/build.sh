#!/usr/bin/env bash
#
# Build and optionally flash SparkFun RTK Surveyor firmware.
#
# Usage:
#   ./build.sh                    # Build only
#   ./build.sh --flash            # Build and flash to connected device
#   ./build.sh --flash /dev/XXX   # Build and flash to specific port
#   ./build.sh --setup            # Install toolchain, then build
#   ./build.sh --version 4 5      # Set firmware version to v4.5
#
# Environment variables (override defaults):
#   FIRMWARE_VERSION_MAJOR  - Major version number (default: 99 = dev)
#   FIRMWARE_VERSION_MINOR  - Minor version number (default: 99 = dev)
#   POINTPERFECT_TOKEN      - u-blox PointPerfect token (default: placeholder)
#   DEBUG_LEVEL             - Arduino debug level: none, error, warn, info, debug, verbose (default: error)
#   ENABLE_DEVELOPER        - Developer mode: true/false (default: true)
#   FLASH_BAUD              - Baud rate for flashing (default: 460800)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKETCH_DIR="$SCRIPT_DIR/RTK_Surveyor"
TOOLS_DIR="$SCRIPT_DIR/Tools"
BUILD_DIR="$SKETCH_DIR/build/esp32.esp32.esp32"

# Defaults
FIRMWARE_VERSION_MAJOR="${FIRMWARE_VERSION_MAJOR:-99}"
FIRMWARE_VERSION_MINOR="${FIRMWARE_VERSION_MINOR:-99}"
POINTPERFECT_TOKEN="${POINTPERFECT_TOKEN:-0xAA,0xBB,0xCC,0xDD,0x00,0x11,0x22,0x33,0x0A,0x0B,0x0C,0x0D,0x00,0x01,0x02,0x03}"
DEBUG_LEVEL="${DEBUG_LEVEL:-error}"
ENABLE_DEVELOPER="${ENABLE_DEVELOPER:-true}"
FLASH_BAUD="${FLASH_BAUD:-460800}"
ESP32_CORE_VERSION="2.0.2"

DO_SETUP=false
DO_FLASH=false
FLASH_PORT=""

# ── Parse arguments ──────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --setup)
            DO_SETUP=true
            shift
            ;;
        --flash)
            DO_FLASH=true
            shift
            # Optional port argument
            if [[ $# -gt 0 && ! "$1" =~ ^-- ]]; then
                FLASH_PORT="$1"
                shift
            fi
            ;;
        --version)
            FIRMWARE_VERSION_MAJOR="$2"
            FIRMWARE_VERSION_MINOR="$3"
            shift 3
            ;;
        --help|-h)
            head -15 "$0" | tail -13
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────────────
info()  { echo "▸ $*"; }
ok()    { echo "✓ $*"; }
fail()  { echo "✗ $*" >&2; exit 1; }

check_tool() {
    command -v "$1" &>/dev/null || return 1
}

# Auto-detect serial port if flashing without explicit port
detect_port() {
    local port
    # macOS: look for USB serial devices
    for port in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*; do
        if [[ -e "$port" ]]; then
            echo "$port"
            return 0
        fi
    done
    # Linux: look for common ESP32 USB serial
    for port in /dev/ttyUSB* /dev/ttyACM*; do
        if [[ -e "$port" ]]; then
            echo "$port"
            return 0
        fi
    done
    return 1
}

# ── Setup: install toolchain ─────────────────────────────────────────
setup_toolchain() {
    info "Installing build toolchain..."

    # arduino-cli
    if ! check_tool arduino-cli; then
        info "Installing arduino-cli..."
        if check_tool brew; then
            brew install arduino-cli
        else
            curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
        fi
    fi
    ok "arduino-cli $(arduino-cli version | head -1)"

    # esptool (for flashing)
    if ! python3 -m esptool version &>/dev/null; then
        info "Installing esptool..."
        pip3 install esptool
    fi
    ok "esptool $(python3 -m esptool version 2>&1 | head -1)"

    # python symlink (ESP32 toolchain needs 'python' on PATH)
    if ! check_tool python; then
        info "Creating python -> python3 symlink..."
        if check_tool brew; then
            ln -sf "$(brew --prefix)/bin/python3" "$(brew --prefix)/bin/python"
        else
            sudo ln -sf "$(which python3)" /usr/local/bin/python
        fi
    fi

    # Arduino ESP32 core
    info "Configuring Arduino ESP32 core..."
    arduino-cli config init --overwrite \
        --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json \
        >/dev/null 2>&1
    arduino-cli core update-index >/dev/null 2>&1
    arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}" >/dev/null 2>&1
    ok "ESP32 core ${ESP32_CORE_VERSION}"

    # Arduino libraries
    info "Installing Arduino libraries..."
    arduino-cli lib install \
        ArduinoJson@6.19.4 \
        ESP32Time@2.0.0 \
        ESP32_BleSerial@1.0.5 \
        "ESP32-OTA-Pull"@1.0.0 \
        Ethernet@2.0.2 \
        JC_Button@2.1.2 \
        PubSubClient@2.8.0 \
        "SdFat"@2.1.1 \
        "SparkFun LIS2DH12 Arduino Library"@1.0.3 \
        "SparkFun MAX1704x Fuel Gauge Arduino Library"@1.0.4 \
        "SparkFun u-blox GNSS v3"@3.0.14 \
        "SparkFun_WebServer_ESP32_W5500"@1.5.5 \
        "SparkFun Qwiic OLED Arduino Library"@1.0.13 \
        SSLClientESP32@2.0.0 \
        >/dev/null 2>&1

    arduino-cli config set library.enable_unsafe_install true >/dev/null 2>&1
    arduino-cli lib install --git-url https://github.com/me-no-dev/ESPAsyncWebServer.git >/dev/null 2>&1
    arduino-cli lib install --git-url https://github.com/me-no-dev/AsyncTCP.git >/dev/null 2>&1
    ok "All libraries installed"

    ok "Toolchain setup complete"
}

# ── Pre-build: patches and generated files ───────────────────────────
prebuild() {
    # Find Arduino15 path (macOS vs Linux)
    local arduino15
    if [[ -d "$HOME/Library/Arduino15" ]]; then
        arduino15="$HOME/Library/Arduino15"
    elif [[ -d "$HOME/.arduino15" ]]; then
        arduino15="$HOME/.arduino15"
    else
        fail "Cannot find Arduino15 directory"
    fi

    local core_path="$arduino15/packages/esp32/hardware/esp32/${ESP32_CORE_VERSION}"
    [[ -d "$core_path" ]] || fail "ESP32 core not found at $core_path. Run: $0 --setup"

    # Patch Server.h
    info "Patching Server.h..."
    cp "$SKETCH_DIR/Patch/Server.h" "$core_path/cores/esp32/Server.h"

    # Copy custom partition table
    info "Installing partition table..."
    cp "$SCRIPT_DIR/app3M_fat9M_16MB.csv" "$core_path/tools/partitions/app3M_fat9M_16MB.csv"

    # Regenerate form.h from web UI sources
    info "Generating form.h from web UI..."
    (cd "$TOOLS_DIR" && python3 index_html_zipper.py ../RTK_Surveyor/AP-Config/index.html ../RTK_Surveyor/form.h)
    (cd "$TOOLS_DIR" && python3 main_js_zipper.py ../RTK_Surveyor/AP-Config/src/main.js ../RTK_Surveyor/form.h)
}

# ── Build ────────────────────────────────────────────────────────────
build() {
    info "Compiling RTK Surveyor firmware v${FIRMWARE_VERSION_MAJOR}.${FIRMWARE_VERSION_MINOR}..."
    info "  Debug level: ${DEBUG_LEVEL}"
    info "  Developer mode: ${ENABLE_DEVELOPER}"

    arduino-cli compile \
        --fqbn "esp32:esp32:esp32":DebugLevel="${DEBUG_LEVEL}" \
        "$SKETCH_DIR/RTK_Surveyor.ino" \
        --build-property build.partitions=app3M_fat9M_16MB \
        --build-property upload.maximum_size=3145728 \
        --build-property "compiler.cpp.extra_flags=\"-DPOINTPERFECT_TOKEN=${POINTPERFECT_TOKEN}\" \"-DFIRMWARE_VERSION_MAJOR=${FIRMWARE_VERSION_MAJOR}\" \"-DFIRMWARE_VERSION_MINOR=${FIRMWARE_VERSION_MINOR}\" \"-DENABLE_DEVELOPER=${ENABLE_DEVELOPER}\"" \
        --export-binaries \
        2>&1 | grep -E "^Sketch uses|^Global variables|error:" || true

    [[ -f "$BUILD_DIR/RTK_Surveyor.ino.bin" ]] || fail "Build failed — no output binary"

    local size
    size=$(stat -f%z "$BUILD_DIR/RTK_Surveyor.ino.bin" 2>/dev/null || stat -c%s "$BUILD_DIR/RTK_Surveyor.ino.bin")
    ok "Build complete: RTK_Surveyor.ino.bin ($(( size / 1024 )) KB)"
    echo "  Output: $BUILD_DIR/RTK_Surveyor.ino.bin"
}

# ── Flash ────────────────────────────────────────────────────────────
flash() {
    local port="$FLASH_PORT"
    if [[ -z "$port" ]]; then
        info "Auto-detecting serial port..."
        port=$(detect_port) || fail "No ESP32 serial port found. Specify with: --flash /dev/cu.usbserial-XXXX"
    fi

    info "Flashing to $port at ${FLASH_BAUD} baud..."
    python3 -m esptool \
        --chip esp32 \
        --port "$port" \
        --baud "$FLASH_BAUD" \
        write_flash \
        0x1000  "$BUILD_DIR/RTK_Surveyor.ino.bootloader.bin" \
        0x8000  "$BUILD_DIR/RTK_Surveyor.ino.partitions.bin" \
        0x10000 "$BUILD_DIR/RTK_Surveyor.ino.bin"

    ok "Flash complete — device is rebooting"
}

# ── Main ─────────────────────────────────────────────────────────────
echo "═══ SparkFun RTK Surveyor Firmware Build ═══"
echo ""

if $DO_SETUP; then
    setup_toolchain
    echo ""
fi

# Verify tools are available
check_tool arduino-cli || fail "arduino-cli not found. Run: $0 --setup"
check_tool python3     || fail "python3 not found"

prebuild
build

if $DO_FLASH; then
    echo ""
    flash
fi
