# SaveSystem compatibility fixtures

`v1-screenshotless.spark_save.hex` is an immutable byte-for-byte encoding of a
save emitted through the exact `SaveSystem::WriteToFile` v1 write path from
commit `e1ba1c12`. The generator was compiled with MSVC and populated a real
pre-v2 `Transform` serializer payload with non-default position, rotation, and
scale values. The fixture intentionally has no v2 `screenshotPath` line.

`v2-screenshot-without-hierarchy.spark_save.hex` is the corresponding immutable
v2 disk fixture. It carries a screenshot path but intentionally omits the v3
`Transform.parent` property, so loading it exercises the v2-to-v3 root migration.

Readers may copy and load these artifacts, but compatibility tests must never
rewrite the checked-in hex or the copied legacy slot.
