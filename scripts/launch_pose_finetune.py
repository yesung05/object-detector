"""Launch the original pose experiment without a visible console; persist logs/PID."""
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if __name__ == '__main__':
    logs = ROOT / 'runs/pose/pose-original-launch'
    logs.mkdir(parents=True, exist_ok=True)
    manifest = logs / 'process.json'
    if manifest.exists():
        import psutil
        old = json.loads(manifest.read_text(encoding='utf-8'))
        if psutil.pid_exists(old['pid']):
            raise SystemExit(f"Recorded process {old['pid']} still exists; inspect before starting another run")
    command = [sys.executable, '-u', str(ROOT / 'scripts/finetune_pose_original.py'), *sys.argv[1:]]
    with (logs / 'stdout.log').open('ab') as stdout, (logs / 'stderr.log').open('ab') as stderr:
        child = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr,
                                 stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    result = {'pid': child.pid, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'command': command, 'logs': str(logs)}
    manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
