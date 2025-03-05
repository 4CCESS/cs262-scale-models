// scale-model.cpp
// g++ -std=c++17 -Wall -o scale-model scale-model.cpp


#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cstdlib>
#include <queue>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <sys/wait.h>
#include <ctime>
#include <fstream>
#include <random>
#include <mutex>
#include <thread>
#include <chrono>

using namespace std;
using namespace std::chrono;

#define BUFFER_SIZE 256
#define OPERATION_TIME 60
#define NUM_VMS 3


// Message queue for each process
queue<string> messageQueue;
mutex queueMutex;

// Helper function: set a file descriptor to non-blocking mode.
int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Open a log file for the process.
std::ofstream openLogFile(int processId) {
    std::string logFilename = "log_proc_" + to_string(processId) + ".txt";
    std::ofstream logFile(logFilename, std::ios::out | std::ios::trunc);
    if (!logFile.is_open()) {
         std::cerr << "Unable to open log file: " << logFilename << std::endl;
         exit(EXIT_FAILURE);
    }
    return logFile;
}

// Write an entry to the log file with a timestamp.
void enterLog(std::ofstream &logFile, const std::string &entry) {
    time_t now = time(NULL);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    logFile << "[" << timeStr << "] " << entry << "\n";
    logFile.flush();
}

// Verify all connections are writeable/receivable to end initialization
void handshakePhase(int id, vector<int>& peers, std::ofstream &logFile) {
    enterLog(logFile, "------------------------------------------------------------- ");
    // Build a handshake message.
    string handshakeMsg = "FROM PID=" + to_string(id);
    
    // Send the handshake message on each peer connection.
    for (int fd : peers) {
        if (write(fd, handshakeMsg.c_str(), handshakeMsg.size()) < 0) {
            perror("write handshake");
        } else {
            enterLog(logFile, "PID: " + to_string(id) + " | HANDSHAKE SENT | FD:  " + to_string(fd));
        }
    }
    
    // Now wait to receive handshake messages from each peer.
    int expected = peers.size(); // one handshake from each peer
    int received = 0;
    auto start = steady_clock::now();
    
    // All handshakes must be received before 5-second timeout
    while (received < expected && duration_cast<seconds>(steady_clock::now() - start).count() < 5) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = 0;
        for (int fd : peers) {
            FD_SET(fd, &readfds);
            if (fd > max_fd)
                max_fd = fd;
        }
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            for (int fd : peers) {
                if (FD_ISSET(fd, &readfds)) {
                    char buffer[BUFFER_SIZE];
                    memset(buffer, 0, BUFFER_SIZE);
                    int bytesRead = read(fd, buffer, BUFFER_SIZE - 1);
                    if (bytesRead > 0) {
                        string msg(buffer, bytesRead);
                        // Check if the message is a handshake message.
                        if (msg.find("FROM PID") != string::npos) {
                            enterLog(logFile, "PID: " + to_string(id) + " | HANDSHAKE RECEIVED | " + msg);
                            received++;
                        }
                    }
                }
            }
        }
    }

    // Error if not all handshakes received
    if (received < expected) {
        enterLog(logFile, "PID: " + to_string(id) + " | NOT ALL HANDSHAKES RECEIVED (" +
                 to_string(received) + " of " + to_string(expected) + ")  ----- ABORTING");
        exit(EXIT_FAILURE);
       
    // Handshake complete and verified
    } else {
        enterLog(logFile, "PID: " + to_string(id) + " | HANDSHAKE PHASE COMPLETE");
        enterLog(logFile, "------------------------------------------------------------- ");
    }
}

// Listen on sockets and add messages to queue (not restricted by clock-rate)
void listenerThread(int server_fd, vector<int>& peers, std::ofstream &logFile, int id) {
    
    // Use select() to accept incoming connections and read messages.
    
    while(true) {  
        fd_set readfds;
        FD_ZERO(&readfds);
    
        int max_fd = server_fd;
        for (int fd : peers) {
            FD_SET(fd, &readfds);
            if (fd > max_fd) max_fd = fd;
        }
        
        // Set a timeout for select.
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            for (int fd : peers) {
                if (FD_ISSET(fd, &readfds)) {
                    //enterLog(logFile, "[DEBUG] Process " + to_string(id) + " fd " + to_string(fd) + " ready for reading.");
                    char buffer[BUFFER_SIZE];
                    memset(buffer, 0, BUFFER_SIZE);
                    int bytesRead = read(fd, buffer, BUFFER_SIZE - 1);
                    if (bytesRead > 0) {
                        string msg(buffer, bytesRead);
                        // Log the received message.
                        //enterLog(logFile, "[DEBUG] Process " + to_string(id) + " received: " + msg);
                        {
                            lock_guard<mutex> lock(queueMutex);
                            messageQueue.push(msg);
                        }
                    } else if (bytesRead == 0) {
                        //enterLog(logFile, "[DEBUG] Process " + to_string(id) + " fd " + to_string(fd) + " read 0 bytes (connection closed).");
                        // Optionally, you might want to remove this fd from peers.
                    }                    
                }
            }
        }
    }
}

// Read messages or send message/internal event if none to be read, update logical clock (bound by clock rate)
void operationsRun(int id, int clock_rate, vector<int>& peers, std::ofstream &logFile){

    int logical_clock = 0;
    
    // Random generator for operations selection (1-10)
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> opDist(1, 5); // 1-10 standard, 1-5 low probability

    // Calculate time duration per operation
    auto opDuration = duration<double>(1.0 / clock_rate);

    // Run operations loop for operation time 
    auto startTime = steady_clock::now();
    while (duration_cast<seconds>(steady_clock::now() - startTime).count() < OPERATION_TIME){
        auto tickStart =  steady_clock::now();
        bool processedMessage = false;
        time_t sysTime = time(NULL);

        // Check for a message in the message queue
        {
            lock_guard<mutex> lock(queueMutex);
            if(!messageQueue.empty()){
                // Grab message from queue
                string msg = messageQueue.front();
                messageQueue.pop();

                // Parse mesasge in format "clock - message"
                size_t dashPos = msg.find(" - ");
                if (dashPos != string::npos){
                    string clockStr = msg.substr(0, dashPos);
                    int sender_clock = std::stoi(clockStr);
                    string message = msg.substr(dashPos + 3);

                    int old_clock = logical_clock;

                    // Clock update: new clock = max(local, sender_clock) + 1
                    logical_clock = std::max(logical_clock, sender_clock) + 1;

                    if (logical_clock > old_clock + 1){
                        enterLog(logFile, "..................................................");
                    }

                    // Log message and updated clock
                    int queueLength = messageQueue.size();
                    enterLog(logFile, "PID: " + std::to_string(id) + 
                        " | ULC = " + std::to_string(logical_clock) + " | MESSAGE RECEIVED | SEND CLOCK = " + 
                        clockStr + " | MESSAGE = \"" + message + "\"" +
                        " | SYS TIME = " + std::to_string(sysTime) +
                        " | QUEUE LENGTH = " + std::to_string(queueLength) );
                    processedMessage = true;
                } else {
                    // Log messages with improper format
                    enterLog(logFile, "PID: " + std::to_string(id) + " | IMPROPERLY FORMATTED MESSAGE: " + msg);
                }
            } 
        }
        if (!processedMessage){

            // Generate random op code
            int op = opDist(gen);

            
            if (op == 1 || op == 2) {
                // Send direct messages

                std::string sendContent;
                int target;
                if (op == 1){
                    target = 0;
                    sendContent = "A-";
                } else{
                    target = 1;
                    sendContent  = "B-";
                }
                sendContent = sendContent + to_string(id);

                logical_clock++;  // Increment clock before sending.
                
                std::string sendMsg = std::to_string(logical_clock) + " - " + sendContent;
                if (write(peers[target], sendMsg.c_str(), sendMsg.size()) < 0) {
                    perror("write");
                }
                enterLog(logFile, "PID: " + std::to_string(id) + 
                         " | ULC = " + std::to_string(logical_clock) + " | SENT DIRECT: " +
                         sendContent + " | SYS TIME = " + std::to_string(sysTime) );

            } else if (op == 3){
                // Send message to both machine B and machine C

                logical_clock++;
                string sendContent = "Z-" + to_string(id);
                string sendMsg = to_string(logical_clock) + " - " + sendContent;
                if (write(peers[0], sendMsg.c_str(), sendMsg.size()) < 0) {
                    perror("write");
                }
                if (write(peers[1], sendMsg.c_str(), sendMsg.size()) < 0) {
                    perror("write");
                }
                enterLog(logFile, "PID: " + to_string(id) + 
                         " | ULC = " + to_string(logical_clock) + " | SENT BROADCAST: " +
                         sendContent + " | SYS TIME = " + to_string(sysTime) );
            } else {
                // Internal event (or if op != 1,2,3)
                logical_clock++;
                enterLog(logFile, "PID: " + to_string(id) + 
                         " | ULC = " + to_string(logical_clock) + " | INTERNAL EVENT | SYS TIME = " +
                         to_string(sysTime) );
            }
            
        }

        // Sleep for the remainder of the tick.
        auto tickEnd = steady_clock::now();
        auto elapsed = tickEnd - tickStart;
        if (elapsed < opDuration) {
            this_thread::sleep_for(opDuration - elapsed);
        }
    }
}

int runProc(int id) {
    // Open log file for this process.
    std::ofstream logFile = openLogFile(id);
    enterLog(logFile, "Process " + to_string(id) + " started.");

    // Create a UNIX domain socket for listening.
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    // Prepare the socket address (unique per process).
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    string socketPath = "/tmp/smproc_" + to_string(id);
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    // Remove any existing socket file
    unlink(socketPath.c_str());
    
    // Bind address and socket fd
    if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    // Begin listening
    if(listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    // Set the server socket to non-blocking mode
    if (setNonBlocking(server_fd) < 0) {
        perror("setNonBlocking");
        exit(EXIT_FAILURE);
    }
    
    // Allow time for all processes to create and bind their server sockets.
    sleep(1);
    
    // Send connection to all processes with higher IDs
    vector<int> peers;
    for (int j = 0; j < NUM_VMS; j++) {
        if(j == id) continue;
        if (id < j){
            int peer_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if(peer_fd < 0) {
                perror("socket (client)");
                continue;
            }
            struct sockaddr_un clientAddr;
            memset(&clientAddr, 0, sizeof(clientAddr));
            clientAddr.sun_family = AF_UNIX;
            string otherSocketPath = "/tmp/smproc_" + to_string(j);
            strncpy(clientAddr.sun_path, otherSocketPath.c_str(), sizeof(clientAddr.sun_path) - 1);
            
            // Retry connecting a few times if needed.
            int retries = 3;
            while(connect(peer_fd, (struct sockaddr*)&clientAddr, sizeof(clientAddr)) < 0 && retries > 0) {
                enterLog(logFile, "Process " + to_string(id) + " attempting connection to " + otherSocketPath);
                sleep(1);
                retries--;
            }
            if(retries == 0) {
                cerr << "Process " << id << " failed to connect to " << otherSocketPath << endl;
                close(peer_fd);
                continue;
            }
            enterLog(logFile, "Process " + to_string(id) + " connected to " + otherSocketPath + " with fd = " + to_string(peer_fd));
            peers.push_back(peer_fd);
        }

    }

    // Accept incoming connections from processes with lower IDs.
    // Each process with id > remote will accept a connection from that remote.
    // Processes 0..(id-1) will connect to this process.
    auto acceptStart = steady_clock::now();
    while (peers.size() < (size_t)(NUM_VMS - 1) && duration_cast<seconds>(steady_clock::now() - acceptStart).count() < 5) {
        int conn_fd = accept(server_fd, NULL, NULL);
        if (conn_fd >= 0) {
            setNonBlocking(conn_fd);
            enterLog(logFile, "Process " + to_string(id) + " accepted connection with fd = " + to_string(conn_fd));
            peers.push_back(conn_fd);
        }
    }
    enterLog(logFile, "------------------------------------------------------------- ");

    // Log peer list
    string peerList = "Peers for process " + to_string(id) + ": ";
    for (int fd : peers) {
        peerList += to_string(fd) + " ";
    }
    enterLog(logFile, peerList);
   

    sleep(2);

    // Conduct connection verification
    handshakePhase(id, peers, logFile);

    // Determine clock rate
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<std::mt19937::result_type> dist6(1,3); //1-6 standard, 1-3 low variation

    int clock_rate = dist6(gen);
    enterLog(logFile, "PID: " + to_string(id) + " | INITIALIZATION COMPLETE | CLOCK RATE = " + to_string(clock_rate));
    enterLog(logFile, "------------------------------------------------------------- ");
    
    // Run the listener function that queues messages and logs them.
    thread listener(listenerThread, server_fd, ref(peers), ref(logFile), id);

    // Run operations function that formally receives and sends messages
    operationsRun(id, clock_rate, peers, logFile);

    listener.detach();

    // Clean up client sockets.
    for(auto fd : peers) {
        close(fd);
    }
    close(server_fd);
    unlink(socketPath.c_str());
    enterLog(logFile, "------------------------------------------------------------- ");
    enterLog(logFile, "Process " + to_string(id) + " terminating.");
    logFile.close();
    
    exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[]) {

    
    vector<pid_t> children;
    
    // Fork the specified number of processes.
    for (int i = 0; i < NUM_VMS; i++) {
        pid_t pid = fork();
        if(pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if(pid == 0) { // Child process (loop index = id)
            runProc(i);
        } else {
            // In the parent process, record the child's PID.
            children.push_back(pid);
        }
    }
    
    // Parent process waits for all children to finish.
    for(auto pid : children) {
        waitpid(pid, NULL, 0);
    }
    
    return 0;
}
