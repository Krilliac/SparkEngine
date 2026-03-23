#!/bin/bash

# SparkEngine Documentation Auto-Update Script
# Monitors header files for changes and automatically regenerates documentation

set -e

# Script configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
GENERATE_SCRIPT="$SCRIPT_DIR/generate-docs.sh"
WATCH_DIRS=("$PROJECT_ROOT/SparkEngine/Source" "$PROJECT_ROOT/SparkEditor/Source" "$PROJECT_ROOT/SparkConsole/src" "$PROJECT_ROOT/SparkShaderCompiler/src" "$PROJECT_ROOT/GameModules" "$PROJECT_ROOT/SparkSDK")
CHECKSUM_FILE="$SCRIPT_DIR/.doc_checksums"
LOCK_FILE="$SCRIPT_DIR/.update_lock"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[AUTO-UPDATE]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[AUTO-UPDATE]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[AUTO-UPDATE]${NC} $1"
}

log_error() {
    echo -e "${RED}[AUTO-UPDATE]${NC} $1"
}

# Function to check if required tools are available
check_dependencies() {
    local missing_deps=()
    
    if ! command -v find &> /dev/null; then
        missing_deps+=("find")
    fi
    
    if ! command -v md5sum &> /dev/null; then
        missing_deps+=("md5sum")
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "Missing dependencies: ${missing_deps[*]}"
        exit 1
    fi
}

# Function to calculate checksums of all header files
calculate_checksums() {
    local checksums=""
    
    for watch_dir in "${WATCH_DIRS[@]}"; do
        if [ -d "$watch_dir" ]; then
            # Find all header files and calculate their checksums
            local dir_checksums=$(find "$watch_dir" -type f \( -name "*.h" -o -name "*.hpp" \) -exec md5sum {} \; 2>/dev/null | sort)
            checksums="$checksums$dir_checksums"
        fi
    done
    
    echo "$checksums"
}

# Function to check if files have changed
files_changed() {
    local current_checksums=$(calculate_checksums)
    
    if [ ! -f "$CHECKSUM_FILE" ]; then
        # First run, create checksum file
        echo "$current_checksums" > "$CHECKSUM_FILE"
        return 0  # True, files "changed" (initial run)
    fi
    
    local previous_checksums=$(cat "$CHECKSUM_FILE")
    
    if [ "$current_checksums" != "$previous_checksums" ]; then
        echo "$current_checksums" > "$CHECKSUM_FILE"
        return 0  # True, files changed
    fi
    
    return 1  # False, no changes
}

# Function to acquire lock
acquire_lock() {
    if [ -f "$LOCK_FILE" ]; then
        local lock_pid=$(cat "$LOCK_FILE")
        if kill -0 "$lock_pid" 2>/dev/null; then
            log_warning "Another documentation update is in progress (PID: $lock_pid)"
            return 1
        else
            log_warning "Removing stale lock file"
            rm -f "$LOCK_FILE"
        fi
    fi
    
    echo $$ > "$LOCK_FILE"
    return 0
}

# Function to release lock
release_lock() {
    rm -f "$LOCK_FILE"
}

# Function to regenerate documentation
regenerate_docs() {
    log_info "Changes detected in header files, regenerating documentation..."
    
    if ! acquire_lock; then
        log_error "Could not acquire lock for documentation update"
        return 1
    fi
    
    # Ensure we release the lock on exit
    trap release_lock EXIT
    
    # Make the generate script executable
    chmod +x "$GENERATE_SCRIPT"
    
    # Run the documentation generation
    if "$GENERATE_SCRIPT"; then
        log_success "Documentation updated successfully"
        log_info "Last update: $(date)"
        return 0
    else
        log_error "Documentation generation failed"
        return 1
    fi
}

# Function to run continuous monitoring
monitor_continuous() {
    log_info "Starting continuous monitoring of header files..."
    log_info "Monitoring directories:"
    for dir in "${WATCH_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            log_info "  - $dir"
        else
            log_warning "  - $dir (not found)"
        fi
    done
    log_info "Press Ctrl+C to stop monitoring"
    
    # Initial check and generation
    if files_changed; then
        regenerate_docs
    else
        log_info "No changes detected, documentation is up to date"
    fi
    
    # Continuous monitoring loop
    while true; do
        sleep 30  # Check every 30 seconds
        
        if files_changed; then
            regenerate_docs
        fi
    done
}

# Function to run single check
check_once() {
    log_info "Checking for changes in header files..."
    
    if files_changed; then
        regenerate_docs
        return $?
    else
        log_info "No changes detected, documentation is up to date"
        return 0
    fi
}

# Function to force regeneration
force_regenerate() {
    log_info "Forcing documentation regeneration..."
    
    # Update checksums to current state
    calculate_checksums > "$CHECKSUM_FILE"
    
    # Regenerate documentation
    regenerate_docs
}

# Function to show status
show_status() {
    echo "======================================================"
    log_info "SparkEngine Documentation Auto-Update Status"
    echo "======================================================"
    
    log_info "Project root: $PROJECT_ROOT"
    log_info "Documentation: $SCRIPT_DIR"
    
    echo
    log_info "Monitored directories:"
    for dir in "${WATCH_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            local file_count=$(find "$dir" -type f \( -name "*.h" -o -name "*.hpp" \) | wc -l)
            log_info "  ✓ $dir ($file_count header files)"
        else
            log_warning "  ✗ $dir (not found)"
        fi
    done
    
    echo
    if [ -f "$CHECKSUM_FILE" ]; then
        log_info "Last checksum update: $(stat -c %y "$CHECKSUM_FILE" 2>/dev/null || stat -f %Sm "$CHECKSUM_FILE" 2>/dev/null || echo "unknown")"
    else
        log_info "No previous checksums found (first run)"
    fi
    
    if [ -f "$LOCK_FILE" ]; then
        local lock_pid=$(cat "$LOCK_FILE")
        if kill -0 "$lock_pid" 2>/dev/null; then
            log_warning "Update in progress (PID: $lock_pid)"
        else
            log_warning "Stale lock file found"
        fi
    else
        log_info "No active update process"
    fi
    
    echo "======================================================"
}

# Function to display help
show_help() {
    echo "SparkEngine Documentation Auto-Update Script"
    echo
    echo "Usage: $0 [COMMAND]"
    echo
    echo "Commands:"
    echo "  monitor     Start continuous monitoring (default)"
    echo "  check       Check once for changes and update if needed"
    echo "  force       Force regenerate documentation"
    echo "  status      Show current status"
    echo "  help        Show this help message"
    echo
    echo "Examples:"
    echo "  $0                    # Start continuous monitoring"
    echo "  $0 check             # Check once and exit"
    echo "  $0 force             # Force regeneration"
    echo "  $0 status            # Show status"
    echo
    echo "The script monitors header files in:"
    for dir in "${WATCH_DIRS[@]}"; do
        echo "  - $dir"
    done
}

# Main execution
main() {
    local command="${1:-monitor}"
    
    check_dependencies
    
    case "$command" in
        "monitor")
            monitor_continuous
            ;;
        "check")
            check_once
            ;;
        "force")
            force_regenerate
            ;;
        "status")
            show_status
            ;;
        "help"|"-h"|"--help")
            show_help
            ;;
        *)
            log_error "Unknown command: $command"
            echo
            show_help
            exit 1
            ;;
    esac
}

# Handle Ctrl+C gracefully
trap 'log_info "Monitoring stopped by user"; release_lock; exit 0' INT TERM

# Run main function if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi