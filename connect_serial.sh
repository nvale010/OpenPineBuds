#!/bin/bash

# Configuration
RFCOMM_DEV="rfcomm1"  # Using 1 to avoid conflicts with 0
CHANNEL=15  # RFCOMM_CHANNEL_GREEN (data channel)
MAC_ADDRESS="20:22:11:12:34:6F" # Updated from your logs

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}OpenPineBuds Serial Helper${NC}"
echo "--------------------------------"

# Check for root/sudo
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Please run as root (sudo)${NC}"
  exit 1
fi

# 1. Clean up existing binding
echo -n "Cleaning up /dev/$RFCOMM_DEV... "
if rfcomm release $RFCOMM_DEV > /dev/null 2>&1; then
    echo "Done."
else
    echo "Already clean."
fi

# 2. Bind to Channel 15 (data channel)
echo -n "Binding $MAC_ADDRESS to Channel $CHANNEL on /dev/$RFCOMM_DEV... "
rfcomm bind $RFCOMM_DEV $MAC_ADDRESS $CHANNEL
if [ $? -eq 0 ]; then
    echo -e "${GREEN}Success!${NC}"
else
    echo -e "${RED}Failed to bind!${NC}"
    exit 1
fi

# 3. Launch Minicom
echo "Launching minicom..."
echo "Press Ctrl+A, then X to exit."
minicom -D /dev/$RFCOMM_DEV

# 4. Cleanup on exit
echo -e "\n${GREEN}Disconnected.${NC}"
echo "Cleaning up..."
rfcomm release $RFCOMM_DEV
echo "Done."
