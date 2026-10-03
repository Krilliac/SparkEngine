from dataclasses import dataclass
import importlib.util
from pathlib import Path
import unittest

script=Path(__file__).with_name('isa_failure_diagnostics.py')
if not script.is_file():
    script=Path(__file__).resolve().parents[2]/'.github/scripts/isa_failure_diagnostics.py'
spec=importlib.util.spec_from_file_location('collector',script)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
@dataclass
class Procedure:
    start:int
    end:int
    name:str
    ambiguous:bool=False
    noreturn:bool=False

def findings(*addresses):return [{'address':hex(a)[2:],'feature':'undecodable','symbol':'f','text':'<unknown>'} for a in addresses]

class ContractTests(unittest.TestCase):
    def test_all_74_findings_share_one_complete_region(self):
        f=findings(*range(0x1010,0x1010+74))
        regions=m.collect_regions(f,[Procedure(0x1000,0x1080,'f')],0x1000,
            [(0,128,128,32,0x20000000)],lambda offset,size:bytes(range(128)),lambda a,b:'complete disassembly')
        self.assertEqual(len(f),74)
        self.assertEqual(len(regions),1)
        self.assertEqual(regions[0]['fileOffset'],32)
        self.assertEqual(regions[0]['startRva'],0)
        self.assertEqual(bytes.fromhex(regions[0]['rawHex']),bytes(range(128)))
    def test_overlapping_pdb_extents_preserved(self):
        plan=m.region_plan(findings(15),[Procedure(10,20,'a',True),Procedure(12,24,'b',True)])
        self.assertEqual((plan[0]['start'],plan[0]['end']),(10,24))
        self.assertEqual(len(plan[0]['pdbExtents']),2)
    def test_uncovered_finding_captures_unclassified_context(self):
        regions=m.collect_regions(findings(3000),[Procedure(10,20,'same name')],0,
            [(0,6000,6000,100,0x20000000)],lambda offset,size:b'x'*size,lambda a,b:'raw context')
        region=regions[0]
        self.assertEqual((region['start'],region['end']),(1976,4025))
        self.assertEqual(region['pdbExtents'],[])
        self.assertEqual(len(bytes.fromhex(region['rawHex'])),2049)
        window=region['unclassifiedWindows'][0]
        self.assertIn('no matching PDB extent',window['classification'])
        self.assertEqual(window['adjacentParsedExtents'][0]['name'],'same name')
    def test_uncovered_window_clips_at_both_section_edges(self):
        regions=m.region_plan(findings(1000,1099),[],1000,[(0,100,100,0,0x20000000)])
        self.assertEqual(len(regions),1)
        self.assertEqual((regions[0]['start'],regions[0]['end']),(1000,1100))
        self.assertEqual(len(regions[0]['unclassifiedWindows']),2)
    def test_uncovered_ambiguous_or_missing_section_fails(self):
        section=(0,100,100,0,0x20000000)
        for sections in ([],[section,section]):
            with self.subTest(sections=sections),self.assertRaisesRegex(ValueError,'unique executable'):
                m.region_plan(findings(1),[],0,sections)
    def test_overlapping_window_and_extent_preserve_all_facts(self):
        regions=m.region_plan(findings(1500,2100),[Procedure(2000,2200,'f')],0,
            [(0,5000,5000,0,0x20000000)])
        self.assertEqual(len(regions),1)
        self.assertEqual(len(regions[0]['pdbExtents']),1)
        self.assertEqual(regions[0]['unclassifiedWindows'][0]['findingAddress'],1500)
    def test_merged_context_does_not_evade_size_cap(self):
        with self.assertRaisesRegex(ValueError,'bound'):
            m.region_plan(findings(1,65536),[Procedure(0,65536,'f')],0,[(0,70000,70000,0,0x20000000)])
    def test_region_and_aggregate_bounds(self):
        with self.assertRaisesRegex(ValueError,'bound'):m.region_plan(findings(1),[Procedure(0,m.MAX_REGION+1,'f')])
        procs=[Procedure(i*100000,i*100000+m.MAX_REGION,str(i)) for i in range(3)]
        with self.assertRaisesRegex(ValueError,'bound'):m.region_plan(findings(*(p.start for p in procs)),procs)
    def test_region_count_bound(self):
        procs=[Procedure(i*20,i*20+10,str(i)) for i in range(9)]
        with self.assertRaisesRegex(ValueError,'bound'):m.region_plan(findings(*(p.start for p in procs)),procs)
    def test_ambiguous_file_mapping_rejected(self):
        section=(0,20,20,0,0x20000000)
        with self.assertRaisesRegex(ValueError,'unique'):
            m.collect_regions(findings(1),[Procedure(0,10,'f')],0,[section,section],lambda a,b:b'0'*b,lambda a,b:'text')
    def test_truncated_bytes_fail(self):
        with self.assertRaisesRegex(ValueError,'Truncated'):
            m.collect_regions(findings(1),[Procedure(0,10,'f')],0,[(0,20,20,0,0x20000000)],lambda a,b:b'',lambda a,b:'text')
    def test_disassembly_cannot_be_silently_truncated(self):
        for text in ('','x'*(256*1024+1)):
            with self.subTest(size=len(text)),self.assertRaises(ValueError):
                m.collect_regions(findings(1),[Procedure(0,10,'f')],0,[(0,20,20,0,0x20000000)],lambda a,b:b'0'*b,lambda a,b:text)
    def test_final_json_cap(self):
        with self.assertRaisesRegex(ValueError,'1 MiB'):m.encoded({'proof':'x'*m.CAP})
    def test_va_rva_file_offset_are_not_interchangeable(self):
        seen=[]
        def read(offset,size):
            seen.append((offset,size));return b'x'*size
        region=m.collect_regions(findings(0x180001018),[Procedure(0x180001010,0x180001020,'f')],
            0x180000000,[(0x1000,64,64,512,0x20000000)],read,lambda a,b:'text')[0]
        self.assertEqual(seen,[(528,16)])
        self.assertEqual(region['startRva'],0x1010)
        self.assertEqual(region['imageBase'],0x180000000)

    def test_diagnostic_completion_preserves_failed_scan(self):
        record={'scanExitCode':1,'findings':[{'feature':'undecodable'}]}
        self.assertEqual(m.finish_diagnostics(record,[],[]),1)
        self.assertTrue(record['diagnosticComplete'])
        self.assertEqual(record['findings'],[{'feature':'undecodable'}])

if __name__=='__main__':unittest.main()
