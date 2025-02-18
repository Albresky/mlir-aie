#!/bin/bash
set -e

# Convert installation directory to absolute path
INSTALL_DIR=$(realpath "$1")
if [ -z "$INSTALL_DIR" ]; then
    echo "Error: Installation directory not specified"
    echo "Usage: $0 <install_directory>"
    exit 1
fi

# Check if python directory exists
if [ ! -d "${INSTALL_DIR}/python" ]; then
    echo "Error: ${INSTALL_DIR}/python directory does not exist"
    exit 1
fi

# Check if aie package exists
if [ ! -d "${INSTALL_DIR}/python/aie" ]; then
    echo "Error: ${INSTALL_DIR}/python/aie directory does not exist"
    exit 1
fi

# Set MLIR Python package prefix for AIE
export HOST_MLIR_PYTHON_PACKAGE_PREFIX=aie

# Create temp directory for installation
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

# Create a virtual environment for building
python3 -m venv "${TEMP_DIR}/venv"
source "${TEMP_DIR}/venv/bin/activate"

# Clone mlir-python-extras
git clone https://github.com/makslevental/mlir-python-extras.git "${TEMP_DIR}/src"
cd "${TEMP_DIR}/src"

# Install build dependencies in the virtual environment
pip install -r requirements.txt

# Build the package
python setup.py build

# Only copy the extras directory
cp -r build/lib/aie/extras "${INSTALL_DIR}/python/aie/"

# Deactivate virtual environment
deactivate

# Verify installation
echo "Verifying installation..."
PYTHONPATH="${INSTALL_DIR}/python" python3 -c "
import aie.extras
print('MLIR extras installation successful')
"

echo "Patching successfully"
