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

    def test_global_play_entries_name_the_preview(self) -> None:
        has_copy(self, "SparkEditor/Source/Core/EditorMenuBar.cpp", "State preview only (F5); use Play Control > Launch Game for gameplay")
        has_copy(self, "SparkEditor/Source/Core/EditorUI.cpp", "State preview active (no gameplay tick)")
        has_copy(self, "SparkEditor/Source/Core/EditorUI.cpp", "Simulation preview active (no gameplay tick)")
        has_copy(self, "SparkEditor/Source/Core/EditorCommandRegistry.cpp", "Preview scene state")

    def test_real_game_launcher_remains_separate(self) -> None:
        path = "SparkEditor/Source/Panels/PlayControlPanel.cpp"
        has_copy(self, path, "In-editor play is not offered")
        has_copy(self, path, 'ImGui::Button("Launch Game")')

    def test_game_view_calls_out_the_preview(self) -> None:
        path = "SparkEditor/Source/Panels/GameViewPanel.cpp"
        has_copy(self, path, "F5: state preview only (no gameplay)")
        has_copy(self, path, "Click for simulated HUD input")

    def test_toolbar_uses_manager_as_single_source_of_play_state(self) -> None:
        for path in (
            "SparkEditor/Source/Core/EditorUI.h",
            "SparkEditor/Source/Core/EditorUI.cpp",
            "SparkEditor/Source/Core/EditorMenuBar.cpp",
            "SparkEditor/Source/Core/EditorCommandRegistry.cpp",
        ):
            self.assertIsNone(re.search(r"\bm_playMode\b", source(path)), f"duplicated play state remains in {path}")
        menu = source("SparkEditor/Source/Core/EditorMenuBar.cpp")
        self.assertIn("m_playModeManager.IsInPlayMode()", menu)
        self.assertIn("m_playModeManager.ExitPlayMode()", menu)

    def test_debug_panels_do_not_promise_f5_runtime_data(self) -> None:
        for panel, truth in (
            ("AIDebugPanel.cpp", "F5 preview does not tick AI"),
            ("AIEditorPanel.cpp", "F5 preview does not tick AI"),
            ("AudioMixerPanel.cpp", "F5 preview does not run game audio"),
            ("CoroutineDebugPanel.cpp", "F5 preview does not run game coroutines"),
            ("ScriptEditorPanel.cpp", "F5 preview does not execute game scripts"),
            ("StreamingPanel.cpp", "F5 preview does not stream game areas"),
            ("VRConfigPanel.cpp", "F5 preview does not update VR tracking"),
        ):
            has_copy(self, f"SparkEditor/Source/Panels/{panel}", truth)


if __name__ == "__main__":
    unittest.main()
