// MeshCoreSniffer GUI: fixed Dear ImGui dashboard (GLFW + OpenGL3).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.h"
#include "panels.h"
#include "theme.h"

namespace {

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}
#endif

} // namespace

int main()
{
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    const char* kGlsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1500, 940, "MeshCoreSniffer", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    mcs::gui::theme::apply();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlsl);

    mcs::gui::AppState s;
    s.init();

    double exitAfter = 0.0;
#ifdef _WIN32
    bool startNow = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--wav" && i + 1 < argc) {
                const std::string p = wideToUtf8(argv[++i]);
                std::snprintf(s.cfgWavPath, sizeof(s.cfgWavPath), "%s", p.c_str());
                s.cfgLive = false;
                s.saveSettings();
            } else if (a == L"--live") {
                s.cfgLive = true;
                s.saveSettings();
            } else if (a == L"--start") {
                startNow = true;
            } else if (a == L"--exit-after" && i + 1 < argc) {
                exitAfter = std::wcstod(argv[++i], nullptr);
            }
        }
        LocalFree(argv);
    }
    if (startNow) s.startReceiver();
#endif

    const ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    bool wasRunning = false;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (exitAfter > 0.0 && glfwGetTime() >= exitAfter)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        s.status = s.receiver.status();
        if (!s.status.lastError.empty()) s.lastError = s.status.lastError;
        const bool running = s.receiver.running();
        if (wasRunning && !running && s.statusMsg == "Decoding WAV...") s.statusMsg = "Finished";
        wasRunning = running;
        if (s.requestQuit) glfwSetWindowShouldClose(window, GLFW_TRUE);

        mcs::gui::drawMenuBar(s);

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::Begin("MeshCoreSniffer##root", nullptr, kRootFlags);
        {
            mcs::gui::drawHeader(s);
            mcs::gui::drawToolbar(s);

            const float statusH = 42.0f;
            const float bodyH = std::max(120.0f, ImGui::GetContentRegionAvail().y - statusH);
            ImGui::BeginChild("body", ImVec2(0, bodyH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            mcs::gui::drawSidebar(s);
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            {
                const float H = ImGui::GetContentRegionAvail().y;
                mcs::gui::drawMapPanel(s, ImVec2(0, H * 0.54f));
                mcs::gui::drawMainTabs(s);
            }
            ImGui::EndChild();
            ImGui::EndChild();

            ImGui::BeginChild("status", ImVec2(0, statusH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            mcs::gui::drawStatusBar(s);
            ImGui::EndChild();

            mcs::gui::drawModals(s);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        s.tiles.pump();

        ImGui::Render();
        int dw = 0, dh = 0;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        const ImVec4 bg = mcs::gui::theme::bg;
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    s.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(); }
#endif
