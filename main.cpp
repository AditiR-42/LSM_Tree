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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> // For close

#include <cerrno>
#include <cstring> // For strerror

// --- Server Configuration ---
const int SERVER_PORT = 8080;
const int MAX_CLIENTS = 64;

std::unique_ptr<lsm_tree> db;
std::mutex db_mutex;

// Helper function to execute a single command and send the response
// Returns false if sending fails critically (e.g., EPIPE), true otherwise.
bool execute_and_send(char command, std::stringstream& ss_args, int client_socket, std::mutex& db_mutex, lsm_tree& db_instance) {
    std::stringstream response_ss; // Use a stringstream to capture output from LSM tree methods
    std::string client_response = ""; // Final string to send back

    // Acquire mutex for this command execution
    std::lock_guard<std::mutex> lock(db_mutex);

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
                    db_instance.get(key, response_ss);
                    client_response = response_ss.str(); // Capture output
                } else {
                    client_response = "Error: Invalid get format. Usage: g <key>\n\n"; // Match get's empty output format on error
                }
                break;
            }
            case 'd': { // del
                int key;
                if (ss_args >> key) {
                    db_instance.delete_key(key);
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
                    db_instance.range(startKey, endKey, response_ss);
                    client_response = response_ss.str(); // Capture output
                } else {
                    client_response = "Error: Invalid range format. Usage: r <start_key> <end_key>\n";
                }
                break;
            }
            case 's': { // stats
                // printStats now writes to the provided ostream (response_ss)
                db_instance.printStats(response_ss);
                client_response = response_ss.str(); // Capture output
                break;
            }
            case 'c': { // cleanup
                 db_instance.cleanup_files();
                 client_response = "Cleanup initiated.\n";
                 break;
            }
            case 'l': { // The 'l' command is handled specifically outside this function
                 client_response = "Error: 'l' command should be processed by the main handler, not execute_and_send.\n";
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

    // Send the captured response back to the client
    if (!client_response.empty()) {
        const char* response_data = client_response.c_str();
        size_t total_sent = 0;
        size_t remaining = client_response.length();
        while (remaining > 0) {
            ssize_t sent = send(client_socket, response_data + total_sent, remaining, 0);
            if (sent < 0) {
                if (errno == EPIPE) { // Client disconnected unexpectedly
                     std::cerr << "Client socket " << client_socket << " broken pipe during send." << std::endl;
                     return false; // Indicate fatal send error (client disconnected)
                } else {
                    std::cerr << "Send error on socket " << client_socket << ": " << strerror(errno) << std::endl;
                }
                // For other send errors, we might choose to continue, but EPIPE is fatal.
                // Simplest is to treat any send error here as potentially fatal for the connection.
                return false;
            }
            total_sent += sent;
            remaining -= sent;
        }
    }
    return true; // Indicate command executed and response (if any) sent successfully
}


// Function to handle communication with a single client
void handle_client(int client_socket) {
    std::cout << "Client connected (socket: " << client_socket << ")" << std::endl;

    char buffer[1024];
    std::string client_buffer; // Buffer to accumulate data for line processing
    ssize_t bytes_received;

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
            char command_char;
            ss >> command_char; // Read the first character as the command

            // Debug print, careful with potentially large file loads
            // std::cout << "Received command '" << command_char << "' from socket " << client_socket << std::endl;

            if (ss.fail() && !command_line.empty()) {
                // Failed to read the command character itself
                 std::string response = "Error: Invalid command format\n";
                 send(client_socket, response.c_str(), response.length(), 0); // Simplistic error send
                 continue; // Get next line from buffer
            }
             if (command_line.empty()) {
                 // Skip empty lines
                 continue;
             }

            if (command_char == 'l') {
                // Special case: 'l' command loads commands from a file
                std::string fileName;
                if (ss >> fileName) {
                    std::ifstream command_file(fileName);
                    if (!command_file.is_open()) {
                         std::cerr << "Error opening file for load: " << fileName << std::endl;
                         std::string error_response = "Error: Could not open file '" + fileName + "' for loading.\n";
                         send(client_socket, error_response.c_str(), error_response.length(), 0);
                    } else {
                        std::cout << "Processing commands from file: " << fileName << " for client socket " << client_socket << std::endl;
                        std::string file_command_line;
                        bool load_successful = true;

                        // Read commands from the file line by line
                        while (std::getline(command_file, file_command_line)) {
                             if (file_command_line.empty()) continue; // Skip empty lines in the file

                             // Extract the command character from the file line
                             std::stringstream ss_file(file_command_line);
                             char file_command_char;
                             ss_file >> file_command_char;

                             if (ss_file.fail()) {
                                  std::cerr << "Warning: Failed to parse command char from file line: '" << file_command_line << "'" << std::endl;
                                  std::string parse_error_response = "Error in file '" + fileName + "': Invalid command format: '" + file_command_line + "'\n";
                                  if (send(client_socket, parse_error_response.c_str(), parse_error_response.length(), 0) < 0) {
                                       load_successful = false; // Cannot even report parse error, client likely disconnected
                                       break; // Stop processing file
                                  }
                                  continue; // Get next line from file
                             }

                              // Pass the rest of the line stream (after command char) to the helper
                              // This will execute the command and send the response to the client socket
                              if (!execute_and_send(file_command_char, ss_file, client_socket, db_mutex, *db)) {
                                   load_successful = false; // Send failed within helper (client disconnected)
                                   break; // Stop processing file
                              }
                        }
                        command_file.close();

                        if (load_successful) {
                            // Send a final "OK" for the load command itself, after all file commands are processed
                            std::string ok_response = "OK: File '" + fileName + "' processed.\n";
                            send(client_socket, ok_response.c_str(), ok_response.length(), 0);
                        } else {
                             std::cerr << "Processing file '" << fileName << "' interrupted due to client socket error." << std::endl;
                             // The `goto` below will handle closing the socket due to the send error
                        }
                    }
                } else {
                    std::string error_response = "Error: Invalid load format. Usage: l <filename>\n";
                    send(client_socket, error_response.c_str(), error_response.length(), 0);
                }
            } else {
                // Not the 'l' command, execute and send response using the helper
                 // Pass 'ss', which still contains the args *after* the initial command_char read
                 if (!execute_and_send(command_char, ss, client_socket, db_mutex, *db)) {
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
        db = std::unique_ptr<lsm_tree>(new lsm_tree());
        std::cout << "LSM Tree initialized." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize LSM Tree: " << e.what() << std::endl;
        return 1;
    }

    // --- Setup Server Socket ---
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Error creating server socket");
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
        return 1;
    }

    // Listen for incoming connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Error listening on server socket");
        close(server_socket);
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
        // Pass `db` pointer and `db_mutex` by reference to the thread
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