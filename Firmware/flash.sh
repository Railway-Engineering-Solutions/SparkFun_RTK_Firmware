#!/usr/bin/env bash
#
# Flash SparkFun RTK Surveyor firmware to a connected ESP32 device.
#
# Usage:
#   ./flash.sh                        # Flash using pre-built binaries, auto-detect port
#   ./flash.sh /dev/cu.usbserial-XXX  # Flash to a specific port
#   ./flash.sh --build                # Build v4.7 first, then flash
#   ./flash.sh --build --port /dev/X  # Build and flash to specific port
#   ./flash.sh --baud 921600          # Use custom baud rate
#
# Requires: esptool (pip3 install esptool)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/RTK_Surveyor/build/esp32.esp32.esp32"

# Defaults
FLASH_BAUD="${FLASH_BAUD:-460800}"
FLASH_PORT=""
DO_BUILD=false

# ── Parse arguments ──────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)
            DO_BUILD=true
            shift
            ;;
        --port)
            FLASH_PORT="$2"
            shift 2
            ;;
        --baud)
            FLASH_BAUD="$2"
            shift 2
            ;;
        --help|-h)
            head -12 "$0" | tail -10
            exit 0
            ;;
        /dev/*)
            FLASH_PORT="$1"
            shift
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

detect_port() {
    local port
    # macOS
    for port in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*; do
        [[ -e "$port" ]] && { echo "$port"; return 0; }
    done
    # Linux
    for port in /dev/ttyUSB* /dev/ttyACM*; do
        [[ -e "$port" ]] && { echo "$port"; return 0; }
    done
    return 1
}

# ── Build (optional) ────────────────────────────────────────────────
if $DO_BUILD; then
    info "Building firmware v4.7..."
    "$SCRIPT_DIR/build.sh" --version 4 7
    echo ""
fi

# ── Verify binaries exist ───────────────────────────────────────────
FIRMWARE_BIN="$BUILD_DIR/RTK_Surveyor.ino.bin"
BOOTLOADER_BIN="$BUILD_DIR/RTK_Surveyor.ino.bootloader.bin"
PARTITIONS_BIN="$BUILD_DIR/RTK_Surveyor.ino.partitions.bin"

for f in "$FIRMWARE_BIN" "$BOOTLOADER_BIN" "$PARTITIONS_BIN"; do
    [[ -f "$f" ]] || fail "Missing binary: $f — run with --build or build first"
done

FIRMWARE_SIZE=$(stat -f%z "$FIRMWARE_BIN" 2>/dev/null || stat -c%s "$FIRMWARE_BIN")
ok "Firmware binary: $(( FIRMWARE_SIZE / 1024 )) KB"

# ── Detect port ─────────────────────────────────────────────────────
if [[ -z "$FLASH_PORT" ]]; then
    info "Auto-detecting serial port..."
    FLASH_PORT=$(detect_port) || fail "No ESP32 serial port found. Specify with: --port /dev/cu.usbserial-XXXX"
fi
ok "Using port: $FLASH_PORT"

# ── Verify esptool ──────────────────────────────────────────────────
if ! python3 -m esptool version &>/dev/null; then
    fail "esptool not found. Install with: pip3 install esptool"
fi

# ── Flash ────────────────────────────────────────────────────────────
echo ""
echo "═══ Flashing RTK Surveyor Firmware ═══"
echo "  Port:       $FLASH_PORT"
echo "  Baud:       $FLASH_BAUD"
echo "  Bootloader: $BOOTLOADER_BIN"
echo "  Partitions: $PARTITIONS_BIN"
echo "  Firmware:   $FIRMWARE_BIN"
echo ""

python3 -m esptool \
    --chip esp32 \
    --port "$FLASH_PORT" \
    --baud "$FLASH_BAUD" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size detect \
    0x1000  "$BOOTLOADER_BIN" \
    0x8000  "$PARTITIONS_BIN" \
    0x10000 "$FIRMWARE_BIN"

echo ""
ok "Flash complete — device is rebooting"
