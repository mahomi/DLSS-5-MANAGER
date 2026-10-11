"""Moving, fractional-pixel packaged-image fixture; no game/desktop capture.
APP OUTPUT_DIR [--worker PATH] [--baseline-worker PATH]. Restores user Fast JSON.
Reports motion-compensated frame differences in the bottom half of the picture.
This is a temporal diagnostic, not a perceptual quality metric for a game.
"""
from pathlib import Path
import os,sys,json,time
import numpy as np,cv2
app=Path(sys.argv[1]).resolve(); dest=Path(sys.argv[2]).resolve();dest.mkdir(parents=True,exist_ok=True)
native=app/'mod files/native';sys.path.insert(0,str(native/'screen'))
os.environ.update(NS_NR_DLL=str(native.parent/'dlss 5/nvngx_dlssnr.dll'),NS_NR_SMALL='0',
    NS_FLOW_MFG='0',NS_MOTION_BACKEND='cpu',NS_HALO='0',NS_SHARPNESS='0',
    NS_EDGE_SMOOTH='0',NS_COLORS='00000000',NS_UPSCALER='0',DLSS_MANAGER_PASSES='1')
import processor,transport
processor.NATIVE_HOME=native;processor.WORKER_FILE=native/'nvngx.dll'
if '--worker' in sys.argv:processor.WORKER_FILE=Path(sys.argv[sys.argv.index('--worker')+1]).resolve()
settings=Path(os.environ['LOCALAPPDATA'])/'DLSS5Manager/Screen/fast-multipass.json'
saved=settings.read_bytes() if settings.exists() else None
w,h=512,288;yy,xx=np.mgrid[:h,:w].astype(np.float32)
source=cv2.cvtColor(cv2.resize(cv2.imread(str(app/'backgrounds/midnight-titanium.jpg')),(w,h)),cv2.COLOR_BGR2RGBA)
reports=[]
current_worker=processor.WORKER_FILE
try:
    for speed in [.75,8.25]:
        inputs=[cv2.remap(source,xx,yy-f*speed,cv2.INTER_LINEAR,borderMode=cv2.BORDER_WRAP) for f in range(40)]
        cases=[('one',dict(enabled=False,target=1)),
               ('auto90',dict(enabled=True,target=3)),
               ('legacy-toggle-ignored',dict(enabled=True,strength=1,reference=False,texture=True,tone=.6,target=3,temporal=False,stability=.9,full_stability=False,final_output=True)),
               ('stability0',dict(enabled=True,target=3,stability=0)),
               ('stability50',dict(enabled=True,target=3,stability=.5))]
        if '--baseline-worker' in sys.argv:
            cases.append(('previous90',dict(enabled=True,strength=3,reference=True,target=3,temporal=True,stability=.9,full_stability=True)))
        automatic=None
        for name,cfg in cases:
            processor.WORKER_FILE=(Path(sys.argv[sys.argv.index('--baseline-worker')+1]).resolve()
                if name=='previous90' else current_worker)
            temp=settings.with_suffix('.scroll.tmp');temp.write_text(json.dumps(cfg));os.replace(temp,settings);time.sleep(.15)
            worker=None
            try:
                params=dict(style=2,auto_mask=1,intensity=1.,local_tone=1.5,local_structure=1.5,skin_structure=1.5)
                worker,logs,reader,stop=processor.launch_worker(params,w,h,1,w,h)
                frames=[]
                for f,rgba in enumerate(inputs):
                    motion=np.zeros((h,w,2),np.float16)
                    # Known camera pan, small realistic per-frame flow variation.
                    motion[:,:,1]=-speed*(1+.15*(-1 if f%2 else 1))
                    transport.post_frame(worker,f,rgba,motion,f==0,f*16667,want_pixels=True)
                    out=reader.next_result(f,45);assert out is not None
                    assert np.array_equal(out[:,:,3],rgba[:,:,3]);frames.append(out)
                if name=='auto90': automatic=np.stack(frames)
                if name in ('legacy-toggle-ignored','previous90'):
                    delta=np.abs(np.stack(frames).astype(np.int16)-automatic.astype(np.int16))
                    assert delta.max()==0, f'{name}: output differed from automatic90 by {delta.max()} bytes'
                    print(f'PASS {name}: all 40 frames identical to automatic90',flush=True)
                effects=[v[:,:,:3].astype(np.float32)-i[:,:,:3] for v,i in zip(frames,inputs)]
                rgb=[];chroma=[]
                for f in range(10,len(frames)):
                    previous=cv2.remap(effects[f-1],xx,yy-speed,cv2.INTER_LINEAR,borderMode=cv2.BORDER_WRAP)
                    difference=(effects[f]-previous)[h//2:h-20,20:w-20]
                    rgb.append(float(np.abs(difference).mean()))
                    difference-=np.mean(difference,axis=-1,keepdims=True)
                    chroma.append(float(np.abs(difference).mean()))
                report=dict(speed=speed,mode=name,rgb_change=float(np.mean(rgb)),chroma_change=float(np.mean(chroma)))
                reports.append(report);print(report,flush=True)
            finally:
                if worker:
                    processor.stop_worker(worker,stop)
                    (dest/f'{speed}-{name}-worker.log').write_text('\n'.join(logs),encoding='utf-8')
                    assert worker.returncode==0
    (dest/'scroll-report.json').write_text(json.dumps(reports,indent=2))
    print('PASS automatic90 matches previous90 and ignores obsolete toggle/scope/placement at both speeds; 0/50% also exercised',flush=True)
finally:
    if saved is None:settings.unlink(missing_ok=True)
    else:settings.write_bytes(saved)
    print('Original Fast settings restored',flush=True)
