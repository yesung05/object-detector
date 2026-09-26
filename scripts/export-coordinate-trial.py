"""Export only coordinate-trial tables; never opens camera or image files.
Usage: python scripts/export-coordinate-trial.py --logs logs --out trial-export
"""
import argparse
import json
import sqlite3
from pathlib import Path

def export(logs, output):
    output.mkdir(parents=True, exist_ok=False)
    manifest={"schema":1,"format":"jsonl","contains_images":False,"databases":[]}
    for path in sorted(logs.glob("*.db")):
        if path.name.endswith("_perf.db"): continue
        db=sqlite3.connect(path.resolve().as_uri()+"?mode=ro",uri=True)
        db.row_factory=sqlite3.Row
        try:
            tables={row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "replay_frames" not in tables: continue
            db.execute("BEGIN")
            entry={"source":path.name,"tables":{}}
            queries={name:"SELECT * FROM "+name for name in ("replay_sessions","replay_frames","replay_events","replay_geometry")}
            queries["events"]="SELECT e.* FROM events e JOIN replay_events r ON e.id=r.event_id"
            for name,query in queries.items():
                target=output/(path.stem+"_"+name+".jsonl");count=0
                with target.open("x",encoding="utf-8") as f:
                    for row in db.execute(query):
                        item=dict(row)
                        for key in ("payload","metadata"):
                            if key in item: item[key]=json.loads(item[key])
                        f.write(json.dumps(item,ensure_ascii=False,separators=(",",":"))+"\n");count+=1
                entry["tables"][name]={"file":target.name,"rows":count}
            db.rollback();manifest["databases"].append(entry)
        finally: db.close()
    (output/"manifest.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding="utf-8")
    return manifest

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs",type=Path,default=Path("logs"))
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    result=export(args.logs,args.out)
    print(f"Exported {len(result['databases'])} coordinate databases to {args.out}")
