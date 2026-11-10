"""Opt-in subprocess decoder used before accepting remote PNG clipboard data."""
import subprocess
import sys
from .clipboard import MAX_PNG

class BoundedPNGDecoder:
    def __init__(self):
        try:r=subprocess.run([sys.executable,'-B','-m','hop_linux.png_worker','--probe'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5)
        except (OSError,subprocess.TimeoutExpired) as exc:raise RuntimeError('PNG decoder unavailable') from exc
        if r.returncode:raise RuntimeError('PNG decoder unavailable')
    def __call__(self,data):
        if not isinstance(data,bytes) or len(data)>MAX_PNG:return False
        try:
            r=subprocess.run([sys.executable,'-B','-m','hop_linux.png_worker'],input=data,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=8)
            return r.returncode==0
        except (OSError,subprocess.TimeoutExpired):return False
