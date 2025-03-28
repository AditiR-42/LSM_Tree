#!/bin/bash

# --- Configuration ---
# The exact name of the data directory created by your LSM Tree program
DATA_DIR_NAME="data"

# Path to the data directory relative to THIS script's location.
# Adjust if your script is located elsewhere relative to the data directory.
# Example: If script is in 'scripts/' and 'lsm_data' is in the parent dir:
DATA_DIR_REL_PATH="./${DATA_DIR_NAME}"
# Example: If script is in the SAME directory as 'lsm_data':
# DATA_DIR_REL_PATH="./${DATA_DIR_NAME}"
# --- End Configuration ---

# Get the directory where the script is located
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Construct the full path to the data directory
full_data_dir_path="${script_dir}/${DATA_DIR_REL_PATH}"
# Resolve potential '..' components to get a cleaner path for display (optional)
resolved_data_dir_path=$(cd "$full_data_dir_path" 2>/dev/null && pwd || echo "$full_data_dir_path")


echo "--- Database Cleanup Script ---"
echo "Script Directory:   ${script_dir}"
echo "Target Data Dir:    ${resolved_data_dir_path}" # Display the resolved path
echo "-----------------------------"

# Check if the target data directory exists
if [ -d "$full_data_dir_path" ]; then
    echo "Found data directory. Attempting to remove it..."
    # Remove the directory recursively and forcefully
    rm -rf "$full_data_dir_path"
    EXIT_CODE=$? # Capture the exit code of rm

    if [ $EXIT_CODE -eq 0 ]; then
        echo "SUCCESS: Data directory '${resolved_data_dir_path}' removed."
        exit 0
    else
        echo "ERROR: Failed to remove data directory '${resolved_data_dir_path}'. Exit code: ${EXIT_CODE}"
        exit $EXIT_CODE
    fi
else
    echo "INFO: Data directory '${resolved_data_dir_path}' not found. Nothing to clean."
    exit 0 # Exit successfully as there was nothing to do
fi