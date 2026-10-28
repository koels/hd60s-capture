"""Convert a captured YUY2 frame to PNG using limited-range BT.601/709."""
import argparse
import numpy as np
from PIL import Image
p=argparse.ArgumentParser()
p.add_argument('input'); p.add_argument('output'); p.add_argument('--width',type=int,required=True); p.add_argument('--height',type=int,required=True); p.add_argument('--bt709',action='store_true')
a=p.parse_args()
b=np.fromfile(a.input,dtype=np.uint8).reshape(a.height,a.width//2,4).astype(np.float32)
y=np.stack((b[:,:,0],b[:,:,2]),axis=-1).reshape(a.height,a.width)
u=np.repeat(b[:,:,1],2,axis=1)-128
v=np.repeat(b[:,:,3],2,axis=1)-128
y=(y-16)*255/219
if a.bt709: r=y+1.792741*v; g=y-.213249*u-.532909*v; blue=y+2.112402*u
else: r=y+1.596027*v; g=y-.391762*u-.812968*v; blue=y+2.017232*u
Image.fromarray(np.clip(np.stack((r,g,blue),axis=-1),0,255).astype(np.uint8)).save(a.output)
