#!/bin/bash

LOG_DIR="flsim_records"
KEEP_FILE=".gitkeep"

if [ ! -d "$LOG_DIR" ]; then
  echo "Directory $LOG_DIR does not exist."
  exit 1
fi

echo "Cleaning $LOG_DIR (preserving $KEEP_FILE)..."

find "$LOG_DIR" -mindepth 1 ! -name "$KEEP_FILE" -exec rm -rf {} +

echo "Done."
