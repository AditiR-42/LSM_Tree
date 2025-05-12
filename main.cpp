#include "lsm_tree.hh"
#include <iostream>
#include <map>
#include <sstream>
#include <fstream>
#include <vector>
#include <thread> 
#include <mutex> 
#include <memory> 
#include <fstream> 
#include <cstdio> 

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

// --- Server Configuration ---
const int SERVER_PORT = 8080;
const int MAX_CLIENTS = 64;

std::unique_ptr<lsm_tree> db;
std::mutex db_mutex;

// Function to handle communication with a single client
void handle_client(int client_socket) {
    std::cout << "Client connected (socket: " << client_socket << ")" << std::endl;

    char buffer[1024];
    std::string client_buffer; // Buffer to accumulate data for line processing
    ssize_t bytes_received;

    // Loop to read commands from the client
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
            char command;
            ss >> command;

            // Use a stringstream to capture output from LSM tree methods
            std::stringstream response_ss;
            std::string client_response = ""; // Final string to send back

            std::cout << "Received command '" << command << "' from socket " << client_socket << std::endl; // Debug

            // Acquire mutex before accessing the shared database
            std::lock_guard<std::mutex> lock(db_mutex);

            try {
                switch (command) {
                    case 'p': { // put
                        int key, value;
                        if (ss >> key >> value) {
                            db->insert({key, value});
                            client_response = "OK\n";
                        } else {
                             client_response = "Error: Invalid put format\n";
                        }
                        break;
                    }
                    case 'g': { // get
                        int key;
                        if (ss >> key) {
                            // get now writes to the provided ostream
                            db->get(key, response_ss);
                            client_response = response_ss.str(); // Capture output
                        } else {
                            client_response = "Error: Invalid get format\n\n"; // Add newline to match get's empty output format
                        }
                        break;
                    }
                    case 'd': { // del
                        int key;
                        if (ss >> key) {
                            db->delete_key(key);
                            client_response = "OK\n";
                        } else {
                            client_response = "Error: Invalid del format\n";
                        }
                        break;
                    }
                    case 'r': { // range
                        int startKey, endKey;
                        if (ss >> startKey >> endKey) {
                            // range now writes to the provided ostream
                            db->range(startKey, endKey, response_ss);
                            client_response = response_ss.str(); // Capture output
                        } else {
                            client_response = "Error: Invalid range format\n";
                        }
                        break;
                    }
                    case 'l': { // load
                        std::string fileName;
                        if (ss >> fileName) {
                            db->load(fileName); // The load function is now part of lsm_tree
                            client_response = "OK\n"; // Assuming load is successful if no exception
                        } else {
                             client_response = "Error: Invalid load format\n";
                        }
                        break;
                    }
                    case 's': { // stats
                        // printStats now writes to the provided ostream
                        db->printStats(response_ss);
                        client_response = response_ss.str(); // Capture output
                        break;
                    }
                    case 'c': { // cleanup (added for testing)
                         db->cleanup_files();
                         client_response = "Cleanup initiated.\n";
                         break;
                    }
                    default:
                        client_response = "Unknown command: please use p, g, d, r, l, s, or c\n";
                }
            } catch (const std::exception& e) {
                client_response = "Error: " + std::string(e.what()) + "\n";
            } catch (...) {
                client_response = "Error: An unknown exception occurred.\n";
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
                        } else {
                            std::cerr << "Send error on socket " << client_socket << ": " << strerror(errno) << std::endl;
                        }
                        goto close_client_socket; // Use goto to exit thread function and close socket
                    }
                    total_sent += sent;
                    remaining -= sent;
                }
            }
            // --- End process command_line ---
        }
        // If bytes_received > 0 but no newline was found, continue the read loop
        // to get more data into client_buffer until a newline completes a command.
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
            perror("Error accepting client connection");
            // Depending on the error, might continue or break
            continue;
        }

        // Client connected, create a new thread to handle it
        std::thread client_handler(handle_client, client_socket);
        // Detach the thread. The thread will manage its own lifecycle and close the socket.
        // This is simple, but means the main thread doesn't wait for clients.
        client_handler.detach();
    }

    // This part is typically unreachable in a server that runs indefinitely
    close(server_socket); // Close the listening socket
    return 0;
}