#!/bin/bash

print_help() {
    echo "Usage: $0 [max_attempts]"
    echo
    echo "Example:"
    echo "  ./build.sh 5       # Build with maximum 5 attempts in case of resource crash."
}

# Check for help
if [[ "$1" == "help" || "$1" == "-h" || "$1" == "--help" ]]; then
    print_help
    exit 0
fi

# Maximum number of attempts
max_attempts="${1:-5}"
attempt=1
# Loop until the command succeeds or we reach the maximum number of attempts
while true; do
    # echo "Attempt $attempt of $max_attempts"
    ./ns3 build

    # Check the exit status of the command
    if [ $? -eq 0 ]; then
        echo "Build completed successfully."
        break
    else
        echo "Build failed. Attempting again..."
        ((attempt++))
        # Check if we have reached the maximum number of attempts
        if [ $attempt -gt $max_attempts ]; then
            echo "Maximum number of attempts reached. Exiting."
            exit 1
        fi
    fi

    # Optional: sleep between attempts
    sleep .5
done
