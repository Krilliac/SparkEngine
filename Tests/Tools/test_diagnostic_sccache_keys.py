"""Diagnostic workflow cache keys must be reachable without crossing toolsets/configs."""
import copy
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('combined-source-qualification.yml', 'primary-native-qualification.yml')


def steps(document):
    restores = [s for j in document['jobs'].values() for s in j['steps'] if s.get('id') == 'sccache-restore']
    saves = [s for j in document['jobs'].values() for s in j['steps'] if s.get('name') == 'Save sccache cache']
    if len(restores) != 1 or len(saves) != 1:
        raise ValueError('Expected exactly one restore/save pair')
    return restores[0], saves[0]


def render(text, config='Release', revision='new', build_hash='hash'):
    return (text.replace('${{ matrix.config }}', config)
            .replace("${{ hashFiles('**/CMakeLists.txt', '**/*.cmake') }}", build_hash)
            .replace('${{ github.sha }}', revision))


def validate(document):
    restore, save = steps(document)
    if save['with']['key'] != '${{ steps.sccache-restore.outputs.cache-primary-key }}':
        raise ValueError('Save must use the exact primary-key output')
    if save['with']['path'] != restore['with']['path']:
        raise ValueError('Save and restore must use the same cache path')
    primary = render(restore['with']['key'])
    prefixes = [render(p) for p in restore['with']['restore-keys'].splitlines() if p]
    if not any(primary.startswith(p) for p in prefixes):
        raise ValueError('Saved key family is unreachable through restore prefixes')
    for incompatible in (render(restore['with']['key'], config='Debug'),
                         primary.replace('vs2026', 'vs2022'), 'combined-' + primary):
        if any(incompatible.startswith(p) for p in prefixes):
            raise ValueError('Restore prefix crosses a configuration/toolset or malformed family')


class SccacheKeyTests(unittest.TestCase):
    def documents(self):
        for name in NAMES:
            yield yaml.safe_load((ROOT / '.github/workflows' / name).read_text())

    def test_repository_save_restore_contract(self):
        for document in self.documents():
            validate(document)

    def test_prior_doubled_save_key_is_rejected(self):
        for document in self.documents():
            _, save = steps(document)
            save['with']['key'] = 'combined-' + save['with']['key']
            with self.assertRaisesRegex(ValueError, 'exact primary-key'):
                validate(document)

    def test_unreachable_or_cross_scope_restore_is_rejected(self):
        for original in self.documents():
            for prefixes in ('sccache-windows-vs2026-Release-', 'combined-sccache-windows-'):
                document = copy.deepcopy(original)
                restore, _ = steps(document)
                restore['with']['restore-keys'] = prefixes
                with self.assertRaises(ValueError):
                    validate(document)


if __name__ == '__main__':
    unittest.main()
