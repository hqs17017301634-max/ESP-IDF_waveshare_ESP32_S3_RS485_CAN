"""Run ESP-IDF ldgen from a JSON argument file without Windows shell limits."""
import json
from pathlib import Path
import runpy
import sys

payload = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
script = Path(payload['script'])
sys.path.insert(0, str(script.parent))
sys.argv = [str(script), *payload['arguments']]
runpy.run_path(str(script), run_name='__main__')
