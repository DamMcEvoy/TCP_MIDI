// main.cpp
#include "TcpConnection.h"
//#include "midiHandler.cpp"
#include <iostream>
#include <string>

int runMidiHandler(TcpConnection* tcpConnection);

int main() {
    TcpConnection client("20.13.138.208", 443);

    if (!client.connectToServer()) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }

    std::string clientID;
    if (!client.getClientID(clientID)) {
        std::cerr << "Failed to get Client ID" << std::endl;
        client.disconnect();
        return 1;
    }

    std::cout << "Client ID: " << clientID << std::endl;

    int result = runMidiHandler(&client);

    client.stopReceiveLoop();
    client.disconnect();

    return result;
}
