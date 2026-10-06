"""Replay retained LLVM text/PE bytes only; never invoke a disassembler or compiler."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import unittest
from Tests.Tools.test_check_isa_baseline import checker

cm = checker.code_map
FIXTURE = Path(__file__).with_name('fixtures') / 'isa_escape_string_release.json'

class RetainedImage:
    def __init__(self, region, records, entered=False, masks=False):
        self.image_base = region['imageBase']
        self.start, self.end = region['start'], region['end']
        self.raw = bytes.fromhex(region['rawHex'])
        self.records, self.entered, self.masks = records, entered, masks
    def read(self, address, size):
        offset = address-self.start
        return self.raw[offset:offset+size] if 0 <= offset and offset+size <= len(self.raw) else None
    def redecode(self, start, end):
        # Every retained non-table instruction starts on a captured boundary.
        return [r for r in self.records if start <= r.address < end]
    def entered_within(self, start, end):
        return self.entered
    def table_masks_code(self, start, end):
        return self.masks
    def section_bounds(self, address):
        return (self.start,self.end) if self.start <= address < self.end else None

class RetainedReleaseTests(unittest.TestCase):
    def setUp(self):
        self.fixture=json.loads(FIXTURE.read_text(encoding='utf-8'))
        self.region=self.fixture['region']
        self.records=[r for _,r in checker._records(self.region['disassembly'].splitlines())]
    def mapped(self, records=None, **kwargs):
        records=self.records if records is None else records
        return cm.map_procedure(records,self.region['start'],self.region['end'],RetainedImage(self.region,records,**kwargs))
    def changed(self, address, **kwargs):
        return [replace(r,**kwargs) if r.address==address else r for r in self.records]
    def assert_rejected(self, records=None, **kwargs):
        mapped=self.mapped(records,**kwargs)
        self.assertEqual(mapped.tables,[])
        self.assertGreater(sum(r.mnemonic=='<undecodable>' for r in mapped.records),0)
    def test_exact_retained_bytes_and_all_74_findings(self):
        self.assertEqual(hashlib.sha256(bytes.fromhex(self.region['rawHex'])).hexdigest(),self.region['rawSha256'])
        self.assertEqual(self.region['byteCount'],553)
        unknown=[r for r in self.records if r.mnemonic=='<undecodable>']
        self.assertEqual([f'{r.address:x}' for r in unknown],[f['address'] for f in self.fixture['findings']])
        self.assertEqual(len(unknown),74)
    def test_retained_procedure_maps_only_proven_117_table_bytes(self):
        mapped=self.mapped()
        self.assertEqual([(t.start,t.end,t.kind) for t in mapped.tables],
            [(0x1800c8614,0x1800c8634,'jump'),(0x1800c8634,0x1800c8689,'index')])
        self.assertEqual(sum(t.end-t.start for t in mapped.tables),117)
        self.assertFalse(any(r.mnemonic=='<undecodable>' for r in mapped.records))
        self.assertFalse(any(checker.classify(r.mnemonic,r.operands,r.evex) for r in mapped.records))
    def test_strict_scanner_region_has_no_findings_after_mapping(self):
        proc=checker.Procedure(**self.region['pdbExtents'][0])
        pdb=checker.PdbInfo([], [proc], {}, frozenset({checker.REVIEWED_TOOLSET}))
        result=checker.scan_lines('retained-static-fixture',self.region['disassembly'].splitlines(),[],pdb,
                                  RetainedImage(self.region,self.records))
        self.assertEqual(result.violations,[])
        self.assertEqual(result.table_bytes,117)
    def test_unreachable_real_branch_still_rejects_in_final_confirmation(self):
        self.assert_rejected(self.changed(0x1800c8611,mnemonic='jmp',operands='0x1800c8614'))
    def test_real_reachable_branch_into_table_rejects(self):
        self.assert_rejected(self.changed(0x1800c84c7,operands='0x1800c8614'))
    def test_unknown_on_real_base_path_rejects(self):
        self.assert_rejected(self.changed(0x1800c84b4,mnemonic='<undecodable>',operands=''))
    def test_wrong_image_base_rejects(self):
        self.assert_rejected(self.changed(0x1800c84b4,annotation=self.region['imageBase']+8))
    def test_unbounded_selector_rejects(self):
        self.assert_rejected(self.changed(0x1800c84c4,mnemonic='testl',operands='%eax, %eax'))
    def test_structural_entry_into_table_rejects(self):
        self.assert_rejected(entered=True)
    def test_table_masking_code_rejects(self):
        self.assert_rejected(masks=True)
    def test_above_floor_real_instruction_survives_mapping(self):
        mapped=self.mapped(self.changed(0x1800c847d,mnemonic='aesenc',operands='%xmm0, %xmm1'))
        self.assertEqual(len(mapped.tables),2)
        self.assertTrue(any(checker.classify(r.mnemonic,r.operands,r.evex) for r in mapped.records))

    def test_dead_bad_base_predecessor_rechecked_after_bootstrap(self):
        # Replace the three-byte post-return NOP with a dead pop-rbp/jmp block.
        # Its branch targets live loop code, not table data: only the final
        # unfiltered base proof can reject this incoming bad-base path.
        records=[]
        for record in self.records:
            if record.address==0x1800c8611:
                records.extend([replace(record,size=1,mnemonic='popq',operands='%rbp'),
                    replace(record,address=record.address+1,size=2,mnemonic='jmp',operands='0x1800c85e1')])
            else:
                records.append(record)
        image=RetainedImage(self.region,records)
        offset=0x1800c8611-self.region['start']
        image.raw=image.raw[:offset]+bytes.fromhex('5d eb cd')+image.raw[offset+3:]
        self.assertEqual(len(cm.find_switch_tables(records,self.region['start'],self.region['end'],
                                                   image,reachable_only=True)),2)
        mapped=cm.map_procedure(records,self.region['start'],self.region['end'],image)
        self.assertEqual(mapped.tables,[])
        self.assertEqual(sum(r.mnemonic=='<undecodable>' for r in mapped.records),74)

if __name__=='__main__':unittest.main()
