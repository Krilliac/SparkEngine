# SaveSystem compatibility fixtures

Owner decision OD-03 limits readers to the current save version (N = v4) and
the previous one (N-1 = v3). `v3-fps-profile.spark_save.hex` is the N-1 fixture
that must migrate; the v1 and v2 fixtures are kept as real pre-window files that
every read path must refuse without mutating caller state or the file.

`v1-screenshotless.spark_save.hex` is an immutable byte-for-byte encoding of a
save emitted through the exact `SaveSystem::WriteToFile` v1 write path from
commit `e1ba1c12`. The generator was compiled with MSVC and populated a real
pre-v2 `Transform` serializer payload with non-default position, rotation, and
scale values. The fixture intentionally has no v2 `screenshotPath` line.

`v2-screenshot-without-hierarchy.spark_save.hex` is the corresponding immutable
v2 disk fixture. It carries a screenshot path but intentionally omits the v3
`Transform.parent` property. It is N-2 under OD-03 and must fail closed.

`v3-fps-profile.spark_save.hex` is an immutable 355-byte save emitted by the
production v3 writer in the installed MinSizeRel SparkGameFPS qualification at
commit `88b648db52670bc63dbbd8f3ee40fc17c65b54bd`. It carries the complete FPS
local-profile custom state used by that two-process proof (including 37 XP), and
its binary SHA-256 is
`0c7ee57bf47b1b2483e5eccb725c99223e66a46619311e7d0cf365454a36bae1`.
It is the frozen N-1 fixture for the v4 integrity-envelope migration.

Readers may copy and load these artifacts, but compatibility tests must never
rewrite the checked-in hex or the copied legacy slot.
