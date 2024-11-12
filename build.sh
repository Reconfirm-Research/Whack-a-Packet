#!/bin/bash

# Exit on any error
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Building whack...${NC}"

# Function to check if running as root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}Error: Please run as root (sudo)${NC}"
        echo "Root privileges are required to install dependencies"
        exit 1
    fi
}

# Function to install dependencies
install_dependencies() {
    echo "Installing dependencies..."
    apt-get update
    apt-get install -y \
        build-essential \
        cmake \
        pkg-config \
        libxdp-dev \
        xdp-tools \
        linux-headers-$(uname -r) \
        libnuma-dev \
        clang \
        llvm \
        libelf-dev \
        zlib1g-dev

    # Create symlinks for XDP headers if they don't exist
    mkdir -p /usr/include/xdp
    if [ ! -f "/usr/include/xdp/xsk.h" ]; then
        ln -sf /usr/include/linux/xdp.h /usr/include/xdp/xsk.h
    fi
    if [ ! -f "/usr/include/xdp/libxdp.h" ]; then
        ln -sf /usr/include/linux/if_xdp.h /usr/include/xdp/libxdp.h
    fi

    # Verify XDP headers are properly installed
    if [ ! -f "/usr/include/linux/if_xdp.h" ] || [ ! -f "/usr/include/xdp/xsk.h" ]; then
        echo -e "${RED}Error: XDP headers not found after installation${NC}"
        exit 1
    fi
}

# Check kernel version (minimum 5.4 for AF_XDP)
echo "Checking kernel version..."
kernel_version=$(uname -r | cut -d. -f1,2)
if [ $(echo "$kernel_version < 5.4" | bc -l) -eq 1 ]; then
    echo -e "${RED}Error: Kernel version $kernel_version is too old${NC}"
    echo "AF_XDP requires kernel version 5.4 or newer"
    exit 1
fi

# Check for required headers
echo "Checking dependencies..."
if [ ! -f "/usr/include/linux/if_xdp.h" ] || \
   [ ! -f "/usr/include/xdp/xsk.h" ] || \
   [ ! -f "/usr/include/numa.h" ]; then
    echo -e "${YELLOW}Missing required headers. Installing dependencies...${NC}"
    check_root
    install_dependencies
fi

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    echo "Creating build directory..."
    mkdir build
fi

# Navigate to build directory
cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the project
echo "Building project..."
make -j$(nproc) VERBOSE=1

echo -e "${GREEN}Build completed successfully!${NC}"
echo ""
echo "To run whack (requires root privileges for AF_XDP):"
echo "sudo ./whack -i <interface> -d <domains_file> -r <resolvers_file> [options]"
echo ""
echo "Note: Make sure your network interface supports AF_XDP:"
echo "ethtool -i <interface> # Check driver compatibility"
echo ""
echo "For more information, see README.md"

# Print system information
echo -e "\nSystem Information:"
echo "-------------------"
echo "Kernel version: $(uname -r)"
echo "Architecture: $(uname -m)"
if [ -f "/etc/os-release" ]; then
    . /etc/os-release
    echo "OS: $PRETTY_NAME"
fi
echo "Compiler: $(cc --version | head -n1)"
echo "CMake: $(cmake --version | head -n1)"

# Print dependency information
echo -e "\nDependency Information:"
echo "----------------------"
echo "libxdp: $(dpkg -l | grep libxdp-dev || echo 'Not found')"
echo "libnuma: $(dpkg -l | grep libnuma-dev || echo 'Not found')"
echo "kernel headers: $(dpkg -l | grep linux-headers-$(uname -r) || echo 'Not found')"

# Check header files
echo -e "\nHeader Files:"
echo "------------"
for header in \
    "/usr/include/linux/if_xdp.h" \
    "/usr/include/xdp/xsk.h" \
    "/usr/include/xdp/libxdp.h" \
    "/usr/include/numa.h"; do
    if [ -f "$header" ]; then
        echo "$header: ${GREEN}Found${NC}"
    else
        echo "$header: ${RED}Missing${NC}"
    fi
done
