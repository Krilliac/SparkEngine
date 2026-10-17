#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the event-response rule file reader.

Each seed is a rules file as EventResponseSystem::LoadFromJson reads it: an object whose
"rules" array holds {"name", "sourceEntityId", "trigger", "triggerParam", "enabled",
"oneShot", "actions": [{"type", "params": [...]}]} objects (the shape SaveToJson writes), or
a damaged variant. The fuzz adapter (FuzzEventResponseProduction.cpp) parses each one with
ParseEventResponseRules through the vendored nlohmann backend engine builds use.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "event-response-definitions"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-rules.json": b"""{
  "rules": [
    {"name": "Door Open", "sourceEntityId": 12, "trigger": "OnTriggerEnter", "triggerParam": "door_zone",
     "enabled": true, "oneShot": false,
     "actions": [{"type": "PlayAnimation", "params": ["door_open"]}, {"type": "PlaySound", "params": ["creak.wav"]}]},
    {"name": "Tick", "trigger": "OnTimer", "triggerParam": "2.5",
     "actions": [{"type": "SetWorldVariable", "params": ["ticks", 1]}]},
    {"name": "Welcome", "trigger": "OnStart", "oneShot": true,
     "actions": [{"type": "ShowMessage", "params": ["Hello"]}, {"type": "Delay", "params": [0.75]},
                 {"type": "SetPosition", "params": [1.5, -2, 3e2]}]}
  ]
}
""",
        "valid-save-format.json": b"""{
  "rules": [
    {
      "name": "Saved",
      "sourceEntityId": 0,
      "trigger": "OnCustom",
      "triggerParam": "boss_dead",
      "enabled": true,
      "oneShot": true,
      "actions": [
        { "type": "SetWorldFlag", "params": ["boss", true] },
        { "type": "TeleportEntity", "params": [4, 10.25, 0.5, -7] }
      ]
    }
  ]
}
""",
        "entity-ids.json": b'{"rules":[{"name":"zero","sourceEntityId":0},{"name":"max","sourceEntityId":4294967295},'
        b'{"name":"above-int","sourceEntityId":3000000000},{"name":"fraction","sourceEntityId":2.5},'
        b'{"name":"text","sourceEntityId":"9"}]}',
        "unknown-kinds.json": b'{"rules":[{"name":"x","trigger":"OnNothing","actions":[{"type":"Explode"},{"params":[1]}]}]}',
        "non-object-entries.json": b'{"rules":[1,"two",null,[3],{"name":"ok","actions":[4,"five",{"type":"Delay"}]}]}',
        "param-kinds.json": b'{"rules":[{"name":"p","actions":[{"type":"ShowMessage","params":'
        b'[null,true,false,0,-0,-0.0,12345678901234,1e300,"s",[1,2],{"k":"v"}]}]}]}',
        "field-kinds.json": b'{"rules":[{"name":5,"trigger":["OnStart"],"triggerParam":{},"enabled":"yes",'
        b'"oneShot":1,"actions":{"type":"ShowMessage"}}]}',
        "bad-root.json": b'[{"rules":[]}]',
        "rules-not-array.json": b'{"rules":{"name":"lonely"}}',
        "truncated.json": b'{"rules":[{"name":"cut","actions":[{"type":"Show',
        "deep-nesting.json": b'{"rules":[{"name":"deep","actions":[{"type":"ShowMessage","params":['
        + b"[" * 200
        + b"]" * 200
        + b"]}]}]}",
        "unicode-strings.json": '{"rules":[{"name":"caf\\u00e9 \\ud83d\\ude80 雪","triggerParam":"a\\u0000b"}]}'.encode(
            "utf-8"
        ),
        "malformed-negative-entity-id.json": b'{"rules":[{"name":"wrapped","sourceEntityId":-1,"trigger":"OnKilled"}]}',
        "malformed-control-characters.json": b'{"rules":[{"name":"tab\\there\\r\\u0001","actions":'
        b'[{"type":"ShowMessage","params":["bell\\u0007"]}]}]}',
        "malformed-double-precision.json": b'{"rules":[{"name":"precise","actions":[{"type":"SetPosition",'
        b'"params":[1234567.25,0.1,18446744073709551616.0]}]}]}',
        "malformed-out-of-range-integer-param.json": b'{"rules":[{"name":"storm","trigger":"OnStart",'
        b'"actions":[{"type":"SetWeather","params":[1e300]}]}]}',
        "malformed-custom-event-cycle.json": b'{"rules":[{"name":"Echo","trigger":"OnCustom","triggerParam":"ping",'
        b'"actions":[{"type":"FireCustomEvent","params":["ping"]}]},'
        b'{"name":"Kick","trigger":"OnStart","actions":[{"type":"FireCustomEvent","params":["ping"]}]}]}',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
