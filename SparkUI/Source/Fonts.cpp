/**
 * @file Fonts.cpp
 * @brief Font directory discovery, fail-safe face loading and role lookup.
 */

#include "SparkUI/Fonts.h"

#include "SparkUI/Runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <system_error>

#ifdef IMGUI_ENABLE_FREETYPE
#include <misc/freetype/imgui_freetype.h>
#endif

namespace Spark::Ui
{
    namespace
    {
#ifdef IMGUI_ENABLE_FREETYPE
        constexpr unsigned int kLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
#else
        constexpr unsigned int kLoaderFlags = 0;
#endif

        // Font Awesome 6 solid lives in the private-use block U+F000..U+F8FF.
        constexpr std::array<ImWchar, 3> kIconRanges = {0xF000, 0xF8FF, 0};
        constexpr float kIconSizeRatio = 0.92f;
        constexpr float kIconOffsetY = 1.0f;

        bool IsRegularFile(const std::filesystem::path& path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
        }

        bool IsDirectory(const std::filesystem::path& path)
        {
            std::error_code ec;
            return !path.empty() && std::filesystem::is_directory(path, ec);
        }

        std::filesystem::path FirstExisting(const std::filesystem::path& dir, std::initializer_list<const char*> names)
        {
            for (const char* name : names)
            {
                std::filesystem::path candidate = dir / name;
                if (IsRegularFile(candidate))
                {
                    return candidate;
                }
            }
            return {};
        }

        ImFontConfig MakeConfig(const char* name)
        {
            ImFontConfig config;
            config.FontLoaderFlags = kLoaderFlags;
            config.Flags |= ImFontFlags_NoLoadError;
            const std::size_t length = std::min(std::strlen(name), sizeof(config.Name) - 1);
            std::memcpy(config.Name, name, length);
            config.Name[length] = '\0';
            return config;
        }

        // ImGui opens font files through ImFileOpen, which decodes the name as UTF-8 (on Windows it
        // widens with CP_UTF8 and calls _wfopen). path::string() is the wrong spelling there: MSVC
        // converts to the ANSI code page, so an accented install directory fails to open and a
        // character the code page lacks throws std::system_error. u8string() is exact; it can
        // only fail for a Windows name holding an unpaired UTF-16 surrogate, which no UTF-8
        // string can express, so that path is reported as unusable instead of thrown.
        std::string Utf8Path(const std::filesystem::path& path)
        {
            try
            {
                const std::u8string utf8 = path.u8string();
                return std::string(utf8.begin(), utf8.end());
            }
            catch (const std::system_error&)
            {
                return {};
            }
        }

        ImFont* LoadFace(ImFontAtlas& atlas, const std::filesystem::path& file, const char* name, float sizePx,
                         const std::filesystem::path& iconFile)
        {
            const std::string fileUtf8 = Utf8Path(file);
            if (fileUtf8.empty())
            {
                return nullptr;
            }
            ImFontConfig config = MakeConfig(name);
            ImFont* font = atlas.AddFontFromFileTTF(fileUtf8.c_str(), sizePx, &config);
            const std::string iconUtf8 = Utf8Path(iconFile);
            if (font != nullptr && !iconUtf8.empty())
            {
                ImFontConfig icons = MakeConfig(name);
                icons.MergeMode = true;
                icons.PixelSnapH = true;
                icons.GlyphOffset = ImVec2(0.0f, kIconOffsetY);
                icons.GlyphMinAdvanceX = sizePx * kIconSizeRatio;
                atlas.AddFontFromFileTTF(iconUtf8.c_str(), sizePx * kIconSizeRatio, &icons, kIconRanges.data());
            }
            return font;
        }

        void Note(std::string& diagnostic, const char* message)
        {
            if (!diagnostic.empty())
            {
                diagnostic += "; ";
            }
            diagnostic += message;
        }

        const char* ConfigNameFor(FontRole r) noexcept
        {
            switch (r)
            {
            case FontRole::Strong:
            case FontRole::H2:
            case FontRole::H1:
                return kFontNameStrong;
            case FontRole::Display:
                return kFontNameBold;
            case FontRole::Mono:
            case FontRole::MonoSmall:
                return kFontNameMono;
            case FontRole::Small:
            case FontRole::Body:
                break;
            }
            return kFontNameBody;
        }

        ImFont* FontByName(const char* name)
        {
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            if (atlas == nullptr)
            {
                return nullptr;
            }
            for (ImFont* font : atlas->Fonts)
            {
                if (font != nullptr && font->Sources.Size > 0 && std::strcmp(font->Sources[0]->Name, name) == 0)
                {
                    return font;
                }
            }
            return nullptr;
        }
    } // namespace

    std::filesystem::path FindFontDirectory(const std::filesystem::path& exeDir,
                                            std::span<const std::filesystem::path> legacyProbes)
    {
        if (!exeDir.empty())
        {
            std::filesystem::path staged = exeDir / "EditorAssets" / "Fonts";
            if (IsDirectory(staged))
            {
                return staged;
            }
        }
        for (const std::filesystem::path& probe : legacyProbes)
        {
            if (IsDirectory(probe))
            {
                return probe;
            }
        }
        return {};
    }

    FontSet LoadFontSet(ImFontAtlas& atlas, const std::filesystem::path& fontDir, float bodyPx)
    {
        FontSet set;
        const float size = bodyPx > 0.0f ? bodyPx : 15.0f;
        const bool dirExists = IsDirectory(fontDir);
        // Every face lives in fontDir, so a directory without a UTF-8 spelling makes all of them
        // unloadable; treat it like a missing directory rather than half-loading.
        const bool haveDir = dirExists && !Utf8Path(fontDir).empty();
        const std::filesystem::path iconFile =
            haveDir ? FirstExisting(fontDir, {"fa-solid-900.ttf"}) : std::filesystem::path{};

        if (haveDir)
        {
            const std::filesystem::path bodyFile =
                FirstExisting(fontDir, {"IBMPlexSans-Regular.ttf", "Roboto-Regular.ttf"});
            set.body = LoadFace(atlas, bodyFile, kFontNameBody, size, iconFile);
            if (set.body != nullptr && bodyFile.filename() != "IBMPlexSans-Regular.ttf")
            {
                Note(set.diagnostic, "IBMPlexSans-Regular.ttf missing; using Roboto-Regular.ttf");
            }
        }
        if (set.body == nullptr)
        {
            ImFontConfig config = MakeConfig(kFontNameBody);
            config.SizePixels = size;
            set.body = atlas.AddFontDefaultVector(&config);
            set.strong = set.body;
            set.bold = set.body;
            set.mono = set.body;
            set.custom = false;
            if (haveDir)
            {
                Note(set.diagnostic, "no body font in the font directory; using the embedded default font");
            }
            else if (dirExists)
            {
                Note(set.diagnostic, "font directory path is not valid Unicode; using the embedded default font");
            }
            else
            {
                Note(set.diagnostic, "font directory not found; using the embedded default font");
            }
            return set;
        }
        set.custom = true;
        if (iconFile.empty())
        {
            Note(set.diagnostic, "fa-solid-900.ttf missing; icons will not render");
        }

        const std::filesystem::path strongFile =
            FirstExisting(fontDir, {"IBMPlexSans-SemiBold.ttf", "IBMPlexSans-Bold.ttf", "Roboto-Bold.ttf"});
        set.strong = LoadFace(atlas, strongFile, kFontNameStrong, size, iconFile);
        if (set.strong == nullptr)
        {
            set.strong = set.body;
            Note(set.diagnostic, "no semibold or bold face; headers use the body face");
        }
        else if (strongFile.filename() != "IBMPlexSans-SemiBold.ttf")
        {
            Note(set.diagnostic, "IBMPlexSans-SemiBold.ttf missing; headers use a bold face");
        }

        const std::filesystem::path boldFile = FirstExisting(fontDir, {"IBMPlexSans-Bold.ttf", "Roboto-Bold.ttf"});
        set.bold = LoadFace(atlas, boldFile, kFontNameBold, size, iconFile);
        if (set.bold == nullptr)
        {
            set.bold = set.strong;
            Note(set.diagnostic, "no bold face; display text uses the header face");
        }

        const std::filesystem::path monoFile = FirstExisting(fontDir, {"JetBrainsMono-Regular.ttf"});
        set.mono = LoadFace(atlas, monoFile, kFontNameMono, size, iconFile);
        if (set.mono == nullptr)
        {
            set.mono = set.body;
            Note(set.diagnostic, "JetBrainsMono-Regular.ttf missing; monospace text uses the body face");
        }
        return set;
    }

    ImFont* FindFont(FontRole r)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return nullptr;
        }
        if (ImFont* font = FontByName(ConfigNameFor(r)))
        {
            return font;
        }
        if (ImFont* body = FontByName(kFontNameBody))
        {
            return body;
        }
        return ImGui::GetFont();
    }

    float FontSize(FontRole r)
    {
        const Typography& type = ActiveTokens().type;
        switch (r)
        {
        case FontRole::Small:
            return type.caption;
        case FontRole::Body:
            return type.body;
        case FontRole::Strong:
            return type.strong;
        case FontRole::H2:
            return type.h2;
        case FontRole::H1:
            return type.h1;
        case FontRole::Display:
            return type.display;
        case FontRole::Mono:
            return type.mono;
        case FontRole::MonoSmall:
            return type.monoSmall;
        }
        return type.body;
    }

    ScopedFont::ScopedFont(FontRole r, float sizeLogical)
    {
        ImGui::PushFont(FindFont(r), sizeLogical > 0.0f ? sizeLogical : FontSize(r));
    }

    ScopedFont::~ScopedFont()
    {
        ImGui::PopFont();
    }
} // namespace Spark::Ui
