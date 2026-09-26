"""Read-only live process/coordinate metrics. Requires psutil on the test machine."""
import argparse,json,time,statistics,sqlite3,urllib.request
from pathlib import Path
import psutil
p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--seconds',type=int,default=180);p.add_argument('--out',type=Path,required=True);p.add_argument('--db',type=Path,required=True);p.add_argument('--label',default='baseline');a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
logical=psutil.cpu_count();proc=psutil.Process(a.pid);prev={};last=time.monotonic();start=last;samples=[];failures=0
psutil.cpu_percent(None)
with (a.out/'samples.jsonl').open('w',encoding='utf-8') as out:
    while time.monotonic()-start<a.seconds:
        time.sleep(1);now=time.monotonic();delta=now-last;last=now
        try: family=[proc]+proc.children(recursive=True)
        except psutil.NoSuchProcess: raise SystemExit('Target process exited during sampling')
        cpu=0;rss=0;priv=0;read=0;write=0;new={}
        for child in family:
            try:
                t=child.cpu_times();mi=child.memory_info();io=child.io_counters();value=t.user+t.system
                if child.pid in prev:cpu+=max(0,value-prev[child.pid])/delta*100
                new[child.pid]=value;rss+=mi.rss;priv+=getattr(mi,'private',0);read+=io.read_bytes;write+=io.write_bytes
            except (psutil.NoSuchProcess,psutil.AccessDenied):pass
        prev=new
        row={'elapsed':now-start,'process_cpu_core_pct':cpu,'process_cpu_machine_pct':cpu/logical,'system_cpu_pct':psutil.cpu_percent(None),'rss_mib':rss/1048576,'private_mib':priv/1048576,'io_read_bytes':read,'io_write_bytes':write,'process_count':len(new)}
        try:
            with urllib.request.urlopen('http://127.0.0.1:8081/status',timeout=2) as response:st=json.load(response)
            row['recording']=st.get('recording',{});row['tier2']=st.get('tier2',{});row['calibration']=st.get('calibration',{})
        except Exception as exc: failures+=1;row['status_error']=type(exc).__name__
        samples.append(row);out.write(json.dumps(row,ensure_ascii=False)+'\n');out.flush()
        if len(samples)%30==0:print(json.dumps({'label':a.label,'elapsed':round(now-start),'cpu_machine_pct':round(cpu/logical,2),'rss_mib':round(rss/1048576,1),'recording':row.get('recording')},ensure_ascii=False),flush=True)

def stats(values):
    values=sorted(values)
    return {'n':len(values),'mean':statistics.fmean(values),'p50':statistics.median(values),'p95':values[min(len(values)-1,int(len(values)*.95))],'min':values[0],'max':values[-1]} if values else None
summary={'label':a.label,'seconds_observed':samples[-1]['elapsed'],'logical_processors':logical,'pid':a.pid,'status_failures':failures,'warmup_samples_excluded':10}
for field in ('process_cpu_core_pct','process_cpu_machine_pct','system_cpu_pct','rss_mib','private_mib'):
    summary[field]=stats([x[field] for x in samples[10:]])
summary['recording_first']=samples[0].get('recording');summary['recording_last']=samples[-1].get('recording');summary['recording_write_ms']=stats([x['recording']['last_write_ms'] for x in samples[10:] if 'recording' in x]);summary['tier2_first']=samples[0].get('tier2');summary['tier2_last']=samples[-1].get('tier2')
with sqlite3.connect(a.db.resolve().as_uri()+'?mode=ro',uri=True) as db:
    db.execute('BEGIN')
    rows=db.execute('SELECT mono,payload FROM replay_frames ORDER BY mono').fetchall()
    frames=[json.loads(row[1]) for row in rows];diff=[rows[i][0]-rows[i-1][0] for i in range(1,len(rows))]
    summary['coordinate_records']=len(rows);summary['coordinate_payload_bytes']=sum(len(x[1].encode()) for x in rows);summary['coordinate_intervals_seconds']=stats(diff)
    summary['interval_bands']={'at_most_0_3s':sum(x<=.3 for x in diff),'0_3_to_1s':sum(.3<x<=1 for x in diff),'1_to_11s':sum(1<x<=11 for x in diff),'over_11s':sum(x>11 for x in diff)}
    summary['coordinate_span_seconds']=rows[-1][0]-rows[0][0] if rows else 0
    summary['frames_with_people']=sum(bool(x.get('people')) for x in frames);summary['frames_with_objects']=sum(bool(x.get('objects')) for x in frames)
    summary['events_by_module_level']=db.execute('SELECT module,level,count(*) FROM events GROUP BY module,level').fetchall()
    summary['replay_event_count']=db.execute('SELECT count(*) FROM replay_events').fetchone()[0]
    summary['fall_gate_counts']={str(i):sum(p.get('fall_gate')==i for f in frames for p in f.get('people',[])) for i in range(7)}
    for field in ('camera_fps','inference_fps','tier1_mean_ms','tier1_p95_ms','tier2_mean_ms','tier2_p95_ms','recorder_previous_ms','cpu_core_pct'):
        summary[field]=stats([f['perf'][field] for f in frames if field in f.get('perf',{})])
    summary['last_frame_perf']=frames[-1].get('perf') if frames else None
(a.out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'summary':str(a.out/'summary.json'),'cpu':summary['process_cpu_machine_pct'],'memory':summary['rss_mib'],'coordinate_records':len(rows)},ensure_ascii=False),flush=True)
