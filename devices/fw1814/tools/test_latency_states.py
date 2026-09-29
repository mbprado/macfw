import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import fw1814latencystates as b


def fixture(impulse_id=123, extra=0):
    ticks=[1000000000,1000100000,1000200000,1000300000,1014000000+extra,1015000000+extra,1016000000+extra]
    lines=['running at 48000 Hz','mach_timebase_numer=1 mach_timebase_denom=1',
           f'round_trip_seconds={.011+extra/1e9} callback_wall_seconds={.016+extra/1e9}',
           'trace-session='+json.dumps(dict(id=impulse_id,engine_cookie=42,engine_cookie_after=42,generation=7))]
    for i,name in enumerate(b.STAGES):
        point=dict(stage=name,id=impulse_id,tick=ticks[i],frame=99,offset=0,a=99,b=0,c=0,d=0,
                   sample_host=ticks[i]+(4000000 if i==0 else -1000000),queue=0,pcm_queue=0,callback_frames=192)
        lines.append('trace-stage='+json.dumps(point))
    for name,value in [('before',2),('after',4)]:
        lines.append('trace-counters-'+name+'='+json.dumps(dict(tick=1,coherent=True,tx_late=value,tx_roll_miss=0)))
    return '\n'.join(lines)


class StateTests(unittest.TestCase):
    def test_comparison_locates_extra_stage_and_preserves_roundtrip(self):
        fast,slow=b.parse(fixture(),123),b.parse(fixture(extra=70000000),123)
        self.assertEqual(fast['errors'],[])
        self.assertEqual(fast['electrical_ms'],11)
        self.assertEqual(slow['electrical_ms'],81)
        self.assertAlmostEqual(slow['intervals_ms']['tx_prepare -> capture_decode']-fast['intervals_ms']['tx_prepare -> capture_decode'],70)
        self.assertEqual(fast['counter_deltas']['tx_late'],2)
        fast['state']='fast';slow['state']='slow'
        with tempfile.TemporaryDirectory() as temporary:
            self.assertTrue(b.comparison([fast,slow],Path(temporary)))
            self.assertIn('tx_prepare -> capture_decode (+70.000 ms)',(Path(temporary)/'comparison.md').read_text())

    def test_stale_id_missing_marker_and_same_generation_restart_rejected(self):
        self.assertTrue(b.parse(fixture(),124)['errors'])
        text=fixture().replace('"engine_cookie_after": 42','"engine_cookie_after": 43')
        self.assertTrue(b.parse(text,123)['errors'])
        text='\n'.join(line for line in fixture().splitlines() if '"stage": "pcm_read"' not in line)
        parsed=b.parse(text,123)
        self.assertIsNone(parsed['intervals_ms']['pcm_read -> tx_prepare'])
        self.assertTrue(parsed['errors'])

    def test_counter_reset_and_incoherent_snapshot_are_missing(self):
        text=fixture().replace('"tx_late": 4','"tx_late": 1')
        self.assertIsNone(b.parse(text,123)['counter_deltas']['tx_late'])
        text=fixture().replace('"coherent": true','"coherent": false')
        self.assertIsNone(b.parse(text,123)['counter_deltas']['tx_late'])

    def test_receive_arrival_requires_valid_span_and_clock_bracket(self):
        # Compact OHCI receive timestamps, full cycle-timer anchor, 84 cycles.
        point=dict(a=100,last_a=184,cycle_timer=200<<12,cycle_host=1000000000,
                   cycle_uncertainty=1000,tick=1001000000)
        result=b.arrival(point,1e-6)
        self.assertEqual(result['format'],'compact OHCI timestamp')
        self.assertAlmostEqual(result['tick'],987500000)
        point['last_a']=101
        self.assertIsNone(b.arrival(point,1e-6)['tick'])
        point['cycle_uncertainty']=2000000
        self.assertIsNone(b.arrival(point,1e-6)['tick'])

    def test_collector_stops_at_two_states_without_rate_profile_or_restart(self):
        calls=[]; probes=[0]
        def run(argv, **kwargs):
            calls.append(argv)
            text=''
            if '--trace-id' in argv:
                text=fixture(int(argv[argv.index('--trace-id')+1]),70000000 if probes[0] else 0)
                probes[0]+=1
            elif 'engine' in argv:
                text='sample rate: 48000 Hz; FireWire generation at control startup: 7'
            return SimpleNamespace(stdout=text,returncode=0)
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);log=root/'transport-source.log';log.write_text('')
            args=SimpleNamespace(output=root/'results',routing='fixture',host_load='simulated',probes=40,
                                 idle_gap=.25,fast_max_ms=20,slow_min_ms=60,output_channel=2)
            with patch.object(b,'LOG',log),patch.object(b.subprocess,'run',run),patch.object(b.time,'sleep',lambda _:None):
                self.assertEqual(b.run(args),0)
            self.assertEqual(probes[0],2)
            result=json.loads((args.output/'results.json').read_text())
            self.assertEqual([row['state'] for row in result],['fast','slow'])
            self.assertEqual(len(list(args.output.glob('probe-*.txt'))),2)
            self.assertTrue(all(argv[-2:]==['--output-channel','2']
                                for argv in calls if '--trace-id' in argv))
            for argv in calls:
                self.assertNotIn('--rate',argv)
                self.assertNotIn('set',argv)
                self.assertNotIn('launchctl',argv)

    def test_comparison_rejects_pair_from_different_engine_instances(self):
        fast,slow=b.parse(fixture(),123),b.parse(fixture(extra=70000000),123)
        fast['state']='fast';slow['state']='slow';slow['session']['engine_cookie']=43
        with tempfile.TemporaryDirectory() as temporary:
            self.assertFalse(b.comparison([fast,slow],Path(temporary)))
            self.assertIn('INCONCLUSIVE',(Path(temporary)/'comparison.md').read_text())

    def test_partial_raw_trace_returns_explicit_errors(self):
        parsed=b.parse(fixture()+'\ntrace-stage={"stage":',123)
        self.assertTrue(any('partial trace' in error for error in parsed['errors']))

    def test_frame_binding_mismatch_invalidates_chain(self):
        text=fixture().replace('"stage": "hal_delivery", "id": 123, "tick": 1015000000, "frame": 99',
                               '"stage": "hal_delivery", "id": 123, "tick": 1015000000, "frame": 100')
        self.assertIn('capture/HAL source frame mismatch',b.parse(text,123)['errors'])


if __name__=='__main__':unittest.main()
