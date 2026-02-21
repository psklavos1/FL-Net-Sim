#!/bin/bash

# clean.sh - Clean ns3/ns-3 build artifacts

print_help() {
    echo "Usage: $0 [Mode]"
    echo
    echo "Modes:"
    echo "  (no argument)   Run ./ns3 clean (remove compiled binaries and object files)"
    echo "  deep            Run ./ns3 distclean (full cleanup: binaries + config)"
    echo "  help            Show this help message"
    echo
    echo "Examples:"
    echo "  ./clean.sh        # clean only"
    echo "  ./clean.sh deep   # full distclean"
    echo "  ./clean.sh help   # show help"
}

# Parse argument
case "$1" in
    "" )
        echo "Running: ./ns3 clean"
        ./ns3 clean
        ;;
    deep )
        echo "Running: ./ns3 distclean"
        ./ns3 distclean
        ;;
    help | -h | --help )
        print_help
        ;;
    * )
        echo "Invalid option: $1"
        print_help
        exit 1
        ;;
esac
