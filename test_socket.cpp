#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Thread function to handle client connection
void handleClient(int server_fd, const char* test_socket_path) {
    // Step 2: Accept connection to /tmp/test_socket
    std::cout << "[Step 2] Waiting for connection..." << std::endl;
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        std::cerr << "Failed to accept" << std::endl;
        return;
    }
    
    std::cout << "Client connected to " << test_socket_path << std::endl;
    
    // Step 3: Read some data from the connection
    std::cout << "[Step 3] Reading data from connection..." << std::endl;
    char buffer[1024];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        std::cout << "Received " << n << " bytes: " << buffer << std::endl;
        
        // Step 4: Send "RESP" to the connection
        std::cout << "[Step 4] Sending RESP response..." << std::endl;
        const char* response = "RESP";
        ssize_t sent = send(client_fd, response, strlen(response), 0);
        std::cout << "Sent " << sent << " bytes: " << response << std::endl;
    } else {
        std::cerr << "Failed to receive data" << std::endl;
        close(client_fd);
        return;
    }
    
    close(client_fd);
}

int main() {
    const char* test_socket_path = "/tmp/test_socket";
    const char* rvm_socket_path = "/tmp/OpenFinRVM_Messaging";
    
    // Step 1: Listen to a socket "/tmp/test_socket"
    std::cout << "[Step 1] Setting up listener on " << test_socket_path << std::endl;
    
    // Remove existing socket file
    unlink(test_socket_path);
    
    // Create socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }
    
    // Bind to path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, test_socket_path, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        close(server_fd);
        return 1;
    }
    
    // Listen
    if (listen(server_fd, 1) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(server_fd);
        return 1;
    }
    
    std::cout << "Listening on " << test_socket_path << std::endl;
    
    // Start thread to handle client connection
    std::thread clientThread(handleClient, server_fd, test_socket_path);
        
    // Step 5: Connect to /tmp/OpenFinRVM_Messaging
    std::cout << "[Step 5] Connecting to " << rvm_socket_path << "..." << std::endl;
    
    int rvm_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (rvm_fd < 0) {
        std::cerr << "Failed to create socket for RVM connection" << std::endl;
        return 1;
    }
    
    struct sockaddr_un rvm_addr;
    memset(&rvm_addr, 0, sizeof(rvm_addr));
    rvm_addr.sun_family = AF_UNIX;
    strncpy(rvm_addr.sun_path, rvm_socket_path, sizeof(rvm_addr.sun_path) - 1);
    
    if (connect(rvm_fd, (struct sockaddr*)&rvm_addr, sizeof(rvm_addr)) < 0) {
        std::cerr << "Failed to connect to " << rvm_socket_path << std::endl;
        std::cerr << "Make sure rvm-cpp is running!" << std::endl;
        close(rvm_fd);
        return 1;
    }
    
    std::cout << "Connected to " << rvm_socket_path << std::endl;
    
    // Step 6: Create a json string for "get-rvm-info"
    std::cout << "[Step 6] Creating get-rvm-info JSON message..." << std::endl;
    
    json payload = {
        {"topic", "system"},
        {"messageId", "test-message-123"},
        {"payload", {
            {"action", "get-rvm-info"}
        }}
    };
    
    std::string jsonString = payload.dump();
    std::cout << "JSON payload: " << jsonString << std::endl;
    
    // Create message in format: socketPath:S:jsonData
    std::string message = std::string(test_socket_path) + ":S:" + jsonString;
    std::cout << "Full message: " << message << std::endl;
    
    // Step 7: Send get-rvm-info to /tmp/OpenFinRVM_Messaging
    std::cout << "[Step 7] Sending message to RVM..." << std::endl;
    ssize_t sent = send(rvm_fd, message.c_str(), message.length(), 0);
    if (sent < 0) {
        std::cerr << "Failed to send message to RVM" << std::endl;
        close(rvm_fd);
        return 1;
    }
    
    std::cout << "Sent " << sent << " bytes to RVM" << std::endl;
    
    // Step 8: Read "RESP" as confirmation response
    std::cout << "[Step 8] Waiting for RESP confirmation..." << std::endl;
    char resp_buffer[1024];
    ssize_t resp_n = recv(rvm_fd, resp_buffer, sizeof(resp_buffer) - 1, 0);
    if (resp_n > 0) {
        resp_buffer[resp_n] = '\0';
        std::cout << "Received " << resp_n << " bytes: " << resp_buffer << std::endl;
        
        if (strcmp(resp_buffer, "RESP") == 0) {
            std::cout << "✓ Received expected RESP confirmation" << std::endl;
        } else {
            std::cout << "✗ Unexpected response (expected 'RESP')" << std::endl;
        }
    } else {
        std::cerr << "Failed to receive response from RVM" << std::endl;
        close(rvm_fd);
        return 1;
    }
    
    close(rvm_fd);
        
    // Wait for client thread to finish
    clientThread.join();
    
    // Clean up
    close(server_fd);
    unlink(test_socket_path);
    
    return 0;
}
