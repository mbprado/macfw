#!/usr/bin/env python3
"""Capture fast/slow 48 kHz impulses on one engine, without changing its settings."""
import argparse
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import secrets
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / 'devices/fw1814/tools/fw1814audioloopback'
CTL = ROOT / 'devices/fw1814/tools/control/fw1814ctl/fw1814ctl'
LOG = Path('/Library/Logs/macfw-fw1814-transport.log')
STAGES = ['tool_submit','hal_submit','pcm_read','tx_prepare','capture_decode','hal_delivery','tool_delivery']
MAX64 = 2**64-1
FAULT = re.compile(r'rolling TX deadline missed|generation changed during|requesting engine restart|engine exited with status|transient startup faults; restarting')


def numeric(text, key):
    m = re.search(re.escape(key) + r'=([-+0-9.eE]+)', text)
    return float(m[1]) if m else None


def parse(text, impulse_id):
    points, session, counters = {}, {}, {}
    errors = []
    for line in text.splitlines():
        if not line.startswith(('trace-session=','trace-stage=','trace-counters-')):
            continue
        try:
            name,payload=line.split('=',1)
            data=json.loads(payload)
            if not isinstance(data,dict):raise ValueError('trace record is not an object')
            if name=='trace-session':session=data
            elif name=='trace-stage':points[data['stage']]=data
            else:counters[name.removeprefix('trace-counters-')]=data
        except (ValueError,KeyError) as exc:
            errors.append('malformed/partial trace record: '+str(exc))
    if session.get('id') != impulse_id or not session.get('engine_cookie') or session.get('engine_cookie') != session.get('engine_cookie_after'):
        errors.append('missing session/wrong ID/engine changed')
    if any(points.get(stage,{}).get('id') != impulse_id or not points.get(stage,{}).get('tick') for stage in STAGES):
        errors.append('missing or stale stage ID; no nearest/lifetime marker fallback')
    if any(any(key not in points.get(stage,{}) for key in ('frame','offset','a','sample_host')) for stage in STAGES):
        errors.append('missing stage metadata fields')
    scale = None
    numerator, denominator = numeric(text,'mach_timebase_numer'), numeric(text,'mach_timebase_denom')
    if numerator and denominator:
        scale = numerator/denominator/1e6
    else:
        errors.append('missing recorded Mach timebase')
    rt, wall = numeric(text,'round_trip_seconds'), numeric(text,'callback_wall_seconds')
    if rt is None or wall is None or not 0 < rt < 5 or not math.isfinite(rt):
        errors.append('no valid signal/round trip')
    m = re.search(r'running at ([0-9.]+) Hz',text)
    if not m or float(m[1]) != 48000:
        errors.append('wrong AUHAL rate')
    intervals = {}
    for left,right in zip(STAGES,STAGES[1:]):
        a,b = points.get(left,{}), points.get(right,{})
        value = None
        if scale and a.get('id')==impulse_id and b.get('id')==impulse_id and a.get('tick') and b.get('tick'):
            if b['tick'] >= a['tick']:
                value = (b['tick']-a['tick'])*scale
            else:
                errors.append(f'invalid order: {left} -> {right}')
        intervals[left+' -> '+right] = value
    if not errors:
        if any(not points[name].get('a',0)&2 for name in ('tool_submit','tool_delivery')):
            errors.append('CoreAudio callback lacks valid host-time flag')
        if points['hal_submit']['frame'] != points['pcm_read']['frame']:
            errors.append('HAL/PCM source frame mismatch')
        if points['pcm_read']['a'] + points['pcm_read']['offset'] != points['tx_prepare']['frame']:
            errors.append('PCM/TX source frame mismatch')
        if points['capture_decode']['frame'] != points['hal_delivery']['frame']:
            errors.append('capture/HAL source frame mismatch')
    deltas = {}
    before, after = counters.get('before',{}), counters.get('after',{})
    for key in set(before)|set(after):
        if key in ('tick','coherent'): continue
        deltas[key] = after[key]-before[key] if key in before and key in after and before.get('coherent') and after.get('coherent') and after[key]>=before[key] else None
    result = dict(id=impulse_id, session=session, stages=points, intervals_ms=intervals, errors=errors,
                  electrical_ms=rt*1000 if rt is not None else None, wall_ms=wall*1000 if wall is not None else None,
                  counters=counters, counter_deltas=deltas, ns_per_tick=scale*1e6 if scale else None)
    if not errors:
        tx, cap = points['tx_prepare'], points['capture_decode']
        result['intended_tx_tick'] = tx.get('c') if tx.get('c') and tx.get('a')!=MAX64 else None
        result['tx_prepare_to_intended_ms_estimate'] = (tx['c']-tx['tick'])*scale if result['intended_tx_tick'] else None
        result['capture_arrival_estimate'] = arrival(cap,scale)
        recv = result['capture_arrival_estimate'].get('tick')
        result['intended_TX_to_RX_arrival_ms_estimate'] = (recv-tx['c'])*scale if recv and result['intended_tx_tick'] else None
        result['RX_arrival_to_decode_ms_estimate'] = (cap['tick']-recv)*scale if recv else None
        result['output_sample_host_adjustment_ms'] = (points['tool_submit']['sample_host']-points['tool_submit']['tick'])*scale
        result['input_sample_host_adjustment_ms'] = (points['tool_delivery']['sample_host']-points['tool_delivery']['tick'])*scale
        result['wall_from_stage_points_ms'] = sum(intervals.values())
        result['electrical_from_stage_points_ms'] = (points['tool_delivery']['sample_host']-points['tool_submit']['sample_host'])*scale
    return result


def arrival(point, scale):
    """Conservative RX cycle-time correlation: require a full tagged span.

    DCL timestamps are kept raw. Accept either full cycle timer or compact OHCI
    representation only if the first/last packet span fits the 512-frame tag.
    No interpretation or no usable clock bracket means an explicit missing estimate.
    """
    if not point.get('cycle_host') or point.get('cycle_uncertainty',MAX64)*scale > 1:
        return {'status':'missing/uncertain cycle-time anchor','tick':None}
    first,last,timer = point['a'],point.get('last_a',0),point['cycle_timer']
    def full(raw):
        if ((raw>>12)&8191)>=8000 or (raw&4095)>=3072: return None
        return ((raw>>25)&127)*1e9+((raw>>12)&8191)*125000+(raw&4095)*125000/3072
    def compact(raw):
        if raw>65535 or (raw&8191)>=8000: return None
        return ((raw>>13)&7)*1e9+(raw&8191)*125000
    anchor = full(timer)
    if anchor is None: return {'status':'invalid GetCycleTime timer','tick':None}
    possibilities=[]
    for name,convert,period in [('full cycle timer',full,128e9),('compact OHCI timestamp',compact,8e9)]:
        a,b=convert(first),convert(last)
        if a is None or b is None: continue
        span=(b-a)%period
        if not 8e6 <= span <= 14e6: continue
        reference=anchor%period
        offset=(a-reference+period/2)%period-period/2
        tick=point['cycle_host']+offset/(scale*1e6)
        age=(point['tick']-tick)*scale
        if -.5 <= age <= 1000:
            possibilities.append(dict(format=name,tick=tick,packet_span_ms=span/1e6,
                                      uncertainty_ms=.125+point['cycle_uncertainty']*scale/2,
                                      status='estimated from validated raw timestamp span; not a hardware TX timestamp'))
    return possibilities[0] if len(possibilities)==1 else {'status':'unsupported or ambiguous DCL timestamp format','tick':None}


def comparison(rows, output, reason=None):
    output.mkdir(parents=True,exist_ok=True)
    (output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    with (output/'results.csv').open('w',newline='') as f:
        keys=['id','state','raw_file','electrical_ms','wall_ms','errors']
        w=csv.DictWriter(f,fieldnames=keys);w.writeheader()
        for row in rows: w.writerow({key:row.get(key) for key in keys})
    fast=next((r for r in rows if r.get('state')=='fast' and not r['errors']),None)
    slow=next((r for r in rows if r.get('state')=='slow' and not r['errors']),None)
    if fast and slow:
        keys=('engine_cookie','generation','rolling','lead','guard','tx_packets','service_ns')
        if any(fast['session'].get(key)!=slow['session'].get(key) for key in keys):
            fast=slow=None
            reason=reason or 'engine identity or timing settings differ; not a same-engine pair'
    lines=['# 48 kHz same-engine fast/slow impulse comparison','',f'Stop/completion reason: {reason or "both states captured"}.', '',
           'Electrical AUHAL sample-host-time round trip and callback wall time are separate. Tag validation timestamps are not used as onset timestamps. Missing/stale/invalid IDs are never joined.', '']
    if fast and slow:
        lines += [f"Fast impulse {fast['id']}: {fast['electrical_ms']:.3f} ms electrical, {fast['wall_ms']:.3f} ms wall.",
                  f"Slow impulse {slow['id']}: {slow['electrical_ms']:.3f} ms electrical, {slow['wall_ms']:.3f} ms wall.", '',
                  '| Stage | Fast ms | Slow ms | Slow − fast ms |','|---|---:|---:|---:|']
        components=[]
        for name in fast['intervals_ms']:
            a,b=fast['intervals_ms'][name],slow['intervals_ms'][name]
            if a is not None and b is not None:
                lines.append(f'| {name} | {a:.3f} | {b:.3f} | {b-a:+.3f} |')
                components.append((b-a,name))
            else: lines.append(f'| {name} | missing | missing | unavailable |')
        if components:
            delta,stage=max(components)
            lines += ['',f'Largest observed added interval: {stage} ({delta:+.3f} ms). This locates the software interval, not automatically its physical cause.']
        extra_electrical=slow['electrical_ms']-fast['electrical_ms']
        extra_wall=slow['wall_ms']-fast['wall_ms']
        if abs(extra_electrical-extra_wall)>5:
            lines += ['', 'Electrical and wall increases differ by more than 5 ms: inspect AUHAL sample-host adjustments before claiming a physical transport delay.']
        elif components:
            largest,stage=max(components)
            if stage in ('tool_submit -> hal_submit','hal_submit -> pcm_read','pcm_read -> tx_prepare'):
                conclusion='the extra observed delay is before TX preparation'
            elif stage=='capture_decode -> hal_delivery':
                conclusion='the extra observed delay is in capture queue/HAL admission'
            elif stage=='hal_delivery -> tool_delivery':
                conclusion='the extra observed delay is in CoreAudio input delivery'
            else:
                conclusion='the extra observed delay is between TX preparation and capture decode; inspect intended TX and validated RX arrival estimates to split this further'
            lines += ['', 'Stage attribution: '+conclusion+'.']
        lines += ['', '| Timing interpretation | Fast ms | Slow ms | Slow − fast ms |','|---|---:|---:|---:|']
        for key in ('electrical_ms','wall_ms','wall_from_stage_points_ms','electrical_from_stage_points_ms','output_sample_host_adjustment_ms','input_sample_host_adjustment_ms',
                    'tx_prepare_to_intended_ms_estimate','intended_TX_to_RX_arrival_ms_estimate','RX_arrival_to_decode_ms_estimate'):
            a,b=fast.get(key),slow.get(key)
            lines.append(f'| {key} | {a:.3f} | {b:.3f} | {b-a:+.3f} |' if a is not None and b is not None else f'| {key} | missing | missing | unavailable |')
        lines += ['', '| Counter | Fast Δ | Slow Δ |','|---|---:|---:|']
        for key in sorted(set(fast['counter_deltas'])|set(slow['counter_deltas'])):
            lines.append(f"| {key} | {fast['counter_deltas'].get(key)} | {slow['counter_deltas'].get(key)} |")
    else:
        lines += ['INCONCLUSIVE: did not capture both valid states on one unchanged engine. All readings, including slow/no-signal/missing-stage probes, remain in results and raw files. No resets or mode/profile changes were requested.']
    lines += ['', 'Read stage queues and callback sizes in results.json. Queue depths are taken at the leading-edge block, with its sample offset and absolute source frame; they are not post-probe directional latency estimates.', '',
              'Interpretation: tool→HAL delay is CoreAudio output handoff; HAL→PCM and PCM→TX delay is before packet preparation; capture-decode→HAL delay is capture queue/HAL admission; HAL→tool delay is CoreAudio input delivery. TX preparation→capture decode includes scheduling, device/wire time and receive publication. Intended TX time is an estimate from scheduler progress, not observed DMA transmission. RX arrival correlation is accepted only with a validated packet timestamp span and a bounded cycle-time read bracket; otherwise it is missing. Do not call TX scheduling proven from that aggregate interval alone.', '',
              'The 16/20 ms wall clusters can reflect four/five 192-frame callbacks at 48 kHz; they do not explain an 82–90 ms electrical result by themselves. Compare sample-host adjustments and the summed observed stage intervals before attributing a change to callback phase. No transport fix has been applied.']
    (output/'comparison.md').write_text('\n'.join(lines)+'\n')
    print('\n'.join(lines),flush=True)
    return bool(fast and slow)


def run(args):
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    rows=[]; initial=None; cookie=None; configuration=None; stopped=None
    log_inode=LOG.stat().st_ino; offset=LOG.stat().st_size
    (out/'transport-before.log').write_bytes(LOG.read_bytes())
    commands=[]
    def command(argv,name,timeout=20):
        started=datetime.datetime.now(datetime.timezone.utc).isoformat()
        try:
            p=subprocess.run(list(map(str,argv)),cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
            rc,text=p.returncode,p.stdout
        except subprocess.TimeoutExpired as exc:
            rc=124;text=exc.stdout or ''
            if isinstance(text,bytes):text=text.decode(errors='replace')
        (out/(name+'.txt')).write_text(text)
        commands.append(dict(argv=list(map(str,argv)),name=name,started=started,finished=datetime.datetime.now(datetime.timezone.utc).isoformat(),returncode=rc))
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        return rc,text
    def engine(name):
        rc,text=command([CTL,'engine','get'],name)
        m=re.search(r'sample rate:\s*(\d+) Hz.*?FireWire generation at control startup:\s*(\d+)',text,re.S)
        if rc or not m:raise RuntimeError('engine not READY/readable')
        return tuple(map(int,m.groups()))
    def logs():
        nonlocal offset
        stat=LOG.stat()
        if stat.st_ino!=log_inode or stat.st_size<offset:raise RuntimeError('transport log rotated/truncated')
        with LOG.open() as f:f.seek(offset);text=f.read();offset=f.tell()
        with (out/'transport.log').open('a') as f:f.write(text)
        bad=FAULT.search(text)
        if bad:raise RuntimeError('transport fault/restart: '+bad[0])
    def save(reason):
        return comparison(rows,out,reason)
    try:
        initial=engine('initial-engine')
        if initial[0]!=48000:raise RuntimeError('set 48 kHz and let it settle before this test; runner does not change rates')
        rc,processes=command(['ps','-axo','pid,comm'],'initial-processes')
        if re.search(r'Logic Pro(?: X)?\.app/Contents/MacOS/',processes):raise RuntimeError('Logic is running')
        metadata=dict(routing=args.routing,host_load=args.host_load,initial_engine=initial,
                      probes=args.probes,idle_gap_s=args.idle_gap,fast_max_ms=args.fast_max_ms,slow_min_ms=args.slow_min_ms,
                      method='new AUHAL client per impulse; same transport; tagged leading edge; no rate/profile/reset requests')
        for name,argv in [('git-head',['git','rev-parse','HEAD']),('git-status',['git','status','--porcelain']),('os',['sw_vers']),('kernel',['uname','-a']),('hardware',['sysctl','hw.model','hw.memsize','hw.ncpu']),('profile',[CTL,'performance-profile','get']),('launchd-config',['plutil','-p','/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist'])]:
            if name=='profile':argv=[CTL,'performance-profile','get']
            _,metadata[name]=command(argv,'condition-'+name)
        metadata['sha256']={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in [TOOL,CTL,Path(__file__),ROOT/'devices/fw1814/transport/fw1814analog48',ROOT/'devices/fw1814/hal/build/macfw-fw1814.driver/Contents/MacOS/macfw-fw1814',Path('/Library/Application Support/macfw/fw1814/bin/fw1814analog48'),Path('/Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver/Contents/MacOS/macfw-fw1814')] if path.is_file()}
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
        no_signal=0
        nonce=secrets.randbelow(2**32-args.probes-1)+1
        for i in range(args.probes):
            if engine(f'engine-before-{i+1:02d}')!=initial:raise RuntimeError('rate/generation changed')
            _,processes=command(['ps','-axo','pid,comm'],f'processes-{i+1:02d}')
            if re.search(r'Logic Pro(?: X)?\.app/Contents/MacOS/',processes):raise RuntimeError('Logic reopened')
            impulse_id=nonce+i
            name=f'probe-{i+1:02d}-id-{impulse_id}'
            rc,text=command(['sudo','-n',TOOL,'--trace-id',str(impulse_id)],name)
            row=parse(text,impulse_id);row['raw_file']=name+'.txt';row['returncode']=rc
            row['state']='invalid' if row['errors'] or rc else 'fast' if row['electrical_ms']<=args.fast_max_ms else 'slow' if row['electrical_ms']>=args.slow_min_ms else 'intermediate'
            rows.append(row)
            (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
            print(f"{i+1:02d}: id={impulse_id} electrical={row['electrical_ms']} ms wall={row['wall_ms']} ms state={row['state']} errors={row['errors']}",flush=True)
            logs()
            if engine(f'engine-after-{i+1:02d}')!=initial:raise RuntimeError('rate/generation changed')
            if row['session'].get('engine_cookie'):
                if cookie is None:cookie=row['session']['engine_cookie']
                if row['session']['engine_cookie']!=cookie or row['session']['engine_cookie_after']!=cookie:raise RuntimeError('engine instance changed, even if generation is unchanged')
            if row['session']:
                settings=tuple(row['session'].get(key) for key in ('rolling','lead','guard','tx_packets','service_ns'))
                if configuration is None:configuration=settings
                if settings!=configuration:raise RuntimeError('transport timing configuration changed during comparison')
            if rc not in (0,2):raise RuntimeError('probe failure/unavailable tracing; raw output saved')
            no_signal=no_signal+1 if rc==2 else 0
            if no_signal>=3:raise RuntimeError('three consecutive missing tagged returns')
            if rc==0 and row['errors']:raise RuntimeError('tag/stage correlation invalid; preserve evidence and inspect')
            if any(r['state']=='fast' for r in rows) and any(r['state']=='slow' for r in rows):break
            time.sleep(args.idle_gap)
        else:stopped='probe limit reached without both states'
    except (Exception,KeyboardInterrupt) as exc:
        stopped=str(exc) or 'interrupted'
    finally:
        try:logs()
        except Exception as exc:stopped=stopped or str(exc)
        good=save(stopped)
        (out/'completion.json').write_text(json.dumps(dict(reason=stopped,both_states=good,engine_cookie=cookie),indent=2)+'\n')
        print('RETURN_DIRECTORY='+str(out),flush=True)
    return 0 if good and not stopped else 2


def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='command',required=True)
    c=sub.add_parser('collect');c.add_argument('output',type=Path)
    c.add_argument('--probes',type=int,default=40);c.add_argument('--idle-gap',type=float,default=.25)
    c.add_argument('--fast-max-ms',type=float,default=20);c.add_argument('--slow-min-ms',type=float,default=60)
    c.add_argument('--routing',required=True);c.add_argument('--host-load',required=True)
    a=sub.add_parser('analyze');a.add_argument('directory',type=Path)
    args=p.parse_args()
    if args.command=='analyze':
        rows=json.loads((args.directory/'results.json').read_text())
        return 0 if comparison(rows,args.directory,'offline reanalysis') else 2
    if not 1<=args.probes<=200 or not 0<=args.idle_gap<=10 or not 0<args.fast_max_ms<args.slow_min_ms:
        p.error('invalid probe count, idle gap or state thresholds')
    return run(args)


if __name__=='__main__':raise SystemExit(main())
