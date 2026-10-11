"""Fit signed RGB band coefficients to measured Multi-pass; hold out whole scenes.

Usage: bundled-python fit_fast_multipass.py MEASUREMENTS_DIR HEADER_PATH.
Measurements use full-resolution NR with no finishing effects. Coefficients
predict Y_N-Y_1; they do not create textures absent from first-pass NR.
"""
from pathlib import Path
import sys,json,hashlib
import numpy as np
import cv2

root=Path(sys.argv[1]); header=Path(sys.argv[2])
style=json.loads((root/'measurement.json').read_text(encoding='utf-8'))['params']['style']
d=np.load(root/'references.npz'); x=d['inputs'][:,:,:,:3].astype(np.float64)/255
ys=d['references'][:,:,:,:,:3].astype(np.float64)/255; nr=ys[0]
holdout=[2,5,8]; training=[i for i in range(len(x)) if i not in holdout]
assert len(x)==9
def filt(a,k): return np.stack([cv2.filter2D(v,-1,k,borderType=cv2.BORDER_REPLICATE) for v in a])
k3=np.outer([1,2,1],[1,2,1])/16
k9=np.zeros((9,9)); k9[::2,::2]=np.outer([1,4,6,4,1],[1,4,6,4,1])/256
r=nr-x; small=filt(r,k3); low=filt(r,k9)
fine=r-small; structure=small-low
nrsmall=filt(nr,k3); nrlow=filt(nr,k9)
features=np.stack([fine,structure,low,nr-nrsmall,nrsmall-nrlow,np.ones_like(nr)],axis=-1)
def smooth(a,b,v):
    q=np.clip((v-a)/(b-a),0,1); return q*q*(3-2*q)
finegate=smooth(1/255,3/255,np.max(np.abs(fine),axis=-1))[...,None]
lum=np.array([.2126,.7152,.0722]); lx=x@lum; ly=nr@lum
range3=np.stack([cv2.dilate(np.maximum(a,b),np.ones((3,3)))-cv2.erode(np.minimum(a,b),np.ones((3,3))) for a,b in zip(lx,ly)])
edgegate=(1-smooth(.10,.35,range3))[...,None]
def output(delta,n):
    delta=np.clip(delta,-.04*n,.04*n)
    return np.rint(np.clip(nr+np.clip(delta,-.75*nr,.75*(1-nr)),0,1)*255)/255
coeffs=[np.zeros((3,6))]; reports=[]
for n in range(2,7):
    target=ys[n-1]-nr
    c=[]
    for channel in range(3):
        a=features[training,:,:,channel,:].reshape(-1,6); b=target[training,:,:,channel].reshape(-1)
        ridge=np.diag([.05,.05,.05,.05,.05,1e-5])
        weights=np.linalg.solve(a.T@a+ridge,a.T@b)
        # Robust L1 fit on training frames only, matching the reported absolute
        # pixel error instead of giving a few large model artifacts priority.
        for iteration in range(5):
            robust=1/np.maximum(np.abs(a@weights-b),1/255)
            weights=np.linalg.solve(a.T@(a*robust[:,None])+ridge,a.T@(b*robust))
        weights[:5]=np.clip(weights[:5],-8,8); weights[5]=np.clip(weights[5],-.24,.24)
        c.append(weights)
    c=np.stack(c); coeffs.append(c)
    predicted=output(np.einsum('ihwck,ck->ihwc',features,c),n)
    legacy=output((n-1)*(fine*finegate+1.10*structure+.60*low)*edgegate,n)
    per=[]
    for i in range(len(x)):
        per.append(dict(image=i,split='validation' if i in holdout else 'training',
            calibrated_mae=float(np.abs(predicted[i]-ys[n-1,i]).mean()*255),
            legacy_mae=float(np.abs(legacy[i]-ys[n-1,i]).mean()*255),
            one_pass_mae=float(np.abs(nr[i]-ys[n-1,i]).mean()*255)))
    validation={key:float(np.mean([per[i][key] for i in holdout])) for key in ['calibrated_mae','legacy_mae','one_pass_mae']}
    reports.append(dict(passes=n,validation=validation,scenes=per,coefficients=c.tolist()))
    print(n,validation,flush=True)
    assert validation['calibrated_mae'] < validation['legacy_mae'], 'Held-out calibration did not improve over legacy'
report=dict(measurements_sha256=hashlib.sha256((root/'references.npz').read_bytes()).hexdigest(),
    basis=['residual fine','residual structure','residual low','NR fine','NR structure','bias'],
    training=training,validation=holdout,reports=reports)
(root/'fit-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
lines=['// Generated from actual NR references by fit_fast_multipass.py. Do not hand-tune.',
    '// Coefficients predict added RGB per target pass count. Entire scenes held out.',
    '#pragma once',f'static const float kFastReference{style}[6][3][6] = {{']
for c in coeffs:
    lines.append('    {')
    for row in c: lines.append('        {'+', '.join(f'{v:.9f}f' for v in row)+'},')
    lines.append('    },')
lines.append('};')
header.write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('PASS fitted coefficients validated on three held-out scenes',flush=True)
