#include <iostream>
#include <string>
#include <vector>
#include <cstdio>   // For perror, close
#include <sstream>
#include <fstream>

// POSIX socket headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// Headers for error handling and types
#include <cerrno>
#include <cstring>


// --- Client Configuration ---
const std::string SERVER_IP = "127.0.0.1"; // Server address (localhost)
const int SERVER_PORT = 8080;              // Server port

// Helper to read exactly one newline-terminated line from the socket
// Returns true on success, false on error or disconnect.
bool read_line_from_socket(int sock, std::string& line) {
    line.clear();
    char buffer[1]; // Read one byte at a time
    ssize_t bytes_received;

    while (true) {
        bytes_received = recv(sock, buffer, 1, 0);
        if (bytes_received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recv failed");
                return false; // Fatal error
            }
            // If non-blocking, could yield. With blocking, shouldn't stay here.
            continue; // Retry
        } else if (bytes_received == 0) {
            // Connection closed
            if (line.empty()) {
                // If no data was read before disconnect, it's a clean close.
                 // std::cerr << "Connection closed by server." << std::endl; // Maybe too chatty
            } else {
                 // If data was read but no newline, it's a partial line at disconnect.
                 std::cerr << "Warning: Server closed connection with partial line in buffer." << std::endl;
            }
            return false; // Indicate disconnect
        }

        line += buffer[0];
        if (buffer[0] == '\n') {
            return true; // Found newline
        }
    }
}


int main() {
    // --- Create Client Socket ---
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error creating client socket");
        return 1;
    }

    // --- Setup Server Address Structure ---
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    // Convert IP address string to network address structure
    if (inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr) <= 0) {
        perror("Error converting IP address");
        close(sock);
        return 1;
    }

    // --- Connect to Server ---
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error connecting to server");
        close(sock);
        return 1;
    }

    std::cout << "Connected to server " << SERVER_IP << ":" << SERVER_PORT << std::endl;
    std::cout << "Enter commands (p key value, g key, d key, r start end, l filename [threads], s, c):" << std::endl;

    // --- Command Loop ---
    std::string command_line;
    while (getline(std::cin, command_line)) {
        if (command_line.empty()) {
            continue; // Skip empty lines
        }

        // Extract command type *before* sending
        std::stringstream ss_cmd_type(command_line);
        char command_type = ' ';
        ss_cmd_type >> command_type;

        // --- Send command ---
        std::string full_command = command_line + "\n";
        ssize_t sent = send(sock, full_command.c_str(), full_command.length(), 0);
        if (sent < 0) {
            perror("send failed");
            goto end_client_loop; // Exit loops on error
        }

        // --- Receive and print response based on command type ---
        std::string received_line;

        if (command_type == 's') {
            // For 's' (stats), read lines until the specific terminator
            bool stats_finished = false;
            while (!stats_finished && read_line_from_socket(sock, received_line)) {
                std::cout << received_line; // Print line including newline
                if (received_line == "----------------------\n") {
                    stats_finished = true;
                }
            }
            if (!stats_finished) {
                 // Error occurred or connection closed before terminator
                 goto end_client_loop;
            }

        } else if (command_type == 'l') {
             // For 'l' (load file), read lines until a specific load terminator
             bool load_finished = false;
             while (!load_finished && read_line_from_socket(sock, received_line)) {
                 // --- Suppression Logic for 'p' OKs during load ---
                 if (received_line == "OK\n") {
                      // Suppress "OK" responses from 'p' commands within the file
                      // Do not print this line.
                      // Do NOT set load_finished = true here, as other commands might follow in the file.
                 } else {
                      // Print all other lines (get/range results, errors, info, file open errors, final OK)
                      std::cout << received_line;
                 }
                 // --- End Suppression Logic ---

                 // Check for the load command terminators (success, error, info)
                 if (received_line.rfind("OK: File '", 0) == 0 && received_line.find(" processed in ") != std::string::npos && received_line.find(" seconds.\n") != std::string::npos) {
                      load_finished = true;
                 } else if (received_line.rfind("Error: Could not open file '", 0) == 0 && received_line.find(" for loading.\n") != std::string::npos) {
                      load_finished = true;
                 } else if (received_line.rfind("Error: Invalid number of threads specified for load.\n", 0) == 0) {
                     load_finished = true;
                 } else if (received_line.rfind("Info: File '", 0) == 0 && received_line.find(" is empty or contains only comments.\n") != std::string::npos) {
                      load_finished = true;
                 }
             }
             if (!load_finished) {
                  // Error occurred or connection closed before terminator
                  goto end_client_loop;
             }

        } else {
            // For all other commands (p, g, d, r, c), read exactly one line and print it.
            // This assumes these commands send a single line response.
            // This is robust for p, g, d, c. For 'r', it might be okay depending on exact output format.
            if (read_line_from_socket(sock, received_line)) {
                std::cout << received_line; // Print line including newline
            } else {
                 // Error occurred or connection closed
                 goto end_client_loop;
            }
        }

    } // End while getline(std::cin...)

end_client_loop:
    // Close the socket
    close(sock);
    std::cout << "Connection closed." << std::endl;

    return 0;
}