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
                 // Check for non-blocking errors if socket were non-blocking (not in this example)
                 perror("recv failed");
                 goto end_client_loop; // Exit loops on error
             } else if (bytes_received == 0) {
                 // Server closed the connection - unexpected mid-response for most commands
                 std::cerr << "Server closed connection." << std::endl;
                 goto end_client_loop; // Exit loops
             }

             buffer[bytes_received] = '\0'; // Null-terminate received data
             response_buffer += buffer;

             // Process the buffer line by line
             size_t newline_pos;
             while ((newline_pos = response_buffer.find('\n')) != std::string::npos) {
                 std::string line = response_buffer.substr(0, newline_pos);
                 std::cout << line << std::endl;
                 response_buffer.erase(0, newline_pos + 1);

                 // Decide if the response is finished based on the line and original command type
                 if (command_type == 's') {
                     // Stats command has a specific multi-line output ending
                     if (line == "----------------------") {
                         response_finished = true;
                     }
                 } else {
                     // For all other commands (p, g, d, r, l, c),
                     // the server's response (value, OK, error, range output)
                     // is expected to be a single line ending in a newline.
                     // Therefore, processing *any* complete line here
                     // means the response for the current command is finished.
                     response_finished = true;
                 }

                 // If the response is finished based on the line just processed,
                 // break the inner loop as well, no need to process more lines
                 // from the current buffer chunk for this command's response.
                 if (response_finished) break;

             } // End inner while (processing lines)

             // If response_finished is true here, the outer loop condition will catch it
             // and terminate after this iteration. If not, the outer loop continues recv'ing.

        } // End outer while (!response_finished)

        // If the loop finished because response_finished became true, we successfully processed
        // one command's response and are ready for the next command from stdin.
        // If the loop finished via goto, the connection was lost.

        // Any data left in response_buffer here wasn't followed by a newline
        // (i.e., a partial line). In a clean protocol, this should not happen
        // when response_finished becomes true, unless there was an issue.
        // We can print it as a warning or error indication.
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