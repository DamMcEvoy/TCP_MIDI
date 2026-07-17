#include "GuiApp.h"
#include "AppController.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

GuiApp::GuiApp(AppController& app)
    : app_(app) {}

GuiApp::~GuiApp() {
    shutdown();
}

bool GuiApp::initialize() {
    if (initialized_) {
        return true;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL_Init error: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    window_ = SDL_CreateWindow(
        "TCP MIDI",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        602,
        668,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window_) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        std::cerr << "Failed to create OpenGL context: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window_, glContext_);
    ImGui_ImplOpenGL3_Init("#version 410");

    inputPorts_ = app_.getInputPortNames();
    outputPorts_ = app_.getOutputPortNames();

    initialized_ = true;
    statusMessage_ = "GUI initialized.";
    return true;
}

void GuiApp::shutdown() {
    if (!initialized_) {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (glContext_) {
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
    initialized_ = false;
}

int GuiApp::run() {
    if (!initialize()) {
        return 1;
    }

    running_ = true;
    SDL_Event event;
    while (running_) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                running_ = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        render();

        ImGui::Render();

        int fbWidth = 0;
        int fbHeight = 0;
        SDL_GL_GetDrawableSize(window_, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);

        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);

    }

    return 0;
}

void GuiApp::addReceivedMidiMessage(const std::string& message) {
    receivedMidiLog_.push_back(message);
    if (receivedMidiLog_.size() > maxMidiLogEntries_) {
        receivedMidiLog_.erase(receivedMidiLog_.begin());
    }
}

void GuiApp::addSentMidiMessage(const std::string& message) {
    sentMidiLog_.push_back(message);
    if (sentMidiLog_.size() > maxMidiLogEntries_) {
        sentMidiLog_.erase(sentMidiLog_.begin());
    }
}

void GuiApp::render() {
    ImGui::Begin("TCP MIDI Interface");

    ImGui::SeparatorText("Server Connection");

    ImGui::InputText("IP", ipBuffer_, sizeof(ipBuffer_));
    ImGui::InputText("Port", portBuffer_, sizeof(portBuffer_));

    if (ImGui::Button("Connect")) {
        int port = std::atoi(portBuffer_);
        app_.setServerConfig(ipBuffer_, port);

        if (app_.connect()) {
            statusMessage_ = "Connected to server.";
        } else {
            statusMessage_ = "Failed to connect.";
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Disconnect")) {
        app_.disconnect();
        statusMessage_ = "Disconnected.";
    }

    ImGui::Text("Connection status: %s", app_.isConnected() ? "Connected" : "Disconnected");

    ImGui::SeparatorText("MIDI Ports");

    if (ImGui::Button("Refresh Ports")) {
        inputPorts_ = app_.getInputPortNames();
        outputPorts_ = app_.getOutputPortNames();
        statusMessage_ = "MIDI ports refreshed.";
    }

    const char* inputPreview =
        (selectedInputIndex_ >= 0 && selectedInputIndex_ < static_cast<int>(inputPorts_.size()))
            ? inputPorts_[selectedInputIndex_].c_str()
            : "Select Sending port";

    if (ImGui::BeginCombo("Input", inputPreview)) {
        for (int i = 0; i < static_cast<int>(inputPorts_.size()); ++i) {
            const bool isSelected = (selectedInputIndex_ == i);

            if (ImGui::Selectable(inputPorts_[i].c_str(), isSelected)) {
                if (app_.selectInputPort(static_cast<std::size_t>(i))) {
                    selectedInputIndex_ = i;
                    statusMessage_ = "Selected MIDI Sending port.";
                } else {
                    statusMessage_ = "Failed to select MIDI Sending port.";
                }
            }

            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    const char* outputPreview =
        (selectedOutputIndex_ >= 0 && selectedOutputIndex_ < static_cast<int>(outputPorts_.size()))
            ? outputPorts_[selectedOutputIndex_].c_str()
            : "Select Receiving port";

    if (ImGui::BeginCombo("Output", outputPreview)) {
        for (int i = 0; i < static_cast<int>(outputPorts_.size()); ++i) {
            const bool isSelected = (selectedOutputIndex_ == i);

            if (ImGui::Selectable(outputPorts_[i].c_str(), isSelected)) {
                if (app_.selectOutputPort(static_cast<std::size_t>(i))) {
                    selectedOutputIndex_ = i;
                    statusMessage_ = "Selected MIDI Receiving port.";
                } else {
                    statusMessage_ = "Failed to select MIDI Receiving port.";
                }
            }
    
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("MIDI Streaming");

    if (ImGui::Button("Start")) {
        if (app_.startStreaming()) {
            statusMessage_ = "Streaming started.";
        } else {
            statusMessage_ = "Failed to start streaming.";
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop")) {
        app_.stopStreaming();
        statusMessage_ = "Streaming stopped.";
    }
    //MIDI naming conventions are confusiing for users, an output port is the sending port in this ocntext. 
    ImGui::Text("Streaming status: %s", app_.isStreaming() ? "Running" : "Stopped");
    ImGui::Checkbox("Auto-scroll MIDI logs", &autoScrollMidiLogs_);
    ImGui::SeparatorText("Sent MIDI Events");
    ImGui::BeginChild("ReceivedMidiLog", ImVec2(0, 140), true);
    for (const auto& line : receivedMidiLog_) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (autoScrollMidiLogs_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    //MIDI naming conventions are confusiing for users, an input port is the receiving port in this context. 
    ImGui::SeparatorText("Received MIDI Events");
    ImGui::BeginChild("SentMidiLog", ImVec2(0, 140), true);
    for (const auto& line : sentMidiLog_) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (autoScrollMidiLogs_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    
    ImGui::EndChild();

    ImGui::End();
}