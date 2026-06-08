#!/bin/bash
#
# capture_mic.sh — Record raw mic audio from OpenPineBuds over RFCOMM
#
# Usage:
#   sudo ./capture_mic.sh              # saves to timestamped .raw file
#   sudo ./capture_mic.sh my_clip      # saves to my_clip.raw
#
# Output format: signed 16-bit PCM, little-endian, mono, 8000 Hz
# Import in Audacity: File > Import > Raw Data with the above settings.

MAC_ADDRESS="20:22:11:12:34:6F"
DATA_CHANNEL=15    # RFCOMM_CHANNEL_GREEN  (raw data)
LOG_CHANNEL=16     # RFCOMM_CHANNEL_RED    (status messages)
DATA_DEV="rfcomm1"
LOG_DEV="rfcomm2"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# File descriptor used for the single open connection to the data device
DATA_FD=3

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[x]${NC} $*"; }
die()   { error "$*"; cleanup_and_exit 1; }

CLEANUP_DONE=0
cleanup_and_exit() {
    [ "$CLEANUP_DONE" -eq 1 ] && exit "${1:-0}"
    CLEANUP_DONE=1

    echo ""
    info "Stopping recording..."

    # Send 's' via a fresh write-only open — safe after cat is killed
    printf 's' > /dev/$DATA_DEV 2>/dev/null || true
    # Give the earbud time to process the stop and close the codec before
    # the RFCOMM disconnect happens — prevents double-open on next run
    sleep 1.0

    # Kill background jobs cleanly
    if [ -n "$LOG_PID" ]; then
        kill "$LOG_PID" 2>/dev/null
        wait "$LOG_PID" 2>/dev/null
    fi
    if [ -n "$CAT_PID" ]; then
        kill "$CAT_PID" 2>/dev/null
        wait "$CAT_PID" 2>/dev/null
    fi

    info "Releasing RFCOMM bindings..."
    rfcomm release $DATA_DEV 2>/dev/null
    rfcomm release $LOG_DEV  2>/dev/null

    if [ -f "$OUTFILE" ]; then
        BYTES=$(stat -c%s "$OUTFILE" 2>/dev/null || echo 0)
        # Subtract the small text banner the firmware sends before PCM data.
        # The banner is at most ~64 bytes; duration calculation is approximate.
        PCM_BYTES=$(( BYTES > 64 ? BYTES - 64 : 0 ))
        SAMPLES=$(( PCM_BYTES / 2 ))
        DURATION_MS=$(( SAMPLES * 1000 / 8000 ))
        info "Saved ${BYTES} bytes (~${DURATION_MS} ms of audio) to ${OUTFILE}"
        info "Audacity import: Raw Data | Signed 16-bit PCM | Little-endian | Mono | 8000 Hz"
    fi

    exit "${1:-0}"
}

trap 'cleanup_and_exit 0' INT TERM EXIT

# ---------------------------------------------------------------------------
# Root check
# ---------------------------------------------------------------------------
if [ "$EUID" -ne 0 ]; then
    error "Run with sudo: sudo $0 $*"
    CLEANUP_DONE=1   # skip cleanup — nothing was set up yet
    exit 1
fi

# ---------------------------------------------------------------------------
# Output filename
# ---------------------------------------------------------------------------
if [ -n "$1" ]; then
    OUTFILE="${1%.raw}.raw"
else
    OUTFILE="mic_$(date +%Y%m%d_%H%M%S).raw"
fi

echo -e "${GREEN}OpenPineBuds Mic Capture${NC}"
echo "----------------------------------------"
info "Output file : $OUTFILE"
info "Data device : /dev/$DATA_DEV  (channel $DATA_CHANNEL)"
info "Log device  : /dev/$LOG_DEV   (channel $LOG_CHANNEL)"
echo ""

# ---------------------------------------------------------------------------
# Release any stale bindings
# ---------------------------------------------------------------------------
rfcomm release $DATA_DEV 2>/dev/null
rfcomm release $LOG_DEV  2>/dev/null
sleep 0.2

# ---------------------------------------------------------------------------
# Bind channels
# ---------------------------------------------------------------------------
info "Binding data channel..."
rfcomm bind $DATA_DEV $MAC_ADDRESS $DATA_CHANNEL \
    || { error "Failed to bind data channel. Is the earbud powered on and paired?"; CLEANUP_DONE=1; exit 1; }

info "Binding log channel..."
rfcomm bind $LOG_DEV $MAC_ADDRESS $LOG_CHANNEL
if [ $? -ne 0 ]; then
    warn "Log channel bind failed — continuing without log output"
    LOG_AVAILABLE=0
else
    LOG_AVAILABLE=1
fi

# ---------------------------------------------------------------------------
# Put the data device into raw mode so the tty line discipline doesn't
# process or echo any bytes (including the 'r'/'s' commands).
# ---------------------------------------------------------------------------
info "Connecting to /dev/$DATA_DEV..."
stty -F /dev/$DATA_DEV raw -echo 2>/dev/null
if [ $? -ne 0 ]; then
    error "Could not configure /dev/$DATA_DEV — is the earbud connected?"
    CLEANUP_DONE=1; exit 1
fi
info "Connected."

# ---------------------------------------------------------------------------
# Start log reader in background
# ---------------------------------------------------------------------------
LOG_PID=""
if [ "$LOG_AVAILABLE" -eq 1 ]; then
    (
        while IFS= read -r line 2>/dev/null; do
            echo -e "  ${YELLOW}[earbud]${NC} $line"
        done < /dev/$LOG_DEV
    ) &
    LOG_PID=$!
fi

# ---------------------------------------------------------------------------
# Start raw capture in background.
# cat opens the device read-only; our command write opens it write-only.
# With stty raw -echo these are independent and the tty layer won't
# echo or mangle either direction.
# ---------------------------------------------------------------------------
info "Starting raw capture -> $OUTFILE"
cat /dev/$DATA_DEV > "$OUTFILE" &
CAT_PID=$!

# Give cat a moment to open the device and start reading.
sleep 0.5

# ---------------------------------------------------------------------------
# Send record command via a separate write-only open
# ---------------------------------------------------------------------------
info "Sending 'r' command..."
printf 'r' > /dev/$DATA_DEV

echo ""
echo -e "  ${GREEN}Recording.${NC} Press ${YELLOW}Enter${NC} or ${YELLOW}Ctrl+C${NC} to stop."
echo ""

# Read from the controlling terminal, not stdin (which is the device fd)
read -r _ </dev/tty

# cleanup_and_exit fires via trap on EXIT
