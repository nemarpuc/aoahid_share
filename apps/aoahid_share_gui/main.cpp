// SPDX-License-Identifier: MIT
//
// aoahid_share_gui: edits the config and shows what the daemon reports.
#include "app.hpp"
#include "look.hpp"
#include "page.hpp"

#include "process.hpp"

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <string>

int main() {
    if (glfwInit() == GLFW_FALSE)
        return 1;
#ifdef __APPLE__
    // macOS only offers a modern context as a core, forward-compatible one.
    const char* shader_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* shader_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    // Where the toolkit sizes windows in pixels, follow the monitor's scale.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window =
        glfwCreateWindow(1040, 760, "aoahid_share " AOAHID_SHARE_VERSION, nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Everything lives in the config; the toolkit keeps no file of its own.
    ImGui::GetIO().IniFilename = nullptr;
    float scale = 1.0F;
    float scale_y = 1.0F;
    glfwGetWindowContentScale(window, &scale, &scale_y);
    scale = std::clamp(scale, 1.0F, 4.0F);
    gui::look::setup(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(shader_version);

    gui::App app;
    app.program_dir = gui::program_directory();
    gui::load_config(app);
    // The daemon is never started on its own: the window has a button for it.
    gui::poll_daemon(app);

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        // A window nobody sees asks the daemon nothing.
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE) {
            glfwWaitEvents();
            continue;
        }
        glfwWaitEventsTimeout(gui::poll_seconds);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (ImGui::GetTime() >= app.next_poll) {
            // A daemon started from here that has since ended is collected.
            aoas::reap_detached();
            gui::poll_daemon(app);
            app.next_poll = ImGui::GetTime() + gui::poll_seconds;
        }
        gui::draw_window(app);
        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(1.0F, 1.0F, 1.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
