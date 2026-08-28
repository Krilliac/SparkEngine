# SaveSystem compatibility fixtures

`v1-screenshotless.spark_save.hex` is an immutable byte-for-byte encoding of a
save emitted through the exact `SaveSystem::WriteToFile` v1 write path from
commit `e1ba1c12`. The generator was compiled with MSVC and populated a real
pre-v2 `Transform` serializer payload with non-default position, rotation, and
scale values. The fixture intentionally has no v2 `screenshotPath` line.

Readers may copy and load this artifact, but compatibility tests must never
rewrite the checked-in hex or the copied v1 slot.
