"""Guard the editor's user-visible distinction between preview and gameplay."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def has_copy(test: unittest.TestCase, path: str, text: str) -> None:
    test.assertTrue(text in source(path), f"missing user-facing copy in {path}: {text}")


class EditorPlayTruthTests(unittest.TestCase):
    def test_play_toolbar_discloses_preview_and_non_gameplay_counters(self) -> None:
        path = "SparkEditor/Source/Panels/PlayModeToolbarPanel.cpp"
        has_copy(self, path, "State preview only: no gameplay systems tick here.")
        has_copy(self, path, "Play Control > Launch Game")
        has_copy(self, path, "Preview state frames")
        has_copy(self, path, "Preview state rate")

    def test_global_play_entries_open_play_control(self) -> None:
        has_copy(self, "SparkEditor/Source/Core/EditorMenuBar.cpp",
                 "Open Play Control; Launch Game runs actual gameplay in SparkEngine.exe")
        registry = source("SparkEditor/Source/Core/EditorCommandRegistry.cpp")
        self.assertIn('"Open Play Control", "Command"', registry)
        stop_action = registry.split('"Open Play Control to stop games", "Command"', 1)[1].split(
            '"Shift+F5"', 1)[0]
        self.assertIn('SetPanelVisible("PlayControl", true)', stop_action)
        self.assertIn('"Shift+F5"', registry)
        ui = source("SparkEditor/Source/Core/EditorUI.cpp")
        f5 = ui.split("if (ImGui::IsKeyPressed(ImGuiKey_F5)", 1)[1].split(
            "if (ImGui::IsKeyPressed(ImGuiKey_F6)", 1)[0]
        self.assertIn('SetPanelVisible("PlayControl", true)', f5)
        self.assertNotIn("TogglePlayMode", f5)
        self.assertIn('SetPanelVisible("PlayControl", true)', ui.split(
            "if (ImGui::IsKeyPressed(ImGuiKey_F6)", 1)[1])

    def test_real_game_launcher_remains_separate(self) -> None:
        path = "SparkEditor/Source/Panels/PlayControlPanel.cpp"
        has_copy(self, path, "In-editor play is not offered")
        has_copy(self, path, 'ImGui::Button("Launch Game")')
        has_copy(self, "SparkEditor/Source/Panels/PlayControlLaunch.cpp", "PlayControlPanel::LaunchGame()")
        has_copy(self, "SparkEditor/Source/Utils/EditorProcessLaunch.cpp", 'L" -game "')

    def test_game_view_calls_out_the_preview(self) -> None:
        path = "SparkEditor/Source/Panels/GameViewPanel.cpp"
        has_copy(self, path, "State preview only (no gameplay)")
        has_copy(self, path, "Click for simulated HUD input")

    def test_toolbar_opens_play_control_without_starting_preview(self) -> None:
        for path in (
            "SparkEditor/Source/Core/EditorUI.h",
            "SparkEditor/Source/Core/EditorUI.cpp",
            "SparkEditor/Source/Core/EditorMenuBar.cpp",
            "SparkEditor/Source/Core/EditorCommandRegistry.cpp",
        ):
            self.assertIsNone(re.search(r"\bm_playMode\b", source(path)), f"duplicated play state remains in {path}")
        menu = source("SparkEditor/Source/Core/EditorMenuBar.cpp")
        toolbar = menu.split("void EditorUI::RenderToolbarPlayControls", 1)[1].split(
            "void EditorUI::RenderToolbarSnapControls", 1)[0]
        self.assertIn('ImGui::Button(ICON_FA_PLAY "##OpenPlayControl"', toolbar)
        self.assertIn('SetPanelVisible("PlayControl", true)', toolbar)
        self.assertNotIn("TogglePlayMode", toolbar)

    def test_debug_panels_do_not_promise_state_preview_runtime_data(self) -> None:
        for panel, truth in (
            ("AIDebugPanel.cpp", "State preview does not tick AI"),
            ("AIEditorPanel.cpp", "State preview does not tick AI"),
            ("AudioMixerPanel.cpp", "State preview does not run game audio"),
            ("CoroutineDebugPanel.cpp", "State preview does not run game coroutines"),
            ("ScriptEditorPanel.cpp", "State preview does not execute game scripts"),
            ("StreamingPanel.cpp", "State preview does not stream game areas"),
            ("VRConfigPanel.cpp", "State preview does not update VR tracking"),
        ):
            has_copy(self, f"SparkEditor/Source/Panels/{panel}", truth)


if __name__ == "__main__":
    unittest.main()
