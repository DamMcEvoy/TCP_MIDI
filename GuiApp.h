#ifndef GUI_APP_H
#define GUI_APP_H

#include <string>
#include <vector>

//#include <SDL2/SDL.h>

typedef void* SDL_GLContext;
struct SDL_Window;

class AppController;

class GuiApp {
public:
    explicit GuiApp(AppController& app);
    ~GuiApp();

    int run();
    void addReceivedMidiMessage(const std::string& message);
    void addSentMidiMessage(const std::string& message);

private:
    bool initialize();
    void shutdown();
    void render();

    AppController& app_;

    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    bool initialized_ = false;
    bool running_ = true;

    char ipBuffer_[64] = "20.13.138.208";
    char portBuffer_[16] = "443";

    std::vector<std::string> inputPorts_;
    std::vector<std::string> outputPorts_;
    int selectedInputIndex_ = -1;
    int selectedOutputIndex_ = -1;

    std::string statusMessage_ = "Ready.";

    std::vector<std::string> receivedMidiLog_;
    std::vector<std::string> sentMidiLog_;
    bool autoScrollMidiLogs_ = true;
    std::size_t maxMidiLogEntries_ = 500;
};

#endif // GUI_APP_H