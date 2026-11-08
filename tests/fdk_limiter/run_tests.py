#!/usr/bin/env python3
from pathlib import Path
import subprocess, tempfile, array
p=Path(__file__).resolve().parent
build=p.parents[1]/'build-fdk-limiter-host'
subprocess.run(['cmake','-S',str(p),'-B',str(build),'-G','Ninja'],check=True)
subprocess.run(['cmake','--build',str(build),'-j','8'],check=True)
subprocess.run([str(build/'limiter_test')],check=True,timeout=30)
subprocess.run([str(build/'transport_test')],check=True,timeout=15)
with tempfile.TemporaryDirectory(prefix='aac-limiter-') as tmp:
 tmp=Path(tmp)
 for sr in [48000,44100]:
  aac=tmp/f'quiet-{sr}.aac'
  subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i',f'sine=frequency=997:sample_rate={sr}:duration=0.6','-ac','2','-c:a','aac','-b:a','256k','-f','adts',str(aac)],check=True)
  subprocess.run([str(build/'decode_probe'),str(aac)],check=True,timeout=30)
  waves={}
  for mode in ['on','off']:
   out=tmp/f'{sr}-{mode}.f32'
   subprocess.run([str(build/f'wrapper_{mode}'),str(aac),str(sr),str(out)],check=True,timeout=30)
   v=array.array('f');v.frombytes(out.read_bytes());waves[mode]=v
  delay=sr*15//1000
  a,b=waves['on'],waves['off']
  assert len(a)==len(b)+2*delay
  aligned=max(abs(x-y) for x,y in zip(a[2*delay:],b))
  unaligned=max(abs(x-y) for x,y in zip(a,b))
  print(f'{sr} Hz: lookahead {delay} frames, aligned max error {aligned:.9g}, uncompensated {unaligned:.6g}',flush=True)
  assert aligned<0.000001
  assert unaligned>0.01
  timelines={}
  for mode in ['on','off']:
   v=array.array('f');v.frombytes((tmp/f'{sr}-{mode}.f32.timeline').read_bytes());timelines[mode]=v
  assert len(timelines['on'])==len(timelines['off'])
  assert max(abs(x-y) for x,y in zip(timelines['on'],timelines['off']))<0.000001
  print(f'PASS {sr} Hz actual PCM FIFO: deadline tail drain and contiguous resume preserve the complete timeline',flush=True)
 print('PASS real AAC on/off comparison: lookahead compensation preserves the sample timeline at both rates')
