# Compatibility entry point.
from pathlib import Path
import runpy
globals().update(runpy.run_path(str(Path(__file__).resolve().parent / "tools" / "generate_agent_fonts.py"), run_name=__name__))
