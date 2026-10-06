# Compatibility entry point.
from pathlib import Path
import runpy
globals().update(runpy.run_path(str(Path(__file__).resolve().parent / "tools" / "download_github_runs.py"), run_name=__name__))
