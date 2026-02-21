#!/bin/bash
print_help() {
    echo "Usage: $0 [mode]"
    echo
    echo "Modes:"
    echo "  debug      Configure ns-3 in debug mode (default)"
    echo "  release    Configure ns-3 in release mode"
    echo "  optimized  Configure ns-3 in optimized mode"
    echo "  help       Show this help message"
}

# Usage: ./configure.sh [mode]
# Modes: debug, release, optimized
MODE="${1:-debug}"

# Select ns3 configuration options based on mode
case "$MODE" in
    debug)
        CONFIG_FLAGS="--build-profile=debug"
        ;;
    release)
        CONFIG_FLAGS="--build-profile=release"
        ;;
    optimized)
        CONFIG_FLAGS="--build-profile=optimized"
        ;;
    help|-h|--help)
        print_help
        exit 0
        ;;
    *)
        echo "Invalid mode: $MODE"
        print_help
        exit 1
        ;;
esac

# Run the configuration
echo "Configuring ns-3 in '$MODE' mode..."
./ns3 configure --enable-examples --enable-mtp $CONFIG_FLAGS
