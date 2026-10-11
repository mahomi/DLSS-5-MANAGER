"""Measure actual sequential NR 1..6 on packaged images, without desktop capture.

Run with the prepared app's bundled Python: script APP OUTPUT_DIR.
Close Manager first. User Fast settings are restored in finally. No game files
or global environment are changed. Every reference frame uses NR reset.
"""
from pathlib import Path
import os, sys, json, time, hashlib
import numpy as np
import cv2

app=Path(sys.argv[1]).resolve(); dest=Path(sys.argv[2]).resolve()
dest.mkdir(parents=True,exist_ok=True)
native=app/'mod files/native'
sys.path.insert(0,str(native/'screen'))
os.environ.update(NS_NR_DLL=str(native.parent/'dlss 5/nvngx_dlssnr.dll'),
    NS_NR_SMALL='0',NS_FLOW_MFG='0',NS_MOTION_BACKEND='cpu',NS_HALO='0',
    NS_SHARPNESS='0',NS_EDGE_SMOOTH='0',NS_COLORS='00000000',NS_UPSCALER='0')
import processor, transport
processor.NATIVE_HOME=native; processor.WORKER_FILE=native/'nvngx.dll'
settings=Path(os.environ['LOCALAPPDATA'])/'DLSS5Manager/Screen/fast-multipass.json'
saved=settings.read_bytes() if settings.exists() else None
style=int(sys.argv[sys.argv.index('--style')+1]) if '--style' in sys.argv else 2
params=dict(style=style,auto_mask=1,intensity=1.0,local_tone=1.5,local_structure=1.5,skin_structure=1.5)
images=sorted((app/'backgrounds').glob('*.jpg'))
w,h=512,288
inputs=np.stack([cv2.cvtColor(cv2.resize(cv2.imread(str(p)),(w,h)),cv2.COLOR_BGR2RGBA) for p in images])
references=[]; metadata=dict(params=params,resolution=[w,h],reset=True,halo=0,enhance=False,
    images=[dict(name=p.name,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in images],
    worker_sha256=hashlib.sha256(processor.WORKER_FILE.read_bytes()).hexdigest(),
    nr_model_sha256=hashlib.sha256((native.parent/'dlss 5/nvngx_dlssnr.dll').read_bytes()).hexdigest(),
    numpy_version=np.__version__,opencv_version=cv2.__version__)
try:
    settings.write_text('{"enabled":false,"strength":2}',encoding='utf-8')
    for passes in range(1,7):
        os.environ['DLSS_MANAGER_PASSES']=str(passes)
        worker=None
        try:
            worker,logs,reader,stop=processor.launch_worker(params,w,h,1,w,h)
            outputs=[]
            for index,rgba in enumerate(inputs):
                transport.post_frame(worker,index,rgba,np.zeros((h,w,2),np.float16),True,index*16667,want_pixels=True)
                value=reader.next_result(index,45)
                assert value is not None and value.shape==rgba.shape
                outputs.append(value)
                print(f'MEASURE {passes} passes {images[index].name}',flush=True)
            if passes>1:
                assert any(f'[multipass] {passes} independent layers ready' in line for line in logs), 'Reference silently fell back to fewer layers'
            references.append(np.stack(outputs))
        finally:
            if worker:
                processor.stop_worker(worker,stop)
                (dest/f'reference-{passes}-worker.log').write_text('\n'.join(logs),encoding='utf-8')
                assert worker.returncode==0, f'Worker exited {worker.returncode}'
    np.savez_compressed(dest/'references.npz',inputs=inputs,references=np.stack(references))
    (dest/'measurement.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    print('PASS measured all 54 real NR reference frames',flush=True)
finally:
    if saved is None: settings.unlink(missing_ok=True)
    else: settings.write_bytes(saved)
    print('Original Fast settings restored',flush=True)
