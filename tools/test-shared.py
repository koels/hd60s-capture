"""Exercise IPC with two consumers, a slow reader, and a crashed reader. No USB."""
import os, subprocess, time, json, signal
from pathlib import Path
root=Path(__file__).resolve().parents[1]
env=dict(os.environ,HD60S_TEST_NAMESPACE=f'test-{os.getpid()}')
duration=float(os.environ.get('HD60S_TEST_SECONDS','8'))
service=subprocess.Popen([str(root/'build/hd60s-service'),'--synthetic'],env=env)
try:
    time.sleep(.15)
    fast=subprocess.Popen([str(root/'build/test-shared-probe'),'--synthetic',str(duration)],env=env,stdout=subprocess.PIPE,text=True)
    slow=subprocess.Popen([str(root/'build/test-shared-probe'),'--synthetic','--slow',str(duration)],env=env,stdout=subprocess.PIPE,text=True)
    crash=subprocess.Popen([str(root/'build/test-shared-probe'),'--synthetic','30'],env=env,stdout=subprocess.PIPE,text=True)
    time.sleep(.8)
    crash.kill(); crash.wait()
    a=json.loads(fast.communicate(timeout=duration+12)[0]);b=json.loads(slow.communicate(timeout=duration+12)[0])
    assert fast.returncode==slow.returncode==0,(a,b)
    assert a['frames']>=duration*20 and a['audio_blocks']>=duration*70,a
    assert duration*5<=b['frames']<=duration*13 and b['audio_blocks']>=duration*70,b
    assert a['frames']>b['frames']*2,(a,b)
    assert abs(a['samples']-b['samples'])<12000,(a,b)
    print('Shared IPC passed: independent fast/slow consumers, stereo PCM, monotonic timestamps, continued capture after client crash.')
    print(json.dumps({'fast':a,'slow':b}))
finally:
    service.terminate();service.wait(timeout=12)
