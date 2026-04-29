#include "AppController.h"
#include "GuiApp.h"

#include <iostream>

int main() {
    AppController app("20.13.138.208", 443);

    if (!app.initialize()) {
        std::cerr << "Failed to initialize application." << std::endl;
        app.shutdown();
        return 1;
    }

    GuiApp gui(app);

    app.setSentMidiLogCallback([&gui](const std::string& message) {
        gui.addSentMidiMessage(message);
    });

    app.setReceivedMidiLogCallback([&gui](const std::string& message) {
        gui.addReceivedMidiMessage(message);
    });

    const int result = gui.run();

    app.clearGuiLogCallbacks();
    app.shutdown();
    return result;
}