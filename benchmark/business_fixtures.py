#!/usr/bin/env python3
"""Synthetic inputs and an independent result oracle for business.cpp.

Generate: python3 benchmark/business_fixtures.py --output /tmp/business-fixtures
Verify/recompute: python3 benchmark/business_fixtures.py --verify-manifest PATH
The latter writes a recomputed, headerless TSV to stdout and reports mismatches
on stderr (exit 1 on mismatches). It does not modify the supplied manifest.
"""

import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import sys

OFFSET = 14695981039346656037
PRIME = 1099511628211
MASK = (1 << 64) - 1
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1
MISSING = object()
PROFILES = ("native", "missing", "numeric_strings", "wrong_types")
SIZES = (1024, 4096, 16384)

# Each tuple is (document number, key path, result kind, default).
QUERIES = {
    "image": ((0, ("image_comment_id",), "int64", 0),),
    "permission": ((0, ("comment_permission",), "int32", 0),),
    "audio": ((0, ("cancel_no_audio_extract",), "int64", 0),),
    "user": (
        (0, ("hide_search",), "bool", False),
        (0, ("commerce_user_level",), "int32", 0),
        (0, ("commerce_role_bitmap",), "string", ""),
        (1, ("enable_friend_active",), "int32", 1),
        (1, ("favorite_permission",), "int32", 0),
    ),
    "music": (
        (0, ("is_image_beat",), "int32", 0),
        (0, ("music_video_category", "version"), "int64", 0),
        (0, ("music_video_category", "is_music"), "bool", False),
        (0, ("music_video_category", "score"), "double", 0.0),
        (0, ("luna_music_song",), "string", ""),
        (0, ("music_item_type",), "int64", 0),
        (0, ("diversion_bind_sid",), "int64", 0),
        (0, ("ecom_bar_gid_interverne_type",), "int64", 0),
    ),
    "edit_many": (
        (0, ("aweme_type",), "int64", 0),
        (0, ("is_meteor",), "bool", False),
        (0, ("music_edit_info", "status"), "int64", 0),
        (1, ("is_origin_text",), "bool", False),
        (1, ("is_story",), "bool", False),
        (1, ("task", "id"), "int64", 0),
        (1, ("task", "status"), "int64", 0),
        (1, ("task", "channel"), "string", ""),
        (1, ("mission_platform", "type"), "int64", 0),
        (1, ("mission_platform", "status"), "int64", 0),
        (1, ("star_atlas_order_id",), "int64", 0),
        (1, ("marketing_id",), "int64", 0),
    ),
    "edit_early": ((0, ("aweme_type",), "int64", 0),),
    "parse_item": (),
    "parse_user": (),
}
SCENARIOS = {
    "image": ("ParseItemExtra_GetOne", ("item_extra",)),
    "permission": ("ParseUserExtra_GetPermission", ("user_extra",)),
    "audio": ("ParseItemExtra_GetAudioFlag", ("item_extra",)),
    "user": ("ParseUserExtraAndGeneralExtra_GetFive", ("user_extra", "general_extra")),
    "music": ("ParseItemExtra_MusicMetadata", ("item_extra",)),
    "edit_many": ("ParseContentExtra_GetMany", ("content", "extra")),
    "edit_early": ("ParseContentExtra_EarlyQueryStop", ("content", "extra")),
    "edit_serialize": ("ParseExtra_SerializeEditInfo", ("extra",)),
    "parse_item": ("ParseItemExtra_NoQuery", ("item_extra",)),
    "parse_user": ("ParseUserExtraAndGeneralExtra_NoQuery", ("user_extra", "general_extra")),
}


def compact(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def lookup(document, path):
    for key in path:
        if not isinstance(document, dict) or key not in document:
            return MISSING
        document = document[key]
    return document


def integer(value, bits, default):
    if type(value) is str and re.fullmatch(r"-?[0-9]+", value):
        # Bound work and avoid Python's large-decimal conversion limit, while
        # still accepting arbitrarily many leading zeroes under this contract.
        negative = value.startswith("-")
        digits = value[int(negative):].lstrip("0") or "0"
        if len(digits) > 19:
            return default
        value = int(digits) * (-1 if negative else 1)
    if type(value) is not int:
        return default
    return value if -(1 << (bits - 1)) <= value < (1 << (bits - 1)) else default


def convert(value, kind, default):
    if kind in ("int64", "int32"):
        return integer(value, int(kind[3:]), default)
    if kind == "bool":
        return value if type(value) is bool else default
    if kind == "string":
        return value if type(value) is str else default
    if kind == "double":
        return float(value) if type(value) in (int, float) else default
    raise ValueError("unknown result kind: " + kind)


def fnv(data, state=OFFSET):
    for byte in data:
        state = ((state ^ byte) * PRIME) & MASK
    return state


def mix(state, kind, value):
    if kind == "string":
        encoded = value.encode("utf-8")
        return fnv(encoded, fnv(len(encoded).to_bytes(8, "little"), state))
    if kind == "double":
        return fnv(struct.pack("<d", value), state)
    return fnv((int(value) & MASK).to_bytes(8, "little"), state)


def check_edit_subtree(value):
    if value is None or type(value) in (str, bool):
        return
    if type(value) is int and INT64_MIN <= value <= MASK:
        return
    if isinstance(value, list):
        for child in value:
            check_edit_subtree(child)
        return
    if isinstance(value, dict):
        for child in value.values():
            check_edit_subtree(child)
        return
    raise ValueError("aweme_edit_info oracle requires integer/string/bool/null/array/object; "
                     "integers must fit int64 or uint64, and floats are excluded")


def expected_hash(scenario, documents):
    state = OFFSET
    if scenario == "edit_serialize":
        subtree = lookup(documents[0], ("aweme_edit_info",))
        if isinstance(subtree, dict):
            check_edit_subtree(subtree)
            text = compact(subtree)
        else:
            text = ""
        return mix(state, "string", text)
    for doc, path, kind, default in QUERIES[scenario]:
        state = mix(state, kind, convert(lookup(documents[doc], path), kind, default))
    return state


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate object keys are unsupported by this independent oracle: " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError("non-JSON numeric constant: " + value)


def check_values(value):
    if type(value) in (int, float):
        if not math.isfinite(float(value)):
            raise ValueError("input contains a number outside finite double range")
    elif type(value) is str:
        value.encode("utf-8")  # Reject isolated Unicode surrogates.
    elif isinstance(value, dict):
        for key, child in value.items():
            key.encode("utf-8")
            check_values(child)
    elif isinstance(value, list):
        for child in value:
            check_values(child)


def read_document(path):
    value = json.loads(path.read_bytes().decode("utf-8"),
                       object_pairs_hook=unique_object, parse_constant=reject_constant)
    check_values(value)
    return value


def native_targets(role):
    targets = {
        "item_extra": {
            "image_comment_id": 9007199254740993,
            "cancel_no_audio_extract": 1,
            "is_image_beat": 1,
            "music_video_category": {"version": 20260929, "is_music": True,
                                     "score": 0.875, "label": "音乐分类"},
            "luna_music_song": "月光🙂\n\"live\"\\mix",
            "music_item_type": 7,
            "diversion_bind_sid": 876543210123456789,
            "ecom_bar_gid_interverne_type": -3,
        },
        "user_extra": {
            "comment_permission": 2,
            "hide_search": True,
            "commerce_user_level": 4,
            "commerce_role_bitmap": "商家|creator\t\"verified\"\\v2",
        },
        "general_extra": {"enable_friend_active": 0, "favorite_permission": 2},
        "content": {"aweme_type": 68, "is_meteor": False,
                    "music_edit_info": {"status": 2, "note": "编辑记录"}},
        "extra": {
            "is_origin_text": True, "is_story": False,
            "task": {"id": 876543210123456789, "status": 2, "channel": "创作\nchannel🙂"},
            "mission_platform": {"type": 3, "status": 1},
            "star_atlas_order_id": INT64_MAX - 1, "marketing_id": -12345,
            "aweme_edit_info": {
                "revision": 7, "enabled": True, "reason": None,
                "title": "编辑🙂\n\"draft\"\\path\u0001",
                "steps": [{"id": 9007199254740993, "ok": False}, "旁白", 3, None],
                "nested": {"min": INT64_MIN, "max": MASK},
            },
        },
    }
    return targets[role]


def role_fields(role):
    result = {}
    for scenario, queries in QUERIES.items():
        roles = SCENARIOS[scenario][1]
        for doc, path, kind, default in queries:
            if roles[doc] == role:
                result[path] = (kind, default)
    return result


def set_path(value, path, replacement):
    for key in path[:-1]:
        value = value[key]
    value[path[-1]] = replacement


def profile_targets(role, profile, size_index):
    targets = native_targets(role)
    if profile == "missing":
        return {key: None for index, key in enumerate(targets)
                if (index + size_index) % 3 == 0}
    for index, (path, (kind, _)) in enumerate(role_fields(role).items()):
        value = lookup(targets, path)
        if profile == "numeric_strings":
            if kind in ("int64", "int32"):
                text = str(value)
                text = "-000" + text[1:] if text.startswith("-") else "000" + text
                set_path(targets, path, text)
            elif kind == "double":
                set_path(targets, path, str(value))
        elif profile == "wrong_types":
            wrong = {
                "int64": (True, 1.0, "12tail", [7], {"value": 1}, None),
                "int32": (False, 1.0, "+12", [7], {"value": 1}, None),
                "bool": ("true", 1), "string": (123, ["text"]),
                "double": ("0.875", True),
            }[kind]
            set_path(targets, path, wrong[(index + size_index) % len(wrong)])
    if profile == "wrong_types":
        if "music_video_category" in targets:
            targets["music_video_category"] = [1, 2, 3]
        if "mission_platform" in targets:
            targets["mission_platform"] = "wrong intermediate type"
        if "aweme_edit_info" in targets:
            targets["aweme_edit_info"] = ["not an object", 3]
    return targets


def unrelated(index):
    values = (
        {"tag": "旁支" + str(index), "ratio": index / 8 + 0.375, "ok": index % 2 == 0},
        [index, -index, 0.25, [True, None, "nested"]],
        "说明%d: 雪🙂 \"quote\"\\path\n第二行" % index,
        {"nested": {"leaf": index * 37, "value": "数据"}, "ids": [index, index + 1]},
        9007199254740993 + index,
        index / 16 + 0.125,
        index % 2 == 0,
        [{"kind": "play", "n": index}, {"kind": "stop", "n": index + 1}],
    )
    return values[index % len(values)]


def make_document(role, target_size, profile, size_index, overrides=None):
    targets = profile_targets(role, profile, size_index)
    if overrides:
        for path, replacement in overrides.items():
            set_path(targets, path, replacement)
    profile_index = PROFILES.index(profile)
    placement = (profile_index + size_index + ROLE_ORDER.index(role)) % 3
    target_items = list(targets.items())
    if target_items:
        rotate = (profile_index + size_index) % len(target_items)
        target_items = target_items[rotate:] + target_items[:rotate]
    extras = []
    while True:
        split = (0, len(extras) // 2, len(extras))[placement]
        document = dict(extras[:split] + target_items + extras[split:])
        data = compact(document).encode("utf-8")
        if len(data) >= target_size and len(extras) >= 8:
            break
        index = len(extras)
        extras.append(("unrelated_%04d" % index, unrelated(index)))
    positions = {}
    for path in role_fields(role):
        root_key = path[0]
        present = lookup(document, path) is not MISSING
        positions[".".join(path)] = {
            "present": present,
            "top_level_index": list(document).index(root_key) if root_key in document else None,
            "root_key_byte_offset": data.find((compact(root_key) + ":").encode())
                                    if root_key in document else None,
        }
    if role == "extra":
        positions["aweme_edit_info"] = {
            "present": "aweme_edit_info" in document,
            "top_level_index": list(document).index("aweme_edit_info")
                               if "aweme_edit_info" in document else None,
        }
    info = {"role": role, "profile": profile, "target_bytes": target_size,
            "actual_bytes": len(data), "unrelated_top_level_fields": len(extras),
            "target_placement": ("front", "middle", "back")[placement],
            "query_field_positions": positions,
            "sha256": hashlib.sha256(data).hexdigest()}
    return document, data, info


ROLE_ORDER = ("item_extra", "user_extra", "general_extra", "content", "extra")


def write_new_or_same(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if path.read_bytes() == data:
            return
        raise ValueError("refusing to overwrite different existing file: " + str(path))
    path.write_bytes(data)


def generate(output):
    documents, metadata, rows = {}, {}, []
    for size_index, target_size in enumerate(SIZES):
        for profile in PROFILES:
            for role in ROLE_ORDER:
                relative = "documents/%s_%d_%s.json" % (role, target_size, profile)
                value, data, info = make_document(role, target_size, profile, size_index)
                write_new_or_same(output / relative, data)
                documents[(role, target_size, profile)] = (relative, value)
                metadata[relative] = info
            for scenario, (label, roles) in SCENARIOS.items():
                entries = [documents[(role, target_size, profile)] for role in roles]
                name = "%s/synthetic_%dKiB_%s" % (label, target_size // 1024, profile)
                rows.append((name, scenario, entries[0][0],
                             entries[1][0] if len(entries) == 2 else "-",
                             str(expected_hash(scenario, [e[1] for e in entries]))))
    edges = (
        ("image", "int64_min", ("image_comment_id",), INT64_MIN),
        ("image", "int64_max", ("image_comment_id",), INT64_MAX),
        ("image", "int64_native_overflow", ("image_comment_id",), INT64_MAX + 1),
        ("image", "int64_string_overflow", ("image_comment_id",), str(INT64_MIN - 1)),
        ("permission", "int32_min", ("comment_permission",), -(1 << 31)),
        ("permission", "int32_max", ("comment_permission",), (1 << 31) - 1),
        ("permission", "int32_native_overflow", ("comment_permission",), 1 << 31),
        ("permission", "int32_string_overflow", ("comment_permission",), str(-(1 << 31) - 1)),
        ("music", "negative_zero", ("music_video_category", "score"), -0.0),
    )
    for scenario, edge, path, replacement in edges:
        label, (role,) = SCENARIOS[scenario]
        relative = "documents/edge_%s.json" % edge
        value, data, info = make_document(role, 1024, "native", 0, {path: replacement})
        info.update(profile="edge_" + edge, edge_path=".".join(path))
        write_new_or_same(output / relative, data)
        metadata[relative] = info
        rows.append((label + "/synthetic_1KiB_edge_" + edge, scenario,
                     relative, "-", str(expected_hash(scenario, [value]))))
    stream = io.StringIO(newline="")
    csv.writer(stream, delimiter="\t", lineterminator="\n").writerows(rows)
    write_new_or_same(output / "manifest.tsv", stream.getvalue().encode())
    info = {
        "synthetic": True, "production_distribution": False,
        "description": "Complete synthetic JSON documents; not sampled production traffic or wrapper-equivalent behavior.",
        "profiles": list(PROFILES), "target_bytes_per_document": list(SIZES),
        "sizing": "Approximate lower-bound sizes; complete structured fields are appended, never truncated. Two-document cases read both documents of the stated target size.",
        "placement": "Target field groups rotate front/middle/back with profile, size and role. This is a deterministic diagnostic distribution, not a production estimate.",
        "manifest_columns": ["case_name", "scenario", "input_a", "input_b", "expected_hash_decimal"],
        "case_count": len(rows), "document_count": len(metadata),
        "query_contract": {
            "int64": "Only JSON integer or complete decimal string matching -?[0-9]+, within signed 64-bit range; bool/float are excluded.",
            "int32": "Same conversion, additionally restricted to signed 32-bit range.",
            "bool": "Only JSON bool; default false.",
            "string": "Only JSON string; hash all UTF-8 bytes including embedded NUL; default empty.",
            "double": "JSON integer or finite float, excluding bool; default +0.0.",
            "defaults": "Missing/null/wrong type/out of range use defaults; enable_friend_active defaults to 1, all other integers to 0.",
            "edit_early": "Parse both full documents, then query only Content.aweme_type. Artificial query-stop diagnostic, not real CanEdit business logic.",
            "edit_many": "Representative typed query sequence only; not a real CanEdit decision.",
            "edit_serialize": "Parse Extra; compactly serialize object aweme_edit_info, else hash empty string. Subtree has no floats and only int64/uint64 integers.",
            "parse_only": "Parse one/two complete documents without queries; result remains FNV offset basis.",
            "verification_limits": "Reject duplicate object keys, invalid UTF-8/surrogates, nonfinite/out-of-range JSON numbers, and unsupported edit serialization subtrees rather than claiming equivalence.",
        },
        "hash_contract": {"algorithm": "FNV-1a-64", "initial": OFFSET,
                          "scalars": "Each integer modulo 2^64, bool 0/1, or IEEE binary64 bits is 8 little-endian bytes.",
                          "strings": "8-byte little-endian UTF-8 byte length followed by every UTF-8 byte."},
        "queries": {k: [{"document": doc, "path": list(path), "kind": kind, "default": default}
                         for doc, path, kind, default in v] for k, v in QUERIES.items()},
        "documents": metadata,
        "cases": [{"case_name": row[0], "scenario": row[1],
                   "input_bytes": [metadata[p]["actual_bytes"] for p in row[2:4] if p != "-"],
                   "expected_hash_decimal": row[4]} for row in rows],
    }
    write_new_or_same(output / "metadata.json", (json.dumps(info, ensure_ascii=False, indent=2) + "\n").encode())
    print("Generated %d synthetic cases, %d complete JSON documents: %s" %
          (len(rows), len(metadata), output / "manifest.tsv"), file=sys.stderr)


def verify(manifest):
    seen, cache, mismatches = set(), {}, 0
    writer = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    with manifest.open(encoding="utf-8", newline="") as source:
        for line, row in enumerate(csv.reader(source, delimiter="\t"), 1):
            if len(row) != 5:
                raise ValueError("manifest line %d: expected exactly five columns" % line)
            name, scenario, path_a, path_b, recorded = row
            if any(any(ch in field for ch in "\t\r\n") for field in row):
                raise ValueError("manifest fields cannot contain tabs or line breaks")
            if not name or name in seen or scenario not in SCENARIOS:
                raise ValueError("manifest line %d: duplicate/empty case name or unknown scenario" % line)
            seen.add(name)
            if not re.fullmatch(r"[0-9]+", recorded) or int(recorded) > MASK:
                raise ValueError("manifest line %d: invalid expected uint64 hash" % line)
            count = len(SCENARIOS[scenario][1])
            if not path_a or path_a == "-" or (count == 1) != (path_b == "-"):
                raise ValueError("manifest line %d: wrong document count" % line)
            values = []
            for relative in (path_a, path_b)[:count]:
                if not relative or Path(relative).is_absolute():
                    raise ValueError("manifest paths must be nonempty and relative to the manifest")
                path = manifest.parent / relative
                if path not in cache:
                    cache[path] = read_document(path)
                values.append(cache[path])
            actual = expected_hash(scenario, values)
            if actual != int(recorded):
                mismatches += 1
                print("hash mismatch %s: recorded=%s recomputed=%d" % (name, recorded, actual), file=sys.stderr)
            writer.writerow((name, scenario, path_a, path_b, str(actual)))
    if not seen:
        raise ValueError("manifest is empty")
    print("Recomputed %d cases; mismatches=%d" % (len(seen), mismatches), file=sys.stderr)
    return int(mismatches != 0)


def self_test():
    for bits in (32, 64):
        low, high, default = -(1 << (bits - 1)), (1 << (bits - 1)) - 1, 73
        for value in (low, high, 0, -1):
            assert integer(value, bits, default) == value
            assert integer(str(value), bits, default) == value
        for value in (low - 1, high + 1):
            assert integer(value, bits, default) == default
            assert integer(str(value), bits, default) == default
        for value in (None, True, False, 1.0, "", "-", "+1", " 1", "1 ", "1e2", "1.0", "1x", "１", [], {}):
            assert integer(value, bits, default) == default
        assert integer("-0001", bits, default) == -1
        assert integer("0" * 5000, bits, default) == 0
    assert convert(1, "bool", False) is False
    assert convert(True, "bool", False) is True
    assert convert("1.25", "double", 0.0) == 0.0
    assert convert(True, "double", 0.0) == 0.0
    assert convert(123, "string", "") == ""
    assert lookup({"a": []}, ("a", "b")) is MISSING
    assert fnv(b"hello") == 0xA430D84680AABD0B
    assert mix(OFFSET, "int64", 0) == 0xA8C7F832281A39C5
    assert mix(OFFSET, "int64", -1) == fnv(b"\xff" * 8)
    assert mix(OFFSET, "bool", True) == fnv(b"\x01" + b"\0" * 7)
    assert mix(OFFSET, "double", -0.0) != mix(OFFSET, "double", 0.0)
    assert mix(OFFSET, "string", "雪\0") == fnv(b"\x04" + b"\0" * 7 + "雪\0".encode())
    music = {"music_video_category": {"version": "0012", "is_music": True,
                                      "score": -0.0},
             "is_music": False, "score": 1.0}
    music_expected = OFFSET
    for kind, value in (("int32", 0), ("int64", 12), ("bool", True),
                        ("double", -0.0), ("string", ""), ("int64", 0),
                        ("int64", 0), ("int64", 0)):
        music_expected = mix(music_expected, kind, value)
    assert expected_hash("music", [music]) == music_expected
    assert expected_hash("edit_serialize", [{"aweme_edit_info": []}]) == mix(OFFSET, "string", "")
    assert expected_hash("user", [{}, {}]) == mix(mix(mix(mix(mix(OFFSET, "bool", False), "int32", 0), "string", ""), "int32", 1), "int32", 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--output", type=Path, help="Generate synthetic documents, manifest.tsv and metadata.json here")
    action.add_argument("--verify-manifest", type=Path, help="Recompute TSV to stdout; exit 1 if recorded hashes differ")
    parser.add_argument("--self-test", action="store_true", help="Run embedded boundary/hash assertions (also run before every action)")
    args = parser.parse_args()
    if not (args.output or args.verify_manifest or args.self_test):
        parser.error("specify --output, --verify-manifest, or --self-test")
    self_test()
    if args.output:
        generate(args.output)
    elif args.verify_manifest:
        return verify(args.verify_manifest)
    else:
        print("Self-test passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, OverflowError, UnicodeError) as error:
        print("error: " + str(error), file=sys.stderr)
        sys.exit(2)
