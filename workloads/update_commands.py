import sys
import os
import tempfile # Good practice for creating temporary files

def process_large_file(filename):
    """
    Reads a file line by line, replaces the first number on 'p' lines with numbers
    from 'g' lines in order (collected in a first pass), writes modified lines
    to a temp file, and finally replaces the original file. Memory efficient
    for large files.

    Args:
        filename (str): The path to the input text file.
    """
    g_values = []

    # --- Pass 1: Collect 'g' values ---
    # Read line by line to avoid loading the whole file into memory
    print(f"Pass 1: Collecting 'g' values from '{filename}'...")
    try:
        with open(filename, 'r') as f:
            for i, line in enumerate(f):
                stripped_line = line.strip()
                if stripped_line.startswith('g'):
                    parts = stripped_line.split()
                    if len(parts) > 1:
                        try:
                            g_value = int(parts[1])
                            g_values.append(g_value)
                        except ValueError:
                            print(f"Warning (Pass 1): Could not convert '{parts[1]}' to integer on line {i+1} ('{line.strip()}'). Skipping this 'g' value.")
                    else:
                        print(f"Warning (Pass 1): Skipping malformed 'g' line {i+1} (no value found): '{line.strip()}'")
        print(f"Pass 1 complete. Collected {len(g_values)} 'g' values.")

    except FileNotFoundError:
        print(f"Error: File '{filename}' not found.")
        sys.exit(1)
    except Exception as e:
        print(f"An error occurred during the first pass: {e}")
        sys.exit(1)

    # --- Pass 2: Process 'p' lines and write to a temporary file ---
    current_g_index = 0
    # Create a temporary file to write the modified content to.
    # We use delete=False because we need to rename it later.
    try:
        # Place the temporary file in the same directory as the input file
        # This helps if the system's temp directory is on a different volume or is small
        input_dir = os.path.dirname(os.path.abspath(filename))
        with tempfile.NamedTemporaryFile(mode='w+', delete=False, dir=input_dir) as temp_file:
            temp_filename = temp_file.name
            print(f"Pass 2: Processing lines and writing to temporary file '{temp_filename}'...")

            # Re-open the original file for reading
            with open(filename, 'r') as infile:
                for i, line in enumerate(infile):
                    stripped_line = line.strip()

                    if stripped_line.startswith('p'):
                        parts = stripped_line.split()
                        # A 'p' line with two numbers should have at least 3 parts ('p', num1, num2)
                        if len(parts) >= 3:
                            if current_g_index < len(g_values):
                                # Replace the first number (index 1) with the current g_value
                                parts[1] = str(g_values[current_g_index])
                                modified_line = " ".join(parts) + '\n'
                                temp_file.write(modified_line)
                                # Move to the next g_value for the next 'p' line
                                current_g_index += 1
                            else:
                                # This case implies a problem with the guarantee or Pass 1 warnings
                                print(f"Error (Pass 2): Ran out of 'g' values for 'p' line {i+1} ('{line.strip()}'). This shouldn't happen if the guarantee holds.")
                                # Write the original 'p' line as a fallback
                                temp_file.write(line)
                        else:
                            print(f"Warning (Pass 2): Skipping malformed 'p' line {i+1} ('{line.strip()}'). Keeping original line.")
                            temp_file.write(line) # Keep original malformed line

                    elif stripped_line.startswith('g'):
                        # This is a 'g' line, skip it (do not write to temp_file)
                        pass
                    else:
                        # This is neither 'p' nor 'g', keep it as is
                        temp_file.write(line)

        # Optional: Add a final check for count match
        if current_g_index != len(g_values):
             print(f"Warning: Used {current_g_index} 'g' values but collected {len(g_values)} total 'g' values. Discrepancy possible due to malformed lines.")

    except Exception as e:
        print(f"An error occurred during the second pass: {e}")
        # Clean up the temporary file if it was created before the error
        if 'temp_filename' in locals() and os.path.exists(temp_filename):
            print(f"Attempting to clean up temporary file: {temp_filename}")
            try:
                os.remove(temp_filename)
                print("Temporary file cleaned up.")
            except OSError as cleanup_error:
                print(f"Error during temporary file cleanup: {cleanup_error}")
        sys.exit(1)


    # --- Step 3: Replace the original file with the temporary file ---
    print(f"Pass 3: Replacing original file '{filename}' with temporary file '{temp_filename}'...")
    try:
        # Use os.replace for atomic replacement (safer than os.remove and os.rename)
        # It handles potential issues like permissions better and prevents data loss
        # if the process is interrupted between removing the old and renaming the new.
        os.replace(temp_filename, filename)
        print(f"Successfully processed '{filename}'. 'p' values replaced and 'g' lines removed.")
    except OSError as e:
        print(f"Error replacing original file '{filename}' with temporary file '{temp_filename}': {e}")
        print(f"The processed content is in the temporary file: {temp_filename}. Manual replacement may be needed.")
        sys.exit(1)


# --- Main execution block ---
if __name__ == "__main__":
    # Check if a filename was provided as a command-line argument
    if len(sys.argv) != 2:
        print("Usage: python your_script_name.py <input_filename>")
        print("Example: python your_script_name.py data.txt")
        sys.exit(1) # Exit with an error code if usage is incorrect
    else:
        input_filename = sys.argv[1]
        process_large_file(input_filename)