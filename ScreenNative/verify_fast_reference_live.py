"""Verify production GPU matching against actual sequential NR reference pixels.

Usage: bundled-python script APP CALIBRATION_DIR. No desktop input is used.
The three validation scenes per model were excluded from coefficient fitting.
"""
from pathlib import Path
import os,sys,json,time
import numpy as np
import cv2
app=Path(sys.argv[1]).resolve(); root=Path(sys.argv[2]).resolve()
native=app/'mod files/native'; sys.path.insert(0,str(native/'screen'))
os.environ.update(NS_NR_DLL=str(native.parent/'dlss 5/nvngx_dlssnr.dll'),NS_NR_SMALL='0',
    NS_FLOW_MFG='0',NS_MOTION_BACKEND='cpu',NS_HALO='0',NS_SHARPNESS='0',
    NS_EDGE_SMOOTH='0',NS_COLORS='00000000',NS_UPSCALER='0',DLSS_MANAGER_PASSES='1')
import processor,transport
processor.NATIVE_HOME=native; processor.WORKER_FILE=native/'nvngx.dll'
if '--worker' in sys.argv: processor.WORKER_FILE=Path(sys.argv[sys.argv.index('--worker')+1]).resolve()
settings=Path(os.environ['LOCALAPPDATA'])/'DLSS5Manager/Screen/fast-multipass.json'
saved=settings.read_bytes() if settings.exists() else None
reports=[]
def setting(**kw):
    temp=settings.with_suffix('.reference.tmp')
    values=dict(enabled=True,target=1,stability=0); values.update(kw)
    temp.write_text(json.dumps(values),encoding='utf-8')
    os.replace(temp,settings); time.sleep(.15)
try:
    for style in range(3):
        data=root if style==2 else root/f'style{style}'
        fixture=np.load(data/'references.npz'); inputs=fixture['inputs']; refs=fixture['references']
        fit=json.loads((data/'fit-report.json').read_text()); validation=fit['validation']
        params=json.loads((data/'measurement.json').read_text())['params']
        setting(enabled=False)
        worker=None
        try:
            worker,logs,reader,stop=processor.launch_worker(params,512,288,1,512,288)
            index=0
            def frame(rgba,reset=True):
                global index
                transport.post_frame(worker,index,rgba,np.zeros((288,512,2),np.float16),reset,index*16667,want_pixels=True)
                result=reader.next_result(index,45); index+=1
                assert result is not None and result.shape==rgba.shape
                assert np.array_equal(result[:,:,3],rgba[:,:,3]), 'Alpha changed'
                return result
            for i in range(9): assert np.array_equal(frame(inputs[i]),refs[0,i]), 'NR baseline differs from reference capture'
            for passes in range(2,7):
                setting(target=passes)
                matched=np.stack([frame(rgba) for rgba in inputs])
                bound=np.abs(matched[:,:,:,:3].astype(int)-refs[0,:,:,:,:3].astype(int)).max()
                assert bound <= np.ceil(.04*passes*255), 'Measured mode exceeded channel bound'
                old=fit['reports'][passes-2]['validation']['legacy_mae']
                new=float(np.abs(matched[validation,:,:,:3].astype(float)-refs[passes-1,validation,:,:,:3]).mean())
                expected=fit['reports'][passes-2]['validation']['calibrated_mae']
                assert abs(new-expected)<.02, 'GPU result disagrees with independently calculated fit'
                assert new<old, 'Held-out production output did not improve over stored legacy fit'
                reports.append(dict(style=style,passes=passes,stored_legacy_mae=old,matched_mae=new,
                    reduction_percent=(old-new)/old*100))
                print(f'PASS model={style} target={passes} validation MAE {old:.4f} -> {new:.4f}',flush=True)
                if passes==6:
                    i=validation[-1]
                    panels=[inputs[i],refs[0,i],refs[5,i],matched[i]]
                    labels=['Input','One NR pass','Actual Multi-pass 6','Measured Fast target 6']
                    strips=[]
                    for panel,label in zip(panels,labels):
                        rgb=cv2.cvtColor(panel,cv2.COLOR_RGBA2BGR)
                        bar=np.zeros((28,512,3),np.uint8); cv2.putText(bar,label,(10,20),cv2.FONT_HERSHEY_SIMPLEX,.5,(255,255,255),1)
                        strips.append(np.vstack([bar,rgb]))
                    cv2.imwrite(str(root/f'model{style}-comparison.png'),np.hstack(strips))
            setting(target=1)
            assert np.array_equal(frame(inputs[0]),refs[0,0]), 'Target 1 did not bypass'
            setting(target=4,stability=.9)
            matched=frame(inputs[0]); [frame(inputs[0],False) for _ in range(12)]
            setting(target=4,stability=.9,reference=False,strength=1,texture=True,tone=0,temporal=False,full_stability=False)
            assert np.array_equal(frame(inputs[0]),matched), 'Retired options altered matching'
            setting(enabled=False)
            assert np.array_equal(frame(inputs[0]),refs[0,0]), 'Fast OFF did not restore one pass'
            print(f'PASS model={style}: target1, matching+history, retired options, hot OFF',flush=True)
        finally:
            if worker:
                processor.stop_worker(worker,stop)
                (root/f'verify-model{style}-worker.log').write_text('\n'.join(logs),encoding='utf-8')
                assert worker.returncode==0, 'Worker failed'
    (root/'gpu-validation.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
    print('PASS all three model profiles against actual 2..6-pass references',flush=True)
finally:
    if saved is None: settings.unlink(missing_ok=True)
    else: settings.write_bytes(saved)
    print('Original Fast settings restored',flush=True)
