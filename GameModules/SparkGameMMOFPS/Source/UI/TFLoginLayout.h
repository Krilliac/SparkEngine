/**
 * @file TFLoginLayout.h
 * @brief Geometry of the TERRAFRONT onboarding panel and its sign-in form at any UI scale.
 *
 * TFLoginFlowDraw.cpp draws the panel and the CALLSIGN / PASSPHRASE form from these numbers, so every
 * offset, size and font size grows with EngineSettings uiScale instead of staying at its 100 % pixel
 * value. The header is ImGui-free so SparkTests checks the layout headlessly (TestTFOnboarding.cpp).
 *
 * Contract: pure functions over plain values; any thread. No allocation. Inputs and outputs are screen
 * pixels except @c scale (the effective UI scale, TFUi::UiScale() at draw time).
 */
#pragma once

#include <algorithm>

namespace Terrafront::TFLoginLayout
{
    /// @brief Logical (100 %) metrics of the onboarding panel and the sign-in form.
    namespace Metric
    {
        inline constexpr float kPanelMaxWidth = 600.0f;
        inline constexpr float kPanelMaxHeight = 560.0f;
        inline constexpr float kPanelWidthFraction = 0.92f; ///< Of the viewport width.
        inline constexpr float kViewportMargin = 16.0f;     ///< Total vertical margin kept around the panel.
        inline constexpr float kTopMargin = 20.0f;
        inline constexpr float kSideMargin = 60.0f;
        inline constexpr float kTitleSize = 28.0f;
        inline constexpr float kTitleGap = 4.0f;
        inline constexpr float kSubtitleSize = 14.0f;
        inline constexpr float kFormGap = 16.0f; ///< Subtitle to the first field label.
        inline constexpr float kLabelSize = 13.0f;
        inline constexpr float kLabelGap = 6.0f;  ///< Field label to its input.
        inline constexpr float kFieldGap = 14.0f; ///< Input to the next field label.
        inline constexpr float kErrorGap = 6.0f;
        inline constexpr float kErrorSize = 14.0f;
        inline constexpr float kLanGap = 6.0f;
        inline constexpr float kLanHeaderGap = 6.0f;
        inline constexpr float kListButtonGap = 12.0f;
        inline constexpr float kMinListHeight = 24.0f;
        inline constexpr float kButtonHeight = 40.0f;
        inline constexpr float kButtonGap = 20.0f;
        inline constexpr float kBottomMargin = 28.0f;
    } // namespace Metric

    /// @brief Screen rectangle of the onboarding panel.
    struct PanelRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

    /**
     * @brief The onboarding panel centred in the viewport: 600 x 560 logical pixels, narrowed to 92 % of the
     *        viewport width and to the viewport height less a small margin.
     */
    inline PanelRect ComputePanel(float viewX, float viewY, float viewW, float viewH, float scale)
    {
        PanelRect panel;
        panel.w = std::min(Metric::kPanelMaxWidth * scale, viewW * Metric::kPanelWidthFraction);
        panel.h =
            std::min(Metric::kPanelMaxHeight * scale, std::max(viewH * 0.5f, viewH - Metric::kViewportMargin * scale));
        panel.x = viewX + (viewW - panel.w) * 0.5f;
        panel.y = viewY + (viewH - panel.h) * 0.5f;
        return panel;
    }

    /// @brief Every position and size of the sign-in / create-account form inside the panel.
    struct SignInForm
    {
        float centerX = 0.0f; ///< Horizontal centre of the title, subtitle and error line.
        float titleSize = 0.0f;
        float titleCenterY = 0.0f;
        float subtitleSize = 0.0f;
        float subtitleCenterY = 0.0f;
        float fieldX = 0.0f; ///< Left edge of labels, inputs, the LAN list and the buttons.
        float fieldW = 0.0f; ///< Width of the inputs, the LAN list and the button row.
        float labelSize = 0.0f;
        float callsignLabelY = 0.0f;
        float callsignInputY = 0.0f;
        float passphraseLabelY = 0.0f;
        float passphraseInputY = 0.0f;
        float inputHeight = 0.0f;
        float errorSize = 0.0f;
        float errorCenterY = 0.0f;
        bool showLan = false; ///< False when the panel has no room for a LAN list of the minimum height.
        float lanHeaderY = 0.0f;
        float lanListY = 0.0f;
        float lanListH = 0.0f;
        float buttonY = 0.0f;
        float buttonW = 0.0f;
        float buttonH = 0.0f;
        float buttonGap = 0.0f;
    };

    /**
     * @brief Lay the sign-in form out top-down inside @p panel: title, subtitle, CALLSIGN label over its
     *        input, PASSPHRASE label over its input, an error line, the LAN SERVERS header and list, and the
     *        two buttons anchored to the bottom.
     * @param inputHeight Height of one input box at the current font (ImGui::GetFrameHeight()).
     */
    inline SignInForm ComputeSignInForm(const PanelRect& panel, float scale, float inputHeight)
    {
        using namespace Metric;
        SignInForm form;
        form.centerX = panel.x + panel.w * 0.5f;
        form.fieldX = panel.x + kSideMargin * scale;
        form.fieldW = std::max(0.0f, panel.w - kSideMargin * scale * 2.0f);
        form.inputHeight = inputHeight;

        float y = panel.y + kTopMargin * scale;
        form.titleSize = kTitleSize * scale;
        form.titleCenterY = y + form.titleSize * 0.5f;
        y += form.titleSize + kTitleGap * scale;
        form.subtitleSize = kSubtitleSize * scale;
        form.subtitleCenterY = y + form.subtitleSize * 0.5f;
        y += form.subtitleSize + kFormGap * scale;

        form.labelSize = kLabelSize * scale;
        form.callsignLabelY = y;
        y += form.labelSize + kLabelGap * scale;
        form.callsignInputY = y;
        y += inputHeight + kFieldGap * scale;
        form.passphraseLabelY = y;
        y += form.labelSize + kLabelGap * scale;
        form.passphraseInputY = y;
        y += inputHeight + kErrorGap * scale;

        form.errorSize = kErrorSize * scale;
        form.errorCenterY = y + form.errorSize * 0.5f;
        y += form.errorSize + kLanGap * scale;

        form.buttonH = kButtonHeight * scale;
        form.buttonGap = kButtonGap * scale;
        form.buttonW = std::max(0.0f, (form.fieldW - form.buttonGap) * 0.5f);
        // Buttons stay at the bottom, but never above the form.
        form.buttonY = std::max(y, panel.y + panel.h - kBottomMargin * scale - form.buttonH);

        form.lanHeaderY = y;
        form.lanListY = y + form.labelSize + kLanHeaderGap * scale;
        form.lanListH = form.buttonY - kListButtonGap * scale - form.lanListY;
        form.showLan = form.lanListH >= kMinListHeight * scale;
        if (!form.showLan)
        {
            form.lanListH = 0.0f;
        }
        return form;
    }
} // namespace Terrafront::TFLoginLayout
