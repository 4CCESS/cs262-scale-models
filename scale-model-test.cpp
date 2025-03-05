// scale-model-test.cpp
//g++ -std=c++17 -Wall -o sm-test scale-model-test.cpp scale-model.cpp -lCatch2Main -lCatch2
// *requires catch2 install

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>

// Declarations for the functions we want to test from your code.
// Adjust these declarations if your functions are in a namespace or class.
extern int setNonBlocking(int fd);
extern std::ofstream openLogFile(int processId);
extern void enterLog(std::ofstream &logFile, const std::string &entry);
extern void handshakePhase(int id, std::vector<int>& peers, std::ofstream &logFile);

using namespace std;
using namespace std::chrono;

TEST_CASE("setNonBlocking sets O_NONBLOCK flag", "[setNonBlocking]") {
    int fds[2];
    // Create a pipe.
    REQUIRE(pipe(fds) == 0);
    int ret = setNonBlocking(fds[0]);
    // Check that the call succeeded.
    REQUIRE(ret >= 0);
    // Retrieve flags and check that O_NONBLOCK is set.
    int flags = fcntl(fds[0], F_GETFL, 0);
    REQUIRE(flags & O_NONBLOCK);
    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("openLogFile and enterLog work correctly", "[logging]") {
    int testProcessId = 42;
    // Open a log file (should be cleared due to trunc mode)
    std::ofstream logFile = openLogFile(testProcessId);
    REQUIRE(logFile.is_open());
    // Write a test log entry.
    enterLog(logFile, "Test log entry.");
    logFile.close();
    
    // Read back the file to check that the entry was written.
    std::ifstream inFile("log_proc_42.txt");
    REQUIRE(inFile.is_open());
    std::string line;
    bool found = false;
    while(getline(inFile, line)) {
        if(line.find("Test log entry.") != string::npos) {
            found = true;
            break;
        }
    }
    inFile.close();
    REQUIRE(found == true);
    // Clean up the test log file.
    remove("log_proc_42.txt");
}

TEST_CASE("handshakePhase receives handshake message", "[handshake]") {
    // Create a socket pair to simulate a connection.
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    
    // Use sv[0] as the connection for handshakePhase.
    std::vector<int> peers = { sv[0] };
    
    // Open a temporary log file.
    std::ofstream logFile = openLogFile(0);
    
    // In a separate thread, simulate sending a handshake message on sv[1].
    std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Give handshakePhase time to wait.
        std::string handshakeMsg = "handshake from test";
        write(sv[1], handshakeMsg.c_str(), handshakeMsg.size());
    });
    
    // Call handshakePhase; it should receive the handshake message.
    handshakePhase(0, peers, logFile);
    
    writer.join();
    logFile.close();
    
    // Read back the log file to confirm a handshake message was received.
    std::ifstream inFile("log_proc_0.txt");
    REQUIRE(inFile.is_open());
    std::string logContent((std::istreambuf_iterator<char>(inFile)),
                           std::istreambuf_iterator<char>());
    inFile.close();
    // Verify that the log contains a handshake confirmation.
    REQUIRE(logContent.find("received handshake") != std::string::npos);
    
    // Clean up.
    remove("log_proc_0.txt");
    close(sv[0]);
    close(sv[1]);
}
