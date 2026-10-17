/**
 * @file LauncherApp.cpp
 * @brief Standalone SparkLauncher state, actions and editor-spawn logic (drawing is in LauncherViews.cpp).
 * @author Spark Engine Team
 * @date 2025
 */

#include "LauncherApp.h"
#include "LauncherPaths.h"

#include "Utils/FolderDialog.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace SparkLauncher
{
    namespace
    {
        // Copies all of @p text or nothing: truncating UTF-8 could split a character
        // and leave the buffer undecodable.
        template <std::size_t N> bool CopyWhole(const std::string& text, char (&buffer)[N])
        {
            if (text.size() >= N)
            {
                return false;
            }
            std::memcpy(buffer, text.c_str(), text.size() + 1);
            return true;
        }

        /// The single .sparkproject file in @p folder, or an empty path when there is none.
        fs::path FindProjectFileIn(const fs::path& folder)
        {
            std::error_code iterateError;
            for (fs::directory_iterator it(folder, iterateError), end; !iterateError && it != end;
                 it.increment(iterateError))
            {
                std::error_code entryError;
                if (it->is_regular_file(entryError) && it->path().extension() == ".sparkproject")
                {
                    return it->path();
                }
            }
            return {};
        }

        /// Characters ProjectManager refuses in a project name (it is also a folder name).
        bool HasForbiddenNameCharacter(std::string_view name)
        {
            constexpr std::string_view kForbidden = "\\/:*?\"<>|";
            return std::any_of(
                name.begin(), name.end(), [&](char c)
                { return static_cast<unsigned char>(c) < 0x20 || kForbidden.find(c) != std::string_view::npos; });
        }
    } // namespace

    LauncherApp::LauncherApp() = default;
    LauncherApp::~LauncherApp() = default;

    bool LauncherApp::Initialize()
    {
        m_projectManager = std::make_unique<SparkEditor::ProjectManager>();
        if (!m_projectManager->Initialize())
        {
            ReportStatus("Failed to initialize ProjectManager", true);
            return false;
        }
        m_engineDirectory = PathToUtf8(GetLauncherExecutablePath().parent_path());
        LoadTemplates();
        DefaultNewProjectLocation();
        RefreshRecentProjects();
        return true;
    }

    void LauncherApp::LoadTemplates()
    {
        m_templates.clear();
        const fs::path templatesDir = FindLauncherTemplatesDirectory(GetLauncherExecutablePath(), fs::current_path());
        if (templatesDir.empty())
        {
            return;
        }
        // The error_code forms: a directory that cannot be listed, or an entry whose type cannot
        // be read, ends or skips the scan instead of throwing out of Initialize.
        std::error_code error;
        for (fs::directory_iterator it(templatesDir, error), end; !error && it != end; it.increment(error))
        {
            std::error_code typeError;
            if (!it->is_directory(typeError) || typeError)
            {
                continue;
            }
            auto entry = ReadTemplateEntry(it->path());
            if (entry)
            {
                m_templates.push_back(std::move(*entry));
            }
        }
        std::sort(m_templates.begin(), m_templates.end(),
                  [](const TemplateEntry& a, const TemplateEntry& b) { return a.displayName < b.displayName; });
    }

    void LauncherApp::DefaultNewProjectLocation()
    {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        // getenv returns active-code-page text on Windows, which fs::path(const char*)
        // decodes; the ImGui buffer holds UTF-8.
        fs::path base = home ? fs::path(home) / "SparkProjects" : fs::current_path();
        if (!CopyWhole(PathToUtf8(base), m_form.location))
        {
            m_form.location[0] = '\0';
        }
    }

    void LauncherApp::DrawUI()
    {
        // Another window may have created, moved or deleted projects while the launcher was in
        // the background: re-check the cached list when focus comes back.
        const bool focusLost = ImGui::GetIO().AppFocusLost;
        if (m_appFocusLost && !focusLost)
        {
            RefreshRecentProjects();
        }
        m_appFocusLost = focusLost;

        DrawLauncher(*this);
    }

    void LauncherApp::ReportStatus(std::string message, bool isError)
    {
        m_statusMessage = std::move(message);
        m_statusIsError = isError;
        m_statusTime = std::chrono::steady_clock::now();
    }

    void LauncherApp::ClearStatus()
    {
        m_statusMessage.clear();
        m_statusIsError = false;
    }

    void LauncherApp::RefreshRecentProjects()
    {
        m_projectManager->RefreshRecentProjects();
        m_recentProjects = m_projectManager->GetRecentProjects();
    }

    void LauncherApp::OpenExistingProject()
    {
        std::string folder;
        if (!SparkEditor::Utils::BrowseForFolder(folder, "Select Project Folder"))
        {
            return;
        }
        // BrowseForFolder returns active-code-page text on Windows, which
        // fs::path(std::string) decodes. From here on the project is a path, and
        // it only becomes text again as UTF-8 (ProjectManager, status line).
        const fs::path folderPath(folder);
        // A project directory contains a single .sparkproject file.
        const fs::path chosen = FindProjectFileIn(folderPath);
        if (chosen.empty())
        {
            ReportStatus("No .sparkproject file found in " + PathToUtf8(folderPath), true);
        }
        else if (!m_projectManager->OpenProject(PathToUtf8(chosen)))
        {
            ReportStatus("Failed to open " + PathToUtf8(chosen), true);
        }
        else
        {
            SpawnTarget(chosen, LaunchTarget::Editor);
        }
        RefreshRecentProjects();
    }

    void LauncherApp::OpenRecentProject(const SparkEditor::RecentProject& project, LaunchTarget target)
    {
        if (target == LaunchTarget::Editor && !m_projectManager->OpenProject(project.path))
        {
            ReportStatus("Failed to open " + project.path, true);
            RefreshRecentProjects();
            return;
        }
        // ProjectManager stores recent-project paths as UTF-8 text.
        auto projectFile = PathFromUtf8(project.path);
        if (!projectFile)
        {
            ReportStatus(projectFile.error() + ": " + project.path, true);
            return;
        }
        SpawnTarget(*projectFile, target);
        RefreshRecentProjects();
    }

    void LauncherApp::LocateRecentProject(const SparkEditor::RecentProject& project)
    {
        std::string folder;
        if (!SparkEditor::Utils::BrowseForFolder(folder, "Locate Project Folder"))
        {
            return;
        }
        const fs::path folderPath(folder);
        const fs::path chosen = FindProjectFileIn(folderPath);
        if (chosen.empty())
        {
            ReportStatus("No .sparkproject file found in " + PathToUtf8(folderPath), true);
            return;
        }
        const std::string chosenText = PathToUtf8(chosen);
        if (!m_projectManager->OpenProject(chosenText))
        {
            ReportStatus("Failed to open " + chosenText, true);
            return;
        }
        // OpenProject recorded the new location; drop the stale entry unless it is the same file.
        if (project.path != chosenText)
        {
            m_projectManager->RemoveRecentProject(project.path);
        }
        RefreshRecentProjects();
        ReportStatus("Located " + project.name + " at " + chosenText, false);
    }

    void LauncherApp::RemoveRecentProject(const std::string& path)
    {
        m_projectManager->RemoveRecentProject(path);
        RefreshRecentProjects();
    }

    void LauncherApp::SelectTemplate(int index)
    {
        if (index >= 0 && index < static_cast<int>(m_templates.size()))
        {
            m_selectedTemplate = index;
        }
    }

    NewProjectValidation LauncherApp::ValidateNewProject() const
    {
        NewProjectValidation result;
        const std::string_view nameText(m_form.name);
        const std::string_view locationText(m_form.location);
        if (nameText.empty())
        {
            result.nameError = "Name and location are required";
        }
        if (locationText.empty())
        {
            result.locationError = "Name and location are required";
        }

        // Both buffers are ImGui (UTF-8) text. fs::path(const char*) would read them as
        // the active code page on Windows, and path::string() throws for characters
        // outside it -- every frame, from whatever the user typed.
        const auto name = PathFromUtf8(nameText);
        const auto location = PathFromUtf8(locationText);
        if (result.nameError.empty() && !name)
        {
            result.nameError = "Project name and location must be valid UTF-8 text";
        }
        if (result.locationError.empty() && !location)
        {
            result.locationError = "Project name and location must be valid UTF-8 text";
        }
        if (result.nameError.empty() && (HasForbiddenNameCharacter(nameText) || nameText.front() == ' ' ||
                                         nameText.back() == ' ' || nameText.back() == '.'))
        {
            result.nameError = "Project name cannot contain \\ / : * ? \" < > | or begin or end with a space, or end "
                               "with a dot";
        }
        if (!result.nameError.empty() || !result.locationError.empty())
        {
            return result;
        }

        result.projectRoot = *location / *name;
        std::error_code existsError;
        if (fs::exists(result.projectRoot, existsError))
        {
            result.nameError = "Project destination already exists: " + PathToUtf8(result.projectRoot);
        }
        return result;
    }

    void LauncherApp::BrowseNewProjectLocation()
    {
        std::string folder;
        if (SparkEditor::Utils::BrowseForFolder(folder, "Select Parent Folder"))
        {
            // Active-code-page text from the dialog; the ImGui buffer holds UTF-8.
            if (!CopyWhole(PathToUtf8(fs::path(folder)), m_form.location))
            {
                ReportStatus("Selected folder path is too long", true);
            }
        }
    }

    void LauncherApp::CreateNewProject()
    {
        if (m_templates.empty())
        {
            return;
        }
        const NewProjectValidation validation = ValidateNewProject();
        if (!validation.IsValid())
        {
            ReportStatus(validation.FirstError(), true);
            return;
        }
        const TemplateEntry& tpl = m_templates[static_cast<std::size_t>(m_selectedTemplate)];
        const fs::path& projectRoot = validation.projectRoot;

        std::error_code ec;
        fs::create_directories(projectRoot.parent_path(), ec);
        if (ec)
        {
            ReportStatus("Cannot create directory: " + ec.message(), true);
            return;
        }

        if (!m_projectManager->CreateProjectFromTemplate(m_form.name, PathToUtf8(projectRoot), tpl.directoryName))
        {
            ReportStatus("CreateProjectFromTemplate failed for " + tpl.directoryName, true);
            return;
        }

        fs::path projectFileName = projectRoot.filename();
        projectFileName += ".sparkproject";
        const fs::path sparkproj = projectRoot / projectFileName;
        std::error_code existsError;
        if (!fs::exists(sparkproj, existsError))
        {
            ReportStatus("Project file not produced: " + PathToUtf8(sparkproj), true);
            return;
        }

        SpawnTarget(sparkproj, LaunchTarget::Editor);
        RefreshRecentProjects();
    }

    bool LauncherApp::SpawnTarget(const fs::path& projectFile, LaunchTarget target)
    {
        // This runs inside the ImGui frame with nothing above it to catch: a
        // filesystem or encoding exception from an unusual project path must end as a
        // status message, not std::terminate.
        try
        {
            const fs::path ownPath = GetLauncherExecutablePath();
            if (ownPath.empty())
            {
                ReportStatus("Could not determine launcher path", true);
                return false;
            }
            auto request = BuildLaunchRequest(ownPath.parent_path(), projectFile, target);
            if (!request)
            {
                ReportStatus(request.error(), true);
                return false;
            }
            auto launched = LaunchDetached(*request);
            if (!launched)
            {
                ReportStatus(launched.error(), true);
                return false;
            }
        }
        catch (const std::exception& exception)
        {
            ReportStatus(std::string("Could not launch ") + LaunchTargetName(target) + ": " + exception.what(), true);
            return false;
        }
        ReportStatus(std::string(LaunchTargetName(target)) + " started", false);
        m_shouldClose = target != LaunchTarget::DedicatedServer;
        return true;
    }
} // namespace SparkLauncher
