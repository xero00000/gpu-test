"""Verify the arithmetic and redaction of the published measurement receipts."""
import hashlib
import json
from pathlib import Path
import re
import statistics
import unittest

R = Path(__file__).resolve().parents[1] / 'results/2026-10-24-p2p'
def load(name): return json.loads((R/name).read_text())
def rows(name): return [json.loads(x) for x in (R/name).read_text().splitlines()]

class ReceiptTests(unittest.TestCase):
    def test_published_hashes_and_identifier_redaction(self):
        manifest=load('manifest.json')
        for name,digest in manifest['published_sha256'].items():
            data=(R/name).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(),digest,name)
            text=data.decode()
            self.assertNotRegex(text,r'GPU-[0-9a-fA-F]{8}-')
            self.assertNotIn('/home/',text)
            self.assertNotIn('root=UUID=',text)
            self.assertNotRegex(text,r'0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]')

    def test_copy_comparison_matches_actual_records(self):
        before=rows('copies-installed.jsonl');after=rows('copies-private.jsonl')
        for records in [before,after]:
            measured=[r for r in records if r['type']=='measurement']
            self.assertEqual(len(measured),24)
            self.assertTrue(all(r['correct'] and r['min_us']<=r['median_us']<=r['max_us'] for r in measured))
        for row in load('copy-comparison.json'):
            def find(records,mode):
                return next(x for x in records if x.get('mode')==mode and all(x[k]==row[k] for k in ['source','destination','bytes']))
            a=find(before,'runtime_peer_copy_unenabled');b=find(after,'runtime_peer_copy_enabled')
            self.assertEqual(row['before_GBps'],a['GBps'])
            self.assertEqual(row['after_GBps'],b['GBps'])
            self.assertAlmostEqual(row['change_percent'],100*(b['GBps']/a['GBps']-1))
        kernels=[r for r in rows('remote-kernels.jsonl') if r['type']=='kernel_check']
        self.assertEqual(len(kernels),4)
        self.assertTrue(all(r['correct'] and r['source_unchanged'] for r in kernels))

    def test_ese_request_rates_and_medians_match(self):
        data=load('ese-comparison.json')
        for label in ['original','system','private']:
            requests=data['runs'][label]['requests']
            self.assertEqual(len(requests),6)
            for row in requests.values():
                self.assertEqual(row['output_tokens'],32)
                self.assertEqual(row['decode_calls'],31)
                self.assertAlmostEqual(row['decode_tok_s'],31/row['decode_s'])
            for prompt in ['short','p128']:
                median=statistics.median(v['decode_tok_s'] for k,v in requests.items() if k.startswith(prompt+'-'))
                self.assertEqual(median,data['runs'][label]['medians'][prompt]['decode_tok_s'])
        for prompt,row in data['comparison'].items():
            self.assertAlmostEqual(row['decode_change_percent'],100*(row['private_decode_tok_s']/row['system_decode_tok_s']-1))

if __name__=='__main__': unittest.main()
