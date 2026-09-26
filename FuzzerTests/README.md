# FuzzerTests

libFuzzer targets for SparkEngine's untrusted-input parsers, kept apart from the unit suite in `Tests/`.

- `*.cpp`, `CMakeLists.txt`: harnesses and production adapters, built when `SPARK_ENABLE_FUZZ_TARGETS=ON` (Linux Clang).
- `corpora/<parser>/`: reviewed seeds and `regression-*` reproducers, pinned by `tools/fuzz-policy/corpus-manifest.json`. The blocking smoke replays exactly these.
- `generated/<parser>/`: coverage-minimized units kept from earlier fuzzing. The scheduled campaign reads them as a read-only second corpus. Refresh them only with `-merge=1`.
- `policy/`: the adversarial policy and campaign tests run by the `fuzz-policy` CI job.

```bash
cmake -S tools/fuzz-policy -B build/fuzz-policy        # CC=clang CXX=clang++ CXXFLAGS=-stdlib=libstdc++
cmake --build build/fuzz-policy --target check-fuzz-policy
ctest --test-dir build/fuzz-policy -L '^fuzz$'          # deterministic seed replay
python3 tools/fuzz-policy/run_campaign.py --build-dir build/fuzz-policy --output /tmp/fuzz-campaign --seconds 30
```

Details, including the layout and refresh procedure: [wiki/advanced/Fuzz-Policy-and-Parser-Security.md](../wiki/advanced/Fuzz-Policy-and-Parser-Security.md).
