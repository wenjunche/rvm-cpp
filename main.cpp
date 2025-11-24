#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <algorithm>
#include <iomanip>
#include <curl/curl.h>
#include <zip.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Global variable to track application start time
std::string startTime;

// Structure to hold configuration
struct Config {
    std::string version;
    std::string arguments;
};

// Structure for launch queue
struct LaunchInfo {
    std::string runtimePath;
    std::string configURL;
    std::string runtimeArgs;
    std::string runtimeVersion;
};

// Forward declarations
void logWithTimestamp(const std::string& message);
std::string getCPUArch();
Config fetchConfig(const std::string& url);
void downloadAndExtractRuntime(const std::string& downloadURL, const std::string& targetDir);
void launchApplication(const std::string& appPath, const std::string& manifestUrl, 
                       const std::string& runtimeArgs, const std::string& runtimeVersion);
void startSocketServer(const std::string& socketPath, const std::string& manifestUrl);
void handleConnection(int clientFd, const std::string& socketPath, const std::string& manifestUrl);
void processMessage(const std::string& message, const std::string& socketPath, const std::string& manifestUrl);
void processDOS(const std::string& runtimeSocketName, const std::string& messageId, 
                const std::string& socketPath, const std::string& manifestUrl);
void processRVMInfo(const std::string& runtimeSocketName, const std::string& messageId,
                    const std::string& socketPath, const std::string& manifestUrl);
void sendToRuntime(const std::string& runtimeSocketName, const json& payload, const std::string& socketPath);
std::vector<std::string> split(const std::string& str, char delimiter);
std::string trim(const std::string& str);
bool fileExists(const std::string& path);
void createDirectory(const std::string& path);

// Log with timestamp
void logWithTimestamp(const std::string& message) {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::cerr << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
}

// Get CPU architecture
std::string getCPUArch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x64"; // default
#endif
}

// Callback for CURL to write data to string
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

// Callback for CURL to write data to file
size_t writeFileCallback(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Fetch config from URL
Config fetchConfig(const std::string& url) {
    CURL* curl = curl_easy_init();
    Config config;
    
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
    }
    
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    
    if (httpCode != 200) {
        throw std::runtime_error("HTTP error: " + std::to_string(httpCode));
    }
    
    // Parse JSON
    auto jsonObj = json::parse(response);
    config.version = jsonObj["runtime"]["version"].get<std::string>();
    config.arguments = jsonObj["runtime"].value("arguments", "");
    
    return config;
}

// Extract zip file
void extractZip(const std::string& zipPath, const std::string& destDir) {
    int err = 0;
    zip* za = zip_open(zipPath.c_str(), 0, &err);
    
    if (!za) {
        throw std::runtime_error("Failed to open zip file");
    }
    
    zip_int64_t numEntries = zip_get_num_entries(za, 0);
    
    for (zip_int64_t i = 0; i < numEntries; i++) {
        struct zip_stat st;
        zip_stat_init(&st);
        zip_stat_index(za, i, 0, &st);
        
        std::string filePath = destDir + "/" + st.name;
        
        // Check if it's a directory
        if (st.name[strlen(st.name) - 1] == '/') {
            mkdir(filePath.c_str(), 0755);
            continue;
        }
        
        // Create parent directories
        size_t pos = filePath.find_last_of('/');
        if (pos != std::string::npos) {
            std::string dir = filePath.substr(0, pos);
            std::string cmd = "mkdir -p " + dir;
            system(cmd.c_str());
        }
        
        // Extract file
        zip_file* zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;
        
        FILE* outFile = fopen(filePath.c_str(), "wb");
        if (!outFile) {
            zip_fclose(zf);
            continue;
        }
        
        char buf[8192];
        zip_int64_t bytesRead;
        while ((bytesRead = zip_fread(zf, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, bytesRead, outFile);
        }
        
        fclose(outFile);
        zip_fclose(zf);
        
        // Set default permissions (readable/writable by user, readable by group/others)
        chmod(filePath.c_str(), 0644);
    }
    
    zip_close(za);
}

// Download and extract runtime
void downloadAndExtractRuntime(const std::string& downloadURL, const std::string& targetDir) {
    logWithTimestamp("Downloading runtime from: " + downloadURL);
    
    std::string tmpFile = "/tmp/openfin-runtime-" + std::to_string(time(nullptr)) + ".zip";
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }
    
    FILE* fp = fopen(tmpFile.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Failed to create temporary file");
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, downloadURL.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        unlink(tmpFile.c_str());
        throw std::runtime_error(std::string("Download failed: ") + curl_easy_strerror(res));
    }
    
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    
    if (httpCode != 200) {
        unlink(tmpFile.c_str());
        throw std::runtime_error("HTTP error: " + std::to_string(httpCode));
    }
    
    logWithTimestamp("Downloaded runtime to: " + tmpFile);
    
    // Create target directory
    std::string cmd = "mkdir -p " + targetDir;
    system(cmd.c_str());
    
    // Extract
    logWithTimestamp("Extracting runtime to: " + targetDir);
    extractZip(tmpFile, targetDir);
    
    unlink(tmpFile.c_str());
    logWithTimestamp("Successfully extracted runtime to: " + targetDir);
}

// Launch application
void launchApplication(const std::string& appPath, const std::string& manifestUrl,
                       const std::string& runtimeArgs, const std::string& runtimeVersion) {
    logWithTimestamp("Launching application: " + appPath + " with manifest URL: " + manifestUrl);
    
    if (access(appPath.c_str(), F_OK) != 0) {
        logWithTimestamp("Application not found at path: " + appPath);
        return;
    }
    
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child process
        std::vector<const char*> args;
        args.push_back(appPath.c_str());
        
        std::string configArg = "--config=" + manifestUrl;
        args.push_back(configArg.c_str());
        
        if (!runtimeArgs.empty()) {
            args.push_back(runtimeArgs.c_str());
        }
        
        args.push_back("--v=1");
        
        std::string versionArg = "--version-keyword=" + runtimeVersion;
        args.push_back(versionArg.c_str());
        
        args.push_back("--message-timeout=5000");
        args.push_back("--desktop-owner-settings-timeout=5000");
        args.push_back(nullptr);
        
        execv(appPath.c_str(), const_cast<char* const*>(args.data()));
        
        // If execv returns, it failed
        std::cerr << "Failed to execute application" << std::endl;
        exit(1);
    } else if (pid > 0) {
        logWithTimestamp("Application started with PID: " + std::to_string(pid));
    } else {
        logWithTimestamp("Failed to fork process");
    }
}

// Send to runtime socket
void sendToRuntime(const std::string& runtimeSocketName, const json& payload, const std::string& socketPath) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        logWithTimestamp("Failed to create socket");
        return;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, runtimeSocketName.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        logWithTimestamp("Failed to connect to socket: " + runtimeSocketName);
        close(sockfd);
        return;
    }
    
    logWithTimestamp("Connected to socket: " + runtimeSocketName);
    
    // Marshal JSON
    std::string jsonData = payload.dump();
    
    // Create message: socketPath:S:jsonData
    std::string message = socketPath + ":S:" + jsonData;
    
    // Send message
    ssize_t sent = send(sockfd, message.c_str(), message.length(), 0);
    if (sent < 0) {
        logWithTimestamp("Failed to send data to socket");
        close(sockfd);
        return;
    }
    
    logWithTimestamp("Sent JSON payload to socket (" + std::to_string(sent) + " bytes written)");
    logWithTimestamp("Waiting for response from socket...");
    
    // Wait for response
    char responseBuffer[1024];
    ssize_t n = recv(sockfd, responseBuffer, sizeof(responseBuffer) - 1, 0);
    
    if (n < 0) {
        logWithTimestamp("Failed to read response from socket");
        close(sockfd);
        return;
    }
    
    responseBuffer[n] = '\0';
    std::string responseStr(responseBuffer, n);
    
    logWithTimestamp("Received " + std::to_string(n) + " bytes from socket");
    logWithTimestamp("Response content: " + responseStr);
    
    if (responseStr != "RESP") {
        logWithTimestamp("Unexpected response from socket: " + responseStr + " (expected 'RESP')");
    } else {
        logWithTimestamp("Received expected response: " + responseStr);
    }
    
    close(sockfd);
}

// Process desktop owner settings request
void processDOS(const std::string& runtimeSocketName, const std::string& messageId,
                const std::string& socketPath, const std::string& manifestUrl) {
    json response = {
        {"messageId", messageId},
        {"topic", "application"},
        {"payload", {
            {"action", "get-desktop-owner-settings"},
            {"applicationSettingsExists", true},
            {"desktopOwnerFileExists", true},
            {"openfinSystemApplications", json::object()},
            {"success", true},
            {"payload", {
                {manifestUrl, {
                    {"extensions", json::array()},
                    {"licenses", json::array()},
                    {"permissions", {
                        {"System", {
                            {"launchExternalProcess", {
                                {"enabled", true},
                                {"executables", {
                                    {"enabled", true}
                                }}
                            }},
                            {"setDomainSettings", true},
                            {"resolveDomainSettings", true},
                            {"getDomainSettings", true},
                            {"getCurrentDomainSettings", true},
                            {"openUrlWithBrowser", true},
                            {"launchLogUploader", true},
                            {"downloadAsset", true},
                            {"getOSInfo", true}
                        }}
                    }}
                }}
            }}
        }}
    };
    
    sendToRuntime(runtimeSocketName, response, socketPath);
    logWithTimestamp("Created DOS response");
}

// Process RVM info request
void processRVMInfo(const std::string& runtimeSocketName, const std::string& messageId,
                    const std::string& socketPath, const std::string& manifestUrl) {
    char execPathBuf[1024];
    ssize_t len = readlink("/proc/self/exe", execPathBuf, sizeof(execPathBuf) - 1);
    
    std::string execPath = "/usr/bin/rvm-cpp"; // fallback
    if (len != -1) {
        execPathBuf[len] = '\0';
        execPath = execPathBuf;
    }
    
    size_t pos = execPath.find_last_of('/');
    std::string execDir = (pos != std::string::npos) ? execPath.substr(0, pos) : ".";
    std::string rvmPath = execDir + "/rvm-cpp";
    
    json response = {
        {"broadcast", false},
        {"messageId", messageId},
        {"topic", "system"},
        {"payload", {
            {"action", "get-rvm-info"},
            {"osArch", getCPUArch()},
            {"path", rvmPath},
            {"rvmArch", getCPUArch()},
            {"start-time", startTime},
            {"version", "1.0.0.0"},
            {"working-dir", execDir}
        }}
    };
    
    sendToRuntime(runtimeSocketName, response, socketPath);
    logWithTimestamp("Created RVMInfo response");
}

// Process incoming message
void processMessage(const std::string& message, const std::string& socketPath, const std::string& manifestUrl) {
    size_t pos = message.find(":S:");
    if (pos == std::string::npos) {
        logWithTimestamp("Invalid message format: expected 'messageId:S:jsonString'");
        return;
    }
    
    std::string runtimeSocketName = message.substr(0, pos);
    std::string jsonString = message.substr(pos + 3);
    
    try {
        auto jsonObj = json::parse(jsonString);
        
        std::string topic = jsonObj.value("topic", "");
        std::string messageId = jsonObj.value("messageId", "");
        std::string action = jsonObj["payload"].value("action", "");
        
        logWithTimestamp("Runtime Scoket Name: " + runtimeSocketName);
        logWithTimestamp("Topic: " + topic);
        logWithTimestamp("Payload Message ID: " + messageId);
        logWithTimestamp("Action: " + action);
        
        if (action == "get-desktop-owner-settings") {
            processDOS(runtimeSocketName, messageId, socketPath, manifestUrl);
        } else if (action == "get-rvm-info") {
            processRVMInfo(runtimeSocketName, messageId, socketPath, manifestUrl);
        }
    } catch (const std::exception& e) {
        logWithTimestamp("Failed to parse JSON: " + std::string(e.what()));
    }
}

// Handle connection
void handleConnection(int clientFd, const std::string& socketPath, const std::string& manifestUrl) {
    char buffer[1024 * 1024];
    
    ssize_t n = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        if (n < 0) {
            logWithTimestamp("Error reading from connection");
        }
        logWithTimestamp("Connection closed");
        close(clientFd);
        return;
    }
    
    buffer[n] = '\0';
    std::string message(buffer, n);
    logWithTimestamp("Received message: " + message);
    
    // Send acknowledgment
    const char* response = "RESP";
    ssize_t sent = send(clientFd, response, strlen(response), 0);
    if (sent < 0) {
        logWithTimestamp("Error writing response");
        close(clientFd);
        return;
    }
    
    logWithTimestamp("Sent response: RESP (" + std::to_string(sent) + " bytes written)");
    
    // Process message
    processMessage(message, socketPath, manifestUrl);
    
    close(clientFd);
}

// Start socket server
void startSocketServer(const std::string& socketPath, const std::string& manifestUrl) {
    // Remove existing socket
    unlink(socketPath.c_str());
    
    int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0) {
        logWithTimestamp("Failed to create socket");
        return;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        logWithTimestamp("Failed to bind socket");
        close(serverFd);
        return;
    }
    
    if (listen(serverFd, 50) < 0) {
        logWithTimestamp("Failed to listen on socket");
        close(serverFd);
        return;
    }
    
    logWithTimestamp("Socket server listening on: " + socketPath);
    
    while (true) {
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd < 0) {
            logWithTimestamp("Error accepting connection");
            continue;
        }
        
        logWithTimestamp("New connection established");
        
        // Handle in separate thread
        std::thread(handleConnection, clientFd, socketPath, manifestUrl).detach();
    }
    
    close(serverFd);
    unlink(socketPath.c_str());
}

// Utility functions
std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    
    return tokens;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

bool fileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

// Main function
int main(int argc, char* argv[]) {
    // Record start time
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    startTime = oss.str();
    
    // Initialize CURL
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Parse arguments
    std::string configURLs;
    std::string runtimeDir;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--config=") == 0) {
            configURLs = arg.substr(9);
        } else if (arg.find("--runtime-dir=") == 0) {
            runtimeDir = arg.substr(14);
        }
    }
    
    if (configURLs.empty()) {
        std::cerr << "Error: --config parameter is required" << std::endl;
        std::cerr << "Usage: rvm-cpp --config=<URL1>,<URL2>,... --runtime-dir=<directory>" << std::endl;
        return 1;
    }
    
    if (runtimeDir.empty()) {
        std::cerr << "Error: --runtime-dir parameter is required" << std::endl;
        std::cerr << "Usage: rvm-cpp --config=<URL1>,<URL2>,... --runtime-dir=<directory>" << std::endl;
        return 1;
    }
    
    // Split config URLs
    auto configURLList = split(configURLs, ',');
    std::vector<LaunchInfo> launchQueue;
    
    // Process each config URL
    for (const auto& configURL : configURLList) {
        std::string url = trim(configURL);
        if (url.empty()) continue;
        
        try {
            // Fetch config
            Config config = fetchConfig(url);
            
            if (config.version.empty()) {
                logWithTimestamp("Error: runtime.version not found in config from " + url);
                continue;
            }
            
            logWithTimestamp("Runtime version: " + config.version + " for config: " + url);
            
            // Build runtime path
            std::string runtimePath = runtimeDir + "/" + config.version + "/openfin";
            
            // Check if runtime exists
            if (!fileExists(runtimePath)) {
                logWithTimestamp("Runtime not found at path: " + runtimePath);
                
                std::string cpuArch = getCPUArch();
                logWithTimestamp("Detected CPU architecture: " + cpuArch);
                
                std::string downloadURL = "https://cdn.openfin.co/release/runtime/linux/" + 
                                         cpuArch + "/" + config.version;
                
                std::string targetDir = runtimeDir + "/" + config.version;
                
                try {
                    downloadAndExtractRuntime(downloadURL, targetDir);
                } catch (const std::exception& e) {
                    logWithTimestamp("Failed to download and extract runtime: " + std::string(e.what()));
                    continue;
                }
                
                if (!fileExists(runtimePath)) {
                    logWithTimestamp("Runtime still not found after extraction at path: " + runtimePath);
                    continue;
                }
            }
            
            logWithTimestamp("Runtime ready at path: " + runtimePath);
            
            // Add to launch queue
            launchQueue.push_back({runtimePath, url, config.arguments, config.version});
            
        } catch (const std::exception& e) {
            logWithTimestamp("Error fetching config from " + url + ": " + e.what());
            continue;
        }
    }
    
    // Launch all applications
    logWithTimestamp("All runtimes downloaded. Launching " + std::to_string(launchQueue.size()) + " application(s)...");
    
    // for (const auto& info : launchQueue) {
    //     std::thread(launchApplication, info.runtimePath, info.configURL, 
    //                info.runtimeArgs, info.runtimeVersion).detach();
    // }
    
    // Start socket server
    std::string socketPath = "/tmp/OpenFinRVM_Messaging";
    std::string firstConfigURL = trim(configURLList[0]);
    startSocketServer(socketPath, firstConfigURL);
    
    curl_global_cleanup();
    
    return 0;
}
