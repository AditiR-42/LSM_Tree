#include <iostream>
#include <string>
#include <cstring> // For memset
#include <unistd.h> // For close
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> // For inet_pton
#include <thread>       // For std::this_thread
#include <chrono>       // For std::chrono::milliseconds
#include "../server/server.h" // Include the Server class header

// Default port number (can be overridden with command-line argument)
const int DEFAULT_PORT = 12345;

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    std::string data_dir = "data/"; // Default data directory

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            try {
                port = std::stoi(argv[i + 1]);
            } catch (const std::invalid_argument& e) {
                std::cerr << "Invalid port number: " << argv[i + 1] << std::endl;
                return 1; // Indicate an error
            }
            ++i; // Skip the value
        } else if (std::strcmp(argv[i], "--data_dir") == 0 && i + 1 < argc) {
            data_dir = argv[i + 1];
            ++i;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            return 1;
        }
    }

    // Create a Server instance (pass data_dir to the constructor)
    // kvstore::Server server(port, data_dir);

    // Initialize and start the server
    // if (!server.start()) {
    //     std::cerr << "Failed to start the server." << std::endl;
    //     return 1;
    // }

    // Server is now running.  Typically, you'd have a loop here to handle
    // signals (e.g., SIGINT to shut down gracefully), but for this basic
    // example, we'll just let the server run until the user kills the process.
    std::cout << "Server listening on port " << port << " (data directory: " << data_dir << ")" << std::endl;

    // Keep the server running (you'll need to handle shutdown properly later)
    while (true) {
        // Do nothing, let the server run
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // In a real application, you'd handle shutdown gracefully here (e.g.,
    // closing the server socket, flushing the Memtable, etc.).  For now, this
    // is just a placeholder.

    return 0;
}