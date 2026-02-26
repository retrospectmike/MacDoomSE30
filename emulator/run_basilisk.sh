#!/bin/bash
# Launch Basilisk II with the doom_se30 project configuration

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFS="$SCRIPT_DIR/basilisk/doom_se30.prefs"
BASAPP="/Applications/Basilisk2/BasiliskII.app/Contents/MacOS/BasiliskII"

# Ensure shared folder exists
mkdir -p "$SCRIPT_DIR/shared"

# Clear previous serial debug output
> "$SCRIPT_DIR/serial_debug.txt"

echo "Launching Basilisk II with doom_se30 config..."
echo "Shared folder: $SCRIPT_DIR/shared"
echo "Serial debug:  $SCRIPT_DIR/serial_debug.txt"
echo ""
echo "To monitor debug output in real time, run in another terminal:"
echo "  tail -f $SCRIPT_DIR/serial_debug.txt"
echo ""

"$BASAPP" --config "$PREFS"
