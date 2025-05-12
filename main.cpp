#include "lsm_tree.hh"
#include <iostream>
#include <map>
#include <sstream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <cstdio> // For perror, remove
#include <atomic> // For command index

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> // For close

#include <cerrno>
#include <cstring> // For strerror
#include <chrono> // For timing

// --- Server Configuration ---
const int SERVER_PORT = 8080;
const int MAX_CLIENTS = 64;
const int MAX_FILE_COMMAND_THREADS = 32; // Limit the number of threads for 'l' command

std::unique_ptr<lsm_tree> db;
std::mutex db_mutex; // Global mutex for operations modifying the tree structure (insert, delete, cleanup, merge). Read operations should ideally not hold this for long.


// Helper function to execute a single command and capture output
// Returns false if sending fails critically (e.g., EPIPE), true otherwise.
// Takes a mutex specifically for sending output.
bool execute_command(char command, std::stringstream& ss_args, int client_socket,
                     std::mutex& db_mutex, lsm_tree& db_instance, std::mutex& client_send_mutex)
{
    std::stringstream response_ss; // Use a stringstream to capture output
    std::string client_response = ""; // Final string to send back

    // Determine if this command needs the global db_mutex
    bool needs_global_write_lock = (command == 'p' || command == 'd' || command == 'c');
    bool needs_global_read_lock = (command == 's'); // Stats reads metadata sequentially

    if (needs_global_write_lock || needs_global_read_lock) {
        db_mutex.lock(); // Acquire mutex for these specific ops
    }


    try {
        switch (command) {
            case 'p': { // put
                int key, value;
                if (ss_args >> key >> value) {
                    db_instance.insert({key, value});
                    client_response = "OK\n";
                } else {
                     client_response = "Error: Invalid put format. Usage: p <key> <value>\n";
                }
                break;
            }
            case 'g': { // get
                int key;
                if (ss_args >> key) {
                    // get now writes to the provided ostream (response_ss)
                    // get internally uses getValueForKey which uses parallel level searches
                    // getValueForKey does *not* acquire the global db_mutex, it relies on level mutexes.
                    // So 'g' doesn't hold the global lock here.
                    db_instance.get(key, response_ss); // This call does its own cout_mutex locking for output
                    client_response = response_ss.str(); // Capture output
                } else {
                    client_response = "Error: Invalid get format. Usage: g <key>\n\n"; // Match get's empty output format on error
                }
                break;
            }
            case 'd': { // del
                int key;
                if (ss_args >> key) {
                    db_instance.delete_key(key); // delete_key calls insert internally, which acquires db_mutex
                    client_response = "OK\n";
                } else {
                    client_response = "Error: Invalid del format. Usage: d <key>\n";
                }
                break;
            }
            case 'r': { // range
                int startKey, endKey;
                if (ss_args >> startKey >> endKey) {
                    // range now writes to the provided ostream (response_ss)
                    // range internally calls getValueForKey for each key, which uses parallel level searches
                    // range also uses cout_mutex internally.
                    // So 'r' doesn't hold the global lock here.
                    db_instance.range(startKey, endKey, response_ss); // Corrected range order
                    client_response = response_ss.str(); // Capture output
                } else {
                    client_response = "Error: Invalid range format. Usage: r <start_key> <end_key>\n";
                }
                break;
            }
            case 's': { // stats
                // printStats internally uses sequential locking of levels.
                // We hold the global lock for 's' for a more consistent snapshot (relative).
                db_instance.printStats(response_ss); // This call does its own cout_mutex locking for output
                client_response = response_ss.str(); // Capture output
                break;
            }
            case 'c': { // cleanup
                 db_instance.cleanup_files(); // cleanup_files acquires locks internally for level lists and file deletion
                 client_response = "Cleanup initiated.\n";
                 break;
            }
            case 'l': { // The 'l' command is handled specifically outside this function
                 client_response = "Error: 'l' command should be processed by the main handler, not execute_command.\n";
                 break;
            }
            default:
                client_response = "Unknown command: please use p, g, d, r, l, s, or c\n";
        }
    } catch (const std::exception& e) {
        client_response = "Error: " + std::string(e.what()) + "\n";
    } catch (...) {
        client_response = "Error: An unknown exception occurred during command execution.\n";
    }

    if (needs_global_write_lock || needs_global_read_lock) {
        db_mutex.unlock(); // Release mutex if acquired
    }

    // Send the captured response back to the client
    if (!client_response.empty()) {
        // Synchronize sending to the socket
        std::lock_guard<std::mutex> send_lock(client_send_mutex);
        const char* response_data = client_response.c_str();
        size_t total_sent = 0;
        size_t remaining = client_response.length();
        while (remaining > 0) {
            ssize_t sent = send(client_socket, response_data + total_sent, remaining, 0);
            if (sent < 0) {
                // EPIPE indicates client disconnected, making further sends impossible
                if (errno == EPIPE) {
                     // std::cerr << "Client socket " << client_socket << " broken pipe during send." << std::endl; // Too verbose for every error
                     return false; // Indicate fatal send error (client disconnected)
                } else {
                    std::cerr << "Send error on socket " << client_socket << ": " << strerror(errno) << std::endl;
                }
                // For other send errors, might retry, but EPIPE is definitive.
                // Treat any send error in this loop as a reason to abandon the connection.
                return false;
            }
            total_sent += sent;
            remaining -= sent;
        }
    }
    return true; // Indicate command executed and response (if any) sent successfully
}


// Worker function for processing commands from a file in parallel
void file_command_worker(int thread_id, const std::vector<std::string>& commands,
                         std::atomic<size_t>& command_index, int client_socket,
                         std::mutex& db_mutex, lsm_tree& db_instance, std::mutex& client_send_mutex)
{
    // std::cout << "File worker thread " << thread_id << " started." << std::endl; // Debug

    while (true) {
        size_t current_command_idx = command_index.fetch_add(1); // Atomically get next index

        if (current_command_idx >= commands.size()) {
            // No more commands left
            break;
        }

        const std::string& command_line = commands[current_command_idx];

        if (command_line.empty() || command_line[0] == '#') {
             // Skip empty lines or comments
             continue;
        }

        std::stringstream ss(command_line);
        char command_char;
        ss >> command_char;

        if (ss.fail()) {
             // Failed to read command character
             std::stringstream err_ss;
             err_ss << "Error in command file (line index " << current_command_idx << "): Invalid command format: '" << command_line << "'\n";
             std::lock_guard<std::mutex> send_lock(client_send_mutex); // Lock send
             std::string err_resp = err_ss.str();
             if (send(client_socket, err_resp.c_str(), err_resp.length(), 0) < 0 && errno == EPIPE) {
                 // Fatal send error, terminate this thread
                 std::cerr << "File worker thread " << thread_id << " broken pipe during error send." << std::endl;
                 break; // Exit worker loop
             }
             continue; // Get next command
        }

        // Execute the command and send response
        // execute_command handles db_mutex and client_send_mutex internally
        if (!execute_command(command_char, ss, client_socket, db_mutex, db_instance, client_send_mutex)) {
             // Fatal send error occurred in execute_command, terminate this thread
             break; // Exit worker loop
        }
    }

    // std::cout << "File worker thread " << thread_id << " finished." << std::endl; // Debug
}


// Function to handle communication with a single client
void handle_client(int client_socket) {
    std::cout << "Client connected (socket: " << client_socket << ")" << std::endl;

    char buffer[1024];
    std::string client_buffer; // Buffer to accumulate data for line processing
    ssize_t bytes_received;

    std::mutex client_send_mutex; // Mutex to synchronize sends to this client's socket

    // Loop to read commands from the client socket
    while ((bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate received data
        client_buffer += buffer;

        // Process the buffer line by line
        size_t newline_pos;
        while ((newline_pos = client_buffer.find('\n')) != std::string::npos) {
            std::string command_line = client_buffer.substr(0, newline_pos);
            client_buffer.erase(0, newline_pos + 1); // Remove processed line from buffer

            // --- Process command_line ---
            std::stringstream ss(command_line);
            char command_char = ' '; // Initialize
            ss >> command_char; // Read the first character as the command

            // Debug print, careful with potentially large file loads
            // std::cout << "Received command '" << command_char << "' from socket " << client_socket << ": '" << command_line << "'" << std::endl; // Debug

            if (ss.fail() && !command_line.empty() && command_line[0] != '#') {
                 // Failed to read the command character itself for a non-empty, non-comment line
                 std::string response = "Error: Invalid command format\n";
                 std::lock_guard<std::mutex> send_lock(client_send_mutex);
                 send(client_socket, response.c_str(), response.length(), 0);
                 continue; // Get next line from buffer
            }
             if (command_line.empty() || command_line[0] == '#') {
                 // Skip empty lines or comments
                 continue;
             }

            if (command_char == 'l') {
                // Special case: 'l' command loads commands from a file and processes in parallel
                std::string fileName;
                int num_threads = 1; // Default to 1 thread if not specified

                if (ss >> fileName) {
                    if (ss >> num_threads) {
                         // Read number of threads if provided
                    }
                    // Validate num_threads
                    if (num_threads <= 0 || num_threads > MAX_FILE_COMMAND_THREADS) {
                        std::stringstream err_ss;
                        err_ss << "Error: Invalid number of threads specified for load. Must be between 1 and " << MAX_FILE_COMMAND_THREADS << ".\n";
                        std::lock_guard<std::mutex> send_lock(client_send_mutex);
                        send(client_socket, err_ss.str().c_str(), err_ss.str().length(), 0);
                        continue; // Get next line from buffer
                    }


                    std::ifstream command_file(fileName);
                    if (!command_file.is_open()) {
                         std::cerr << "Error opening file for load: " << fileName << std::endl;
                         std::stringstream err_ss;
                         err_ss << "Error: Could not open file '" << fileName << "' for loading.\n";
                         std::lock_guard<std::mutex> send_lock(client_send_mutex);
                         send(client_socket, err_ss.str().c_str(), err_ss.str().length(), 0);
                    } else {
                        std::cout << "Processing commands from file: " << fileName << " with " << num_threads << " threads for client socket " << client_socket << std::endl;

                        // Read all commands from the file into a vector
                        std::vector<std::string> file_commands;
                        std::string file_command_line;
                        while (std::getline(command_file, file_command_line)) {
                            file_commands.push_back(file_command_line);
                        }
                        command_file.close();

                        if (file_commands.empty()) {
                             std::string msg = "Info: File '" + fileName + "' is empty or contains only comments.\n";
                             std::lock_guard<std::mutex> send_lock(client_send_mutex);
                             send(client_socket, msg.c_str(), msg.length(), 0);
                             continue; // Get next line from buffer
                        }


                        // --- Parallel File Processing ---
                        std::atomic<size_t> command_index = 0; // Shared index for workers to pick commands

                        auto load_start_time = std::chrono::high_resolution_clock::now();

                        std::vector<std::thread> file_worker_threads;
                        for (int i = 0; i < num_threads; ++i) {
                            file_worker_threads.emplace_back(
                                file_command_worker, i + 1, std::cref(file_commands), std::ref(command_index),
                                client_socket, std::ref(db_mutex), std::ref(*db), std::ref(client_send_mutex)
                            );
                        }

                        // Wait for all file worker threads to complete
                        for (auto& t : file_worker_threads) {
                            if (t.joinable()) {
                                t.join();
                            }
                        }

                        auto load_end_time = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<double> load_elapsed = load_end_time - load_start_time;

                        std::stringstream result_ss;
                        result_ss << "OK: File '" << fileName << "' processed in " << load_elapsed.count() << " seconds.\n";
                        std::string final_load_response = result_ss.str();

                         // Send the final result message for the load command
                         std::lock_guard<std::mutex> send_lock(client_send_mutex);
                         if (send(client_socket, final_load_response.c_str(), final_load_response.length(), 0) < 0 && errno == EPIPE) {
                              // Fatal send error after load complete
                              goto close_client_socket; // Exit handler
                         }

                    }
                } else {
                    std::stringstream err_ss;
                    err_ss << "Error: Invalid load format. Usage: l <filename> [num_threads]\n";
                    std::lock_guard<std::mutex> send_lock(client_send_mutex);
                    send(client_socket, err_ss.str().c_str(), err_ss.str().length(), 0);
                }
            } else {
                // Not the 'l' command, execute and send response using the helper
                 // Pass 'ss', which still contains the args *after* the initial command_char read
                 // execute_command handles db_mutex and client_send_mutex internally
                 if (!execute_command(command_char, ss, client_socket, db_mutex, *db, client_send_mutex)) {
                     goto close_client_socket; // Send failed (e.g., EPIPE), exit handler
                 }
            }
            // --- End process command_line ---
        }
        // If bytes_received > 0 but no newline was found, client_buffer holds partial data.
        // The loop continues to receive more data until a complete line arrives or error/disconnect.
    }

    // If bytes_received is 0, client disconnected cleanly. If -1, an error occurred.
    if (bytes_received == 0) {
         std::cout << "Client socket " << client_socket << " disconnected cleanly." << std::endl;
    } else { // bytes_received < 0
         std::cerr << "Read error on socket " << client_socket << ": " << strerror(errno) << std::endl;
    }

close_client_socket:
    // Close the client socket when done
    close(client_socket);
    std::cout << "Client socket " << client_socket << " closed." << std::endl;
}

int main() {
    // Initialize LSM Tree
    try {
        // The constructor *loads* existing data if available.
        // We do NOT call cleanup_files here if we want persistence between runs.
        db = std::unique_ptr<lsm_tree>(new lsm_tree());
        std::cout << "LSM Tree initialized and loading existing data if available." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize LSM Tree: " << e.what() << std::endl;
        return 1;
    }

    // --- Setup Server Socket ---
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Error creating server socket");
        // db is a unique_ptr, its destructor runs on return
        return 1;
    }

    // Optional: Allow reusing the address immediately after the socket is closed
    int optval = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("Error setting socket options");
        // Continue, it's just an optimization, but report it
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces

    // Bind the socket
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding server socket");
        close(server_socket);
        // db is a unique_ptr, its destructor runs on return
        return 1;
    }

    // Listen for incoming connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Error listening on server socket");
        close(server_socket);
        // db is a unique_ptr, its destructor runs on return
        return 1;
    }

    std::cout << "Server listening on port " << SERVER_PORT << "..." << std::endl;

    // --- Accept and Handle Clients ---
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);

        if (client_socket < 0) {
            // Handle accept errors - most common is EINTR (interrupted system call)
            if (errno == EINTR) {
                continue; // Try accepting again
            } else {
                perror("Error accepting client connection");
                // Depending on the error, might need to break the loop
                break; // Exit server loop on critical accept error
            }
        }

        // Client connected, create a new thread to handle it
        // Pass `db` raw pointer (safe because it's unique_ptr managed in main and lives for app duration)
        // Pass `db_mutex` by reference
        std::thread client_handler(handle_client, client_socket);
        // Detach the thread. The thread will manage its own lifecycle and close the socket.
        // This is simple, but means the main thread doesn't wait for clients.
        client_handler.detach();
    }

    // This part is typically unreachable in a server that runs indefinitely
    std::cerr << "Server shutting down." << std::endl;
    close(server_socket); // Close the listening socket

    // The unique_ptr `db` will be automatically deleted here.

    return 0;
}