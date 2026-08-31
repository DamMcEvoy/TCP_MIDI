// Enhanced midiHandler.cpp with explicit shutdown and safety logs

#include <unistd.h>  // for STDIN_FILENO used in select call
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <string>
#include <limits>
#include <atomic>
#include <rtmidi/RtMidi.h>
#include "TcpConnection.h"

// Shared MIDI output message queue and synchronization
std::queue<libremidi::midi_bytes> midiOutputQueue;
std::mutex outputMutex;
std::condition_variable outputCv;

std::atomic<bool> running(true);

// MIDI output thread function: sends MIDI messages from queue to MIDI output port
void midiOutputThreadFunc(RtMidiOut* midiOut) {
    try {
        while (running.load()) {
            std::unique_lock<std::mutex> lock(outputMutex);
            outputCv.wait(lock, [] { return !midiOutputQueue.empty() || !running.load(); });

            while (!midiOutputQueue.empty()) {
                auto msg = midiOutputQueue.front();
                midiOutputQueue.pop();
                lock.unlock();
                try {
                    midiOut->sendMessage(&msg);
                } catch (RtMidiError& e) {
                    std::cerr << "[midiOutputThreadFunc] MIDI send error: " << e.getMessage() << std::endl;
                }
                lock.lock();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[midiOutputThreadFunc] Exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[midiOutputThreadFunc] Unknown exception caught" << std::endl;
    }
}

// Callback inserted by TcpConnection to send MIDI messages from network to output queue
void onMidiReceivedFromNetwork(const libremidi::midi_bytes& midiMessage) {
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        midiOutputQueue.push(midiMessage);
    }
    outputCv.notify_one();
}

// Check stdin readiness with select() without blocking
bool isStdinReady() {
    fd_set set;
    struct timeval timeout = {0, 100000}; // 100 ms timeout
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);

    // Return true if input is ready
    return select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout) > 0;
}

// Main handler: opens MIDI ports, manages threads and synchronizes shutdown cleanly
int runMidiHandler(TcpConnection* tcpConnection) {
    tcpConnection->registerMidiReceiverCallback(onMidiReceivedFromNetwork);

    RtMidiIn midiIn;
    RtMidiOut midiOut;

    try {
        unsigned int nInputPorts = midiIn.getPortCount();
        unsigned int nOutputPorts = midiOut.getPortCount();

        if (nInputPorts == 0 || nOutputPorts == 0) {
            std::cerr << "[runMidiHandler] No MIDI input or output ports available." << std::endl;
            return 1;
        }

        std::cout << "Available MIDI Input Ports:" << std::endl;
        for (unsigned int i = 0; i < nInputPorts; ++i)
            std::cout << "  [" << i << "] " << midiIn.getPortName(i) << std::endl;

        std::cout << "Available MIDI Output Ports:" << std::endl;
        for (unsigned int i = 0; i < nOutputPorts; ++i)
            std::cout << "  [" << i << "] " << midiOut.getPortName(i) << std::endl;

        int chosenInput = -1;
        int chosenOutput = -1;

        std::cout << "Enter the number of the MIDI Input Port you want to open: ";
        std::cin >> chosenInput;
        if (chosenInput < 0 || chosenInput >= static_cast<int>(nInputPorts)) {
            std::cerr << "[runMidiHandler] Invalid input port number." << std::endl;
            return 1;
        }
        midiIn.openPort(chosenInput);

        midiIn.setCallback([](double, libremidi::midi_bytes* message, void* userData) {
            TcpConnection* tcpConn = static_cast<TcpConnection*>(userData);
            if (message && tcpConn && tcpConn->isConnected()) {
                tcpConn->sendMidiMessage(*message);
            }
        }, tcpConnection);

        midiIn.ignoreTypes(false, false, false);

        std::cout << "Enter the number of the MIDI Output Port you want to open: ";
        std::cin >> chosenOutput;
        if (chosenOutput < 0 || chosenOutput >= static_cast<int>(nOutputPorts)) {
            std::cerr << "[runMidiHandler] Invalid output port number." << std::endl;
            return 1;
        }
        midiOut.openPort(chosenOutput);

        // Spawn MIDI output thread
        std::thread outputThread(midiOutputThreadFunc, &midiOut);

        std::cout << "MIDI streaming started. Press Enter to quit." << std::endl;

        // Ensure input buffer clean before wait
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        // Poll for Enter key press without blocking forever
        while (!isStdinReady()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Consume the Enter line
        std::string dummy;
        std::getline(std::cin, dummy);

        std::cout << "[runMidiHandler] Shutdown signal received..." << std::endl;

        // Important: cancel callback before stopping threads to avoid use-after-free
        midiIn.cancelCallback();

        // Signal output thread to exit
        running = false;
        outputCv.notify_all();

        // Join output thread for clean exit
        if (outputThread.joinable())
            outputThread.join();

        // Close MIDI ports
        midiIn.closePort();
        midiOut.closePort();

        std::cout << "[runMidiHandler] MIDI ports closed, exiting." << std::endl;

        return 0;

    } catch (RtMidiError& e) {
        std::cerr << "[runMidiHandler] RtMidi error: " << e.getMessage() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "[runMidiHandler] Unknown exception caught." << std::endl;
        return 1;
    }
}
