#pragma once

/**
 * @file ModuleImGuiTestFrame.h
 * @brief RAII ImGui frame for exercising game-module UI draw paths in headless tests
 *
 * Game modules draw their in-game UI from IModule::OnImGui inside the host's
 * ImGui frame. Tests have no host overlay, so this fixture creates a private
 * context (restoring the previous one on destruction), builds the font atlas
 * so NewFrame() succeeds without a renderer backend, and opens one host
 * window for the module's RenderDebugUI() calls to lay out into.
 *
 * Thread affinity: the test thread that constructs it. Allocation: one ImGui
 * context per instance. Only compiled where Dear ImGui is linked.
 */

#include <imgui.h>

namespace SparkTest
{
    class ModuleImGuiTestFrame
    {
      public:
        ModuleImGuiTestFrame() : m_previous(ImGui::GetCurrentContext()), m_context(ImGui::CreateContext())
        {
            ImGui::SetCurrentContext(m_context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int textureWidth = 0;
            int textureHeight = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &textureWidth, &textureHeight);

            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
            ImGui::Begin("ModuleImGuiTestHost");
        }

        ~ModuleImGuiTestFrame()
        {
            ImGui::End();
            ImGui::EndFrame();
            ImGui::DestroyContext(m_context);
            ImGui::SetCurrentContext(m_previous);
        }

        ModuleImGuiTestFrame(const ModuleImGuiTestFrame&) = delete;
        ModuleImGuiTestFrame& operator=(const ModuleImGuiTestFrame&) = delete;

      private:
        ImGuiContext* m_previous;
        ImGuiContext* m_context;
    };
} // namespace SparkTest
