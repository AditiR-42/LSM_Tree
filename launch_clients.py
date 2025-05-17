#!/usr/bin/env python3

import subprocess
import sys
import os
import argparse
import concurrent.futures
import time

def run_single_client_load(client_exec_path, command_file_to_load, l_threads_count, client_id):
    """
    Launches a single client process and feeds it the 'l' command via stdin.
    Waits for the client process to complete.
    """
    print(f"Client {client_id}: Starting to load '{command_file_to_load}'...")

    command_string_for_client_stdin = f"l {command_file_to_load} {l_threads_count}\n"

    # The actual process command is just the client executable path
    process_cmd = [client_exec_path]

    try:
        result = subprocess.run(
            process_cmd,
            input=command_string_for_client_stdin,
            capture_output=True,
            text=True,
            check=False, # Don't raise exception on non-zero exit
            # timeout=600 # Example: timeout after 10 minutes
        )

        print(f"Client {client_id}: Finished with exit code {result.returncode}")

        # Print captured output from the client for debugging/verification
        if result.stdout:
            print(f"--- Client {client_id} STDOUT ---")
            print(result.stdout.strip()) 
            print("--------------------------")
        if result.stderr:
            print(f"--- Client {client_id} STDERR ---")
            print(result.stderr.strip())
            print("--------------------------")

        if result.returncode != 0:
             print(f"Client {client_id}: Warning: Client exited with non-zero status code {result.returncode}")


    except FileNotFoundError:
        print(f"Client {client_id}: Error: Client executable '{client_exec_path}' not found.")
        print(f"Client {client_id}: Make sure the client is built and located at '{client_exec_path}'.")
    except Exception as e:
        print(f"Client {client_id}: An unexpected error occurred: {e}")

# --- Main Execution ---
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Launch multiple concurrent clients to connect to the server and load commands from a specified file."
    )
    parser.add_argument(
        "-n", "--clients",
        type=int,
        required=True,
        help="Number of concurrent client processes to launch."
    )
    parser.add_argument(
        "-f", "--file",
        type=str,
        required=True,
        help="Path to the file containing the commands that *each* client will load using the 'l' command."
    )
    parser.add_argument(
        "-t", "--l-threads",
        type=int,
        default=8, # Default number of internal threads for the 'l' command in the client
        help="Number of threads for the 'l' command *within* each client process. Defaults to 8."
    )
    parser.add_argument(
        "--client-exec",
        type=str,
        default="./client", # Default path to the client executable
        help="Path to the client executable. Defaults to ./client."
    )

    args = parser.parse_args()

    num_clients = args.clients
    commands_file_for_load = args.file
    l_threads_per_client = args.l_threads
    client_executable_path = args.client_exec

    # Basic validation
    if not os.path.exists(client_executable_path):
        print(f"Error: Client executable not found at '{client_executable_path}'")
        sys.exit(1)

    if not os.path.exists(commands_file_for_load):
         print(f"Error: Commands file for load not found at '{commands_file_for_load}'")
         sys.exit(1)

    if num_clients <= 0:
        print("Error: Number of clients must be a positive integer.")
        sys.exit(1)

    if l_threads_per_client <= 0:
        print("Error: Number of threads for 'l' must be a positive integer.")
        sys.exit(1)


    print(f"--- Launching {num_clients} clients ---")
    print(f"Each client will load commands from: '{commands_file_for_load}'")
    print(f"Each client's 'l' command will use {l_threads_per_client} internal threads.")
    print("-" * 25)

    total_load_start_time = time.perf_counter()

    # Use a ThreadPoolExecutor to manage concurrent execution.
    with concurrent.futures.ThreadPoolExecutor(max_workers=num_clients) as executor:
        futures = [
            executor.submit(
                run_single_client_load,
                client_executable_path,
                commands_file_for_load,
                l_threads_per_client,
                i + 1 # Client ID for logging
            )
            for i in range(num_clients)
        ]

        # Simple wait for all tasks to complete:
        executor.shutdown(wait=True)

    total_load_end_time = time.perf_counter()
    total_elapsed_time = total_load_end_time - total_load_start_time

    print("-" * 25)
    print("All clients finished execution.")
    print(f"Total time for all clients to complete: {total_elapsed_time:.4f} seconds.")