#!/usr/bin/env python3
"""Independent JSON/typed-value/FNV oracle for mixed_scratch --dump-dir output.

Does not import the C++ fixture generator or execute benchmark binaries.
Optional --csv paths validate fixed/duration-run checksums without using timing.
"""
import argparse
import csv
import json
from pathlib import Path

MASK = (1 << 64) - 1
OFFSET = 14695981039346656037
PRIME = 1099511628211


def require(ok, message):
    if not ok:
        raise ValueError(message)


def fnv(data, initial=OFFSET):
    value = initial
    for byte in data:
        value = ((value ^ byte) * PRIME) & MASK
    return value


class Digest:
    def __init__(self):
        self.value = OFFSET

    def integer(self, value):
        self.value = fnv((int(value) & MASK).to_bytes(8, 'little'), self.value)

    def string(self, value):
        raw = value.encode('utf-8')
        self.integer(len(raw))
        self.value = fnv(raw, self.value)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate object key')
        result[key] = value
    return result


def parse(raw):
    def reject_constant(value):
        raise ValueError('non-finite constant: ' + value)
    return json.loads(raw.decode('utf-8'), object_pairs_hook=unique_object,
                      parse_constant=reject_constant)


def typed_equal(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(typed_equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(typed_equal(a, b) for a, b in zip(left, right))
    return left == right


def rows(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream, delimiter='\t'))


def expected_result(kind, raw):
    digest = Digest()
    if kind == 'malformed':
        try:
            parse(raw)
        except json.JSONDecodeError:
            pass
        else:
            raise ValueError('malformed fixture is valid JSON')
        digest.integer(0)
        digest.integer(len(raw))
        return digest.value, None
    value = parse(raw)
    digest.integer(1)
    if kind == 'object':
        require(type(value) is dict and type(value['meta']) is dict, 'object type')
        meta, records = value['meta'], value['rows']
        require(type(meta['id']) is int and meta['id'] >= 0, 'id type')
        require(type(meta['active']) is bool and type(meta['label']) is str, 'meta field type')
        require(type(records) is list and records, 'rows type/empty')
        for index, record in enumerate(records):
            require(type(record) is dict and type(record['seq']) is int and record['seq'] == index,
                    'rows sequence')
            require(typed_equal(record['numbers'], [-7, 0, 42, 0.125]), 'mixed numeric fixture')
            require(record['extra']['none'] is None and record['extra']['flag'] is False,
                    'nested fixture values')
        digest.integer(1)
        digest.integer(meta['id'])
        digest.integer(meta['active'])
        digest.string(meta['label'])
        digest.integer(len(records))
        digest.integer(records[-1]['seq'])
    elif kind == 'dense':
        require(type(value) is list and len(value) == 4096, 'dense array size')
        require(all(type(item) is int and item == index % 10 for index, item in enumerate(value)),
                'dense array values')
        digest.integer(2)
        for item in (len(value), value[0], value[len(value) // 2], value[-1]):
            digest.integer(item)
    else:
        raise ValueError('unknown fixture kind: ' + kind)
    return digest.value, value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump-dir', type=Path, required=True)
    parser.add_argument('--csv', type=Path, action='append', default=[])
    args = parser.parse_args()
    require(fnv(b'a') == 0xaf63dc4c8601ec8c, 'FNV self-check')
    fixtures = {}
    for entry in rows(args.dump_dir / 'fixtures.tsv'):
        name = entry['name']
        require(name not in fixtures, 'duplicate fixture')
        raw = (args.dump_dir / entry['input_json']).read_bytes()
        require(fnv(raw) == int(entry['input_hash']), 'input byte hash: ' + name)
        result, value = expected_result(entry['kind'], raw)
        require(result == int(entry['expected_result_hash']), 'query digest: ' + name)
        if entry['kind'] == 'malformed':
            require(entry['canonical_json'] == '-' and entry['canonical_hash'] == '0', 'invalid canonical entry')
        else:
            canonical = (args.dump_dir / entry['canonical_json']).read_bytes()
            require(fnv(canonical) == int(entry['canonical_hash']), 'canonical byte hash: ' + name)
            require(typed_equal(value, parse(canonical)), 'full parsed DOM differs: ' + name)
        fixtures[name] = {'hash': result, 'bytes': len(raw), 'failed': entry['kind'] == 'malformed'}
    require(len(fixtures) == 10, 'expected ten complete fixture inputs')
    require(fixtures['object_over_1MiB']['bytes'] >= 1024 * 1024 and
            fixtures['object_over_1MiB_truncated']['bytes'] >= 1024 * 1024, 'large fixture too short')
    sequences = {}
    for entry in rows(args.dump_dir / 'sequences.tsv'):
        sequence = sequences.setdefault(entry['case'], [])
        require(int(entry['position']) == len(sequence), 'noncontiguous sequence positions')
        require(entry['fixture'] in fixtures, 'unknown sequence fixture')
        sequence.append(entry['fixture'])
    require(len(sequences) == 4, 'expected four sequences')
    for name, order in sequences.items():
        if 'recovery' in name:
            for index, fixture in enumerate(order):
                if fixtures[fixture]['failed']:
                    require(not fixtures[order[(index + 1) % len(order)]]['failed'], 'failure not followed by recovery')
    checked_csv_rows = 0
    for path in args.csv:
        with path.open(newline='') as stream:
            results = list(csv.DictReader(stream))
        require(bool(results), 'empty result CSV')
        seen = set()
        for entry in results:
            name = entry['case']
            require(name in sequences and name not in seen, 'unknown or duplicate CSV sequence')
            seen.add(name)
            order = sequences[name]
            cycle = [fixtures[item] for item in order]
            count = int(entry['iterations'])
            require(count > 0, 'invalid iteration count')
            repeats, remainder = divmod(count, len(cycle))
            expected = {}
            for field in ('hash', 'bytes', 'failed'):
                expected[field] = sum(item[field] for item in cycle) * repeats + sum(item[field] for item in cycle[:remainder])
            require(int(entry['cycle_documents']) == len(cycle), 'period mismatch')
            require(int(entry['cycle_input_bytes']) == sum(item['bytes'] for item in cycle), 'cycle byte count')
            require(int(entry['cycle_failures']) == sum(item['failed'] for item in cycle), 'cycle failure count')
            require(int(entry['checksum']) == (expected['hash'] & MASK), 'measured checksum mismatch')
            require(int(entry['processed_bytes']) == expected['bytes'], 'processed byte count')
            require(int(entry['parse_failures']) == expected['failed'], 'failure count')
            digest = Digest()
            digest.integer(len(cycle))
            for item in cycle:
                digest.integer(item['hash'])
            require(int(entry['result_hash']) == digest.value, 'fixed sequence hash')
            checked_csv_rows += 1
    print(json.dumps({'fixtures': len(fixtures), 'sequences': len(sequences),
                      'csv_rows_verified': checked_csv_rows,
                      'independent_full_json_and_query_hash': 'PASS'}, sort_keys=True))


if __name__ == '__main__':
    main()
