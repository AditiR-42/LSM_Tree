#include <iostream>
#include <string>
#include <vector>
#include <cstdio>   // For perror, close
#include <sstream>  // Needed for command parsing logic if moved to client

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
    std::cout << "Enter commands (p key value, g key, d key, r start end, l filename, s, c):" << std::endl;

    // --- Command Loop ---
    std::string command_line;
    while (getline(std::cin, command_line)) {
        if (command_line.empty()) {
            std::cout << "Empty command." << std::endl;
            continue;
        }

        // Store the command type to anticipate response format
        char command_type = ' ';
        if (!command_line.empty()) {
             std::stringstream ss(command_line);
             ss >> command_type;
        }


        // Send command + newline to the server
        std::string full_command = command_line + "\n";
        ssize_t sent = send(sock, full_command.c_str(), full_command.length(), 0);
        if (sent < 0) {
            perror("send failed");
            // Assume connection lost
            goto end_client_loop; // Exit loops on error
        }

        // --- Receive and print response ---
        char buffer[1024]; // Buffer for receiving data
        std::string response_buffer; // Buffer to accumulate potentially fragmented responses
        bool response_finished = false; // Flag to indicate response is complete

        // Loop until the response is considered finished
        while (!response_finished) {
             ssize_t bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);

             if (bytes_received < 0) {
                 perror("recv failed");
                 goto end_client_loop; // Exit loops on error
             } else if (bytes_received == 0) {
                 // Server closed the connection - this might happen unexpectedly
                 std::cerr << "Server closed connection unexpectedly." << std::endl;
                 goto end_client_loop; // Exit loops
             }

             buffer[bytes_received] = '\0'; // Null-terminate received data
             response_buffer += buffer;

             // Process the buffer line by line
             size_t newline_pos;
             while ((newline_pos = response_buffer.find('\n')) != std::string::npos) {
                 std::string line = response_buffer.substr(0, newline_pos);
                 response_buffer.erase(0, newline_pos + 1);

                 // Decide if the response for this COMMAND TYPE is finished based on the line
                 if (command_type == 's') {
                     std::cout << line << std::endl; // Always print stats lines
                     // Stats command has a specific multi-line output ending
                     if (line == "----------------------") {
                         response_finished = true;
                     }
                 } else if (command_type == 'l') {
                     // For 'l', print most lines but suppress "OK" lines (assumed from 'p' commands)
                     if (line == "OK") {
                         // Suppress "OK" lines received during an 'l' command.
                         // Other lines (get/range results, errors, file open error, final OK) will be printed.
                     } else {
                         std::cout << line << std::endl; // Print all other lines during load
                     }

                     // Check if the response is finished based on the final lines
                     if (line.rfind("OK: File '", 0) == 0 && line.find(" processed.") != std::string::npos) {
                          response_finished = true;
                     } else if (line.rfind("Error: Could not open file '", 0) == 0 && line.find(" for loading.") != std::string::npos) {
                          response_finished = true;
                     }
                     // If it's neither of the ending lines, response_finished remains false, and we continue reading.

                 } else {
                     // For all other commands (p, g, d, r, c sent directly), print the line
                     std::cout << line << std::endl;
                     // For these commands, the response is generally a single line (OK, value, error).
                     // NOTE: 'r' (range) *can* output multiple lines. This current logic will
                     // unfortunately stop after the *first* line of a range result *if sent directly*.
                     // If you need multi-line range output when sent directly, the response_finished logic
                     // for 'r' would need a specific terminator from the server, similar to 's'.
                     response_finished = true;
                 }

                 // If the response is finished based on the line just processed,
                 // break the inner loop as well.
                 if (response_finished) break;

             } // End inner while (processing lines)

             // If response_finished is true here, the outer loop condition will catch it
             // and terminate after this iteration. If not, the outer loop continues recv'ing.

        } // End outer while (!response_finished)

        // Any data left in response_buffer here wasn't followed by a newline
        // (i.e., a partial line). In a clean protocol, this shouldn't happen
        // when response_finished becomes true based on a complete line marker.
        if (!response_buffer.empty()) {
             std::cerr << "Warning: Partial line left in buffer after response: '" << response_buffer << "'" << std::endl;
        }

    } // End while getline(std::cin...)

end_client_loop:
    // Close the socket
    close(sock);
    std::cout << "Connection closed." << std::endl;

    return 0;
}