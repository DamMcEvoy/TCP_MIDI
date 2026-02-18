#include <SDL2/SDL.h>
#include <SDL_opengl.h>
#include <iostream>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

// ------------------------------------------------------------
// Helper to print actions (shortcut)
// ------------------------------------------------------------
void log_action(const char* msg)
{
    std::cout << msg << std::endl;
}

int main(int argc, char** argv)
{
    // ------------------------------------------------------------
    // SDL + OpenGL initialization
    // ------------------------------------------------------------
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL_Init error: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* window = SDL_CreateWindow(
        "TCP MIDI GUI (C++ / ImGui)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "Failed to create SDL window." << std::endl;
        return -1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // ------------------------------------------------------------
    // ImGui initialization
    // ------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    ImGui::StyleColorsDark();

    // ------------------------------------------------------------
    // GUI State Variables
    // ------------------------------------------------------------
    static char ip_buffer[32] = "20.13.138.208";   // Default IP address, see to obtain the correct one
    static char port_buffer[8] = "443";            // Default Port, see to obtain the correct one

    // Fake MIDI ports (replace with actual MIDI port handling as needed)
    static const char* midi_inputs[]  = { "Input A", "Input B", "Input C" };     // Fake MIDI input ports
    static const char* midi_outputs[] = { "Output X", "Output Y", "Output Z" };  // Fake MIDI output ports

    int current_input = 0;
    int current_output = 0;

    bool running = true;
    SDL_Event event;

    // ------------------------------------------------------------
    // Main loop
    // ------------------------------------------------------------
    while (running)
    {
        // Poll events
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
        }

        // Start new ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ------------------------------------------------------------
        // GUI WINDOWS
        // ------------------------------------------------------------

        
        ImGui::Begin("TCP MIDI Interface", nullptr, ImGuiWindowFlags_NoMove);


        // --- Connection Window ---

        ImGui::SeparatorText("Server Connection");

        ImGui::InputText("IP", ip_buffer, IM_ARRAYSIZE(ip_buffer));
        ImGui::InputText("Port", port_buffer, IM_ARRAYSIZE(port_buffer));

        if (ImGui::Button("Connect"))
            log_action("connect");  

        ImGui::SameLine();
        if (ImGui::Button("Disconnect"))
            log_action("disconnect");
        

        // --- MIDI Ports Window ---

        ImGui::SeparatorText("MIDI Ports");

        ImGui::Text("MIDI Input Ports");
        ImGui::Combo("Input", &current_input, midi_inputs, IM_ARRAYSIZE(midi_inputs));

        ImGui::Text("MIDI Output Ports");
        ImGui::Combo("Output", &current_output, midi_outputs, IM_ARRAYSIZE(midi_outputs));

        if (ImGui::Button("Refresh Ports"))
        {
            std::string msg = "refresh ports : MIDI Input Port " 
                            + std::string(midi_inputs[current_input])
                            + ", MIDI Output Port "
                            + std::string(midi_outputs[current_output]);

            log_action(msg.c_str());
}



        // --- Streaming Window ---
        ImGui::SeparatorText("MIDI Streaming");

        if (ImGui::Button("Start"))
            log_action("start streaming");

        ImGui::SameLine();
        if (ImGui::Button("Stop"))
            log_action("stop streaming");

        
        // --- Received Logs ---
        ImGui::SeparatorText("Received MIDI Events");
        ImGui::Text("Events will appear in terminal.");

        // --- Sent Logs ---
        ImGui::SeparatorText("Sent MIDI Events");
        ImGui::Text("Events will appear in terminal.");



        ImGui::End();


        // ------------------------------------------------------------
        // Rendering
        // ------------------------------------------------------------
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);

        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ------------------------------------------------------------
    // Cleanup
    // ------------------------------------------------------------
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
