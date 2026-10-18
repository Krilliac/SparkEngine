# MOD-340: the installed SparkGamePlatformer completes level 0 ("Green Hills") within three minutes under the
# automated player. Included by Tests/PackageSmoke/RunInstalledModuleObjective.cmake (grammar documented there).
#
# run: reload level 0, hand the player to PlatformerRouteRunner (platformer_autoplay on), and at 180 s of real
#   loop time require level 0 recorded as done, every level 0 checkpoint activated, and a level timer (simulated
#   time of the completed run) of at most 180 s.

set(SPARK_OBJECTIVE_PHASES run)

set(SPARK_OBJECTIVE_run_SCRIPT "1 platformer_level 0
2 platformer_autoplay on
t180 platformer_levels
t180 platformer_status
")
set(SPARK_OBJECTIVE_run_SECONDS 185)
set(SPARK_OBJECTIVE_run_EXPECT
    "platformer_level 0|Level 0 loaded"
    "platformer_autoplay on|Autoplay on \\(level 0 route\\)"
    "platformer_levels|Green Hills \\(11 platforms\\) \\[DONE\\]"
    "platformer_status|Current Level: 0\n"
    "platformer_status|Checkpoints: 3\n"
    "platformer_status|Level time: (([0-9]|[1-9][0-9]|1[0-7][0-9])\\.[0-9][0-9]|180\\.00)(\n|$)")
