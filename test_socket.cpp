#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main() {
    const char* socket_path = "/tmp/test_socket";
    
    // Remove existing socket file
    unlink(socket_path);
    
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
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
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
    
    std::cout << "Listening on " << socket_path << std::endl;
    std::cout << "Test with: echo 'test message' | nc -U " << socket_path << std::endl;
    
    // Accept connection
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        std::cerr << "Failed to accept" << std::endl;
        close(server_fd);
        return 1;
    }
    
    std::cout << "Client connected" << std::endl;
    
    // Read data
    char buffer[1024];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        std::cout << "Received: " << buffer << std::endl;
        
        // Send response
        const char* response = "RESP";
        ssize_t sent = send(client_fd, response, strlen(response), 0);
        std::cout << "Sent " << sent << " bytes: " << response << std::endl;
        
        // Small delay before closing
        usleep(100000); // 100ms
    } else {
        std::cerr << "Failed to receive data" << std::endl;
    }
    
    close(client_fd);
    close(server_fd);
    unlink(socket_path);
    
    return 0;
}
