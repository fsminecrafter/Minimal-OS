#!/bin/bash

# Minimal OS Module Configuration Script
# Tree-based menu to enable/disable kernel modules (scalable for many modules)

CONFIG_FILE="config.mk"

# Color codes for better readability
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color

# Check if config.mk exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo -e "${RED}Error: config.mk not found!${NC}"
    echo "Please run this script from the Minimal OS root directory."
    exit 1
fi

# Function to read current config value
read_config() {
    local key="$1"
    local value=$(grep "^${key}" "$CONFIG_FILE" | grep -o '[01]$')
    echo "$value"
}

# Function to update config value
update_config() {
    local key="$1"
    local new_value="$2"
    sed -i "s/^${key} := [01]/${key} := ${new_value}/" "$CONFIG_FILE"
}

# Function to print status
print_status() {
    local module="$1"
    local status="$2"
    if [ "$status" -eq 1 ]; then
        echo -e "  ${GREEN}[✓]${NC} $module"
    else
        echo -e "  ${RED}[✗]${NC} $module"
    fi
}

# Function to print status with index
print_status_indexed() {
    local index="$1"
    local module="$2"
    local status="$3"
    if [ "$status" -eq 1 ]; then
        echo -e "${YELLOW}  $index)${NC} ${GREEN}[✓]${NC} $module"
    else
        echo -e "${YELLOW}  $index)${NC} ${RED}[✗]${NC} $module"
    fi
}

# ========================================
# CATEGORY MENUS
# ========================================

audio_menu() {
    while true; do
        clear
        echo -e "${BLUE}=====================================${NC}"
        echo -e "${BLUE}   Audio Subsystem Configuration${NC}"
        echo -e "${BLUE}=====================================${NC}"
        echo ""
        
        AUDIO=$(read_config "ENABLE_AUDIO")
        
        echo -e "${MAGENTA}Audio Modules:${NC}"
        print_status_indexed "1" "AC97 Driver (Audio Hardware)" "$AUDIO"
        
        echo ""
        echo -e "${YELLOW}Global Options:${NC}"
        echo "  e) Enable All Audio"
        echo "  d) Disable All Audio"
        echo "  b) Back to Main Menu"
        echo ""
        read -p "Enter your choice: " choice
        
        case $choice in
            1)
                NEW_VALUE=$((1 - AUDIO))
                update_config "ENABLE_AUDIO" "$NEW_VALUE"
                ;;
            e|E)
                update_config "ENABLE_AUDIO" "1"
                echo -e "${GREEN}All audio modules enabled.${NC}"
                sleep 1
                ;;
            d|D)
                update_config "ENABLE_AUDIO" "0"
                echo -e "${GREEN}All audio modules disabled.${NC}"
                sleep 1
                ;;
            b|B)
                break
                ;;
            *)
                echo -e "${RED}Invalid option.${NC}"
                sleep 1
                ;;
        esac
    done
}

gpu_menu() {
    while true; do
        clear
        echo -e "${BLUE}=====================================${NC}"
        echo -e "${BLUE}   GPU Subsystem Configuration${NC}"
        echo -e "${BLUE}=====================================${NC}"
        echo ""
        
        GPU=$(read_config "ENABLE_GPU")
        
        echo -e "${MAGENTA}GPU Modules:${NC}"
        print_status_indexed "1" "Graphics Driver (Display Management)" "$GPU"
        
        echo ""
        echo -e "${YELLOW}Global Options:${NC}"
        echo "  e) Enable All GPU"
        echo "  d) Disable All GPU"
        echo "  b) Back to Main Menu"
        echo ""
        read -p "Enter your choice: " choice
        
        case $choice in
            1)
                NEW_VALUE=$((1 - GPU))
                update_config "ENABLE_GPU" "$NEW_VALUE"
                ;;
            e|E)
                update_config "ENABLE_GPU" "1"
                echo -e "${GREEN}All GPU modules enabled.${NC}"
                sleep 1
                ;;
            d|D)
                update_config "ENABLE_GPU" "0"
                echo -e "${GREEN}All GPU modules disabled.${NC}"
                sleep 1
                ;;
            b|B)
                break
                ;;
            *)
                echo -e "${RED}Invalid option.${NC}"
                sleep 1
                ;;
        esac
    done
}

pci_menu() {
    while true; do
        clear
        echo -e "${BLUE}=====================================${NC}"
        echo -e "${BLUE}   PCI Subsystem Configuration${NC}"
        echo -e "${BLUE}=====================================${NC}"
        echo ""
        
        PCI=$(read_config "ENABLE_PCI")
        
        echo -e "${MAGENTA}PCI Modules:${NC}"
        print_status_indexed "1" "PCI Device Detection (Device Discovery)" "$PCI"
        
        echo ""
        echo -e "${YELLOW}Global Options:${NC}"
        echo "  e) Enable All PCI"
        echo "  d) Disable All PCI"
        echo "  b) Back to Main Menu"
        echo ""
        read -p "Enter your choice: " choice
        
        case $choice in
            1)
                NEW_VALUE=$((1 - PCI))
                update_config "ENABLE_PCI" "$NEW_VALUE"
                ;;
            e|E)
                update_config "ENABLE_PCI" "1"
                echo -e "${GREEN}All PCI modules enabled.${NC}"
                sleep 1
                ;;
            d|D)
                update_config "ENABLE_PCI" "0"
                echo -e "${GREEN}All PCI modules disabled.${NC}"
                sleep 1
                ;;
            b|B)
                break
                ;;
            *)
                echo -e "${RED}Invalid option.${NC}"
                sleep 1
                ;;
        esac
    done
}

usb_menu() {
    while true; do
        clear
        echo -e "${BLUE}=====================================${NC}"
        echo -e "${BLUE}   USB Subsystem Configuration${NC}"
        echo -e "${BLUE}=====================================${NC}"
        echo ""
        
        USB=$(read_config "ENABLE_USB")
        
        echo -e "${MAGENTA}USB Modules:${NC}"
        print_status_indexed "1" "USB Controller (Device Support)" "$USB"
        
        echo ""
        echo -e "${YELLOW}Global Options:${NC}"
        echo "  e) Enable All USB"
        echo "  d) Disable All USB"
        echo "  b) Back to Main Menu"
        echo ""
        read -p "Enter your choice: " choice
        
        case $choice in
            1)
                NEW_VALUE=$((1 - USB))
                update_config "ENABLE_USB" "$NEW_VALUE"
                ;;
            e|E)
                update_config "ENABLE_USB" "1"
                echo -e "${GREEN}All USB modules enabled.${NC}"
                sleep 1
                ;;
            d|D)
                update_config "ENABLE_USB" "0"
                echo -e "${GREEN}All USB modules disabled.${NC}"
                sleep 1
                ;;
            b|B)
                break
                ;;
            *)
                echo -e "${RED}Invalid option.${NC}"
                sleep 1
                ;;
        esac
    done
}

# ========================================
# MAIN MENU
# ========================================

main_menu() {
    while true; do
        clear
        echo -e "${BLUE}=====================================${NC}"
        echo -e "${BLUE}   Minimal OS Module Configuration${NC}"
        echo -e "${BLUE}=====================================${NC}"
        echo ""
        
        # Read current config values
        AUDIO=$(read_config "ENABLE_AUDIO")
        GPU=$(read_config "ENABLE_GPU")
        PCI=$(read_config "ENABLE_PCI")
        USB=$(read_config "ENABLE_USB")
        
        # Display current status overview
        echo -e "${YELLOW}Module Categories:${NC}"
        echo ""
        print_status_indexed "1" "Audio Subsystem" "$AUDIO"
        print_status_indexed "2" "GPU Subsystem" "$GPU"
        print_status_indexed "3" "PCI Subsystem" "$PCI"
        print_status_indexed "4" "USB Subsystem" "$USB"
        
        echo ""
        echo -e "${YELLOW}Quick Actions:${NC}"
        echo "  a) Enable All Modules"
        echo "  n) Disable All Modules"
        echo "  s) Save and Exit"
        echo "  q) Exit without Saving"
        echo ""
        read -p "Select category or action: " choice
        
        case $choice in
            1)
                audio_menu
                ;;
            2)
                gpu_menu
                ;;
            3)
                pci_menu
                ;;
            4)
                usb_menu
                ;;
            a|A)
                update_config "ENABLE_AUDIO" "1"
                update_config "ENABLE_GPU" "1"
                update_config "ENABLE_PCI" "1"
                update_config "ENABLE_USB" "1"
                echo -e "${GREEN}All modules enabled.${NC}"
                sleep 1
                ;;
            n|N)
                update_config "ENABLE_AUDIO" "0"
                update_config "ENABLE_GPU" "0"
                update_config "ENABLE_PCI" "0"
                update_config "ENABLE_USB" "0"
                echo -e "${GREEN}All modules disabled.${NC}"
                sleep 1
                ;;
            s|S)
                echo -e "${GREEN}Configuration saved!${NC}"
                echo ""
                echo "Current configuration:"
                echo "  ENABLE_AUDIO=$(read_config 'ENABLE_AUDIO')"
                echo "  ENABLE_GPU=$(read_config 'ENABLE_GPU')"
                echo "  ENABLE_PCI=$(read_config 'ENABLE_PCI')"
                echo "  ENABLE_USB=$(read_config 'ENABLE_USB')"
                echo ""
                echo -e "${YELLOW}Note: Run 'make clean && make build-x86_64' to rebuild the kernel.${NC}"
                exit 0
                ;;
            q|Q)
                echo -e "${YELLOW}Exit without saving.${NC}"
                exit 0
                ;;
            *)
                echo -e "${RED}Invalid option.${NC}"
                sleep 1
                ;;
        esac
    done
}

# Start main menu
main_menu
