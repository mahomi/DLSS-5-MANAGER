"""Compare measured-only cleanup with dc23ee3 measured-ON worker.
APP OUTPUT [--worker PATH] [--baseline-worker PATH]. Packaged fixture only.
Exercises all models, integer targets and live history amounts before/after resize/Halo.
"""
from pathlib import Path
import os,sys,json,time
import numpy as np,cv2
app=Path(sys.argv[1]).resolve();dest=Path(sys.argv[2]).resolve();dest.mkdir(parents=True,exist_ok=True)
native=app/'mod files/native';sys.path.insert(0,str(native/'screen'))
os.environ.update(NS_NR_DLL=str(native.parent/'dlss 5/nvngx_dlssnr.dll'),NS_FLOW_MFG='0',
    NS_MOTION_BACKEND='cpu',NS_SHARPNESS='0',NS_EDGE_SMOOTH='0',NS_COLORS='00000000',
    NS_UPSCALER='0',DLSS_MANAGER_PASSES='1')
import processor,transport
processor.NATIVE_HOME=native
current=Path(sys.argv[sys.argv.index('--worker')+1]).resolve() if '--worker' in sys.argv else native/'nvngx.dll'
baseline=Path(sys.argv[sys.argv.index('--baseline-worker')+1]).resolve() if '--baseline-worker' in sys.argv else None
settings=Path(os.environ['LOCALAPPDATA'])/'DLSS5Manager/Screen/fast-multipass.json'
saved=settings.read_bytes() if settings.exists() else None
reports=[]
def setting(enabled,target=2,stability=.9,old=False,**extras):
    cfg=dict(enabled=enabled,target=target,stability=stability)
    if old:cfg.update(strength=1,reference=True,texture=False,temporal=True,full_stability=True)
    cfg.update(extras);temp=settings.with_suffix('.measured-check.tmp')
    temp.write_text(json.dumps(cfg));os.replace(temp,settings);time.sleep(.15)
try:
    for small in [True,False]:
        os.environ.update(NS_NR_SMALL='1' if small else '0',NS_HALO='.05' if small else '0')
        fw,fh=(640,360) if small else (512,288);ww,wh=512,288
        rgba=cv2.cvtColor(cv2.resize(cv2.imread(str(app/'backgrounds/emerald-horizon.jpg')),(fw,fh)),cv2.COLOR_BGR2RGBA)
        for style in range(3):
            expected=None
            for tag,path in ([('previous',baseline),('current',current)] if baseline else [('current',current)]):
                processor.WORKER_FILE=path;setting(False,old=tag=='previous');worker=None
                try:
                    params=dict(style=style,auto_mask=1,intensity=1.,local_tone=1.5,local_structure=1.5,skin_structure=1.5)
                    worker,logs,reader,stop=processor.launch_worker(params,ww,wh,1,fw,fh)
                    index=0;results={}
                    def frame(image=rgba,reset=True):
                        global index
                        motion=np.zeros((wh,ww,2),np.float16)
                        transport.post_frame(worker,index,image,motion,reset,index*16667,want_pixels=True)
                        out=reader.next_result(index,45);index+=1
                        assert out is not None and out.shape==image.shape
                        assert np.array_equal(out[:,:,3],image[:,:,3]),'Alpha changed'
                        return out
                    results['off']=frame()
                    for amount in [0,.9]:
                        for target in range(1,7):
                            setting(True,target,amount,old=tag=='previous')
                            results[f't{target}-a{amount}']=frame()
                    assert np.array_equal(results['t1-a0'],results['off']),'Target1 differed from ordinary NR'
                    assert not np.array_equal(results['t2-a0'],results['t6-a0']),'Targets did not change output'
                    for amount in [0,.3,.6,.9]:
                        setting(True,3,amount,old=tag=='previous')
                        results[f'history-{amount}']=np.stack([frame(cv2.convertScaleAbs(rgba,alpha=1,beta=f%3),f==0) for f in range(12)])
                    setting(True,6,.9,old=tag=='previous')
                    normal=frame();setting(True,6,.9,old=tag=='previous',strength=6,reference=False,texture=True,tone=.6,temporal=False,full_stability=False,final_output=True)
                    # Baseline checkbox false intentionally selects legacy; only new worker must ignore it.
                    if tag=='current':assert np.array_equal(frame(),normal),'Removed knobs still select a path'
                    setting(False,old=tag=='previous');assert np.array_equal(frame(),results['off']),'Hot OFF did not restore ordinary NR'
                    if tag=='previous':expected=results
                    elif expected:
                        for key,out in results.items():
                            delta=np.abs(out.astype(np.int16)-expected[key].astype(np.int16))
                            assert delta.max()==0,f'{small=} {style=} {key}: changed by {delta.max()} bytes'
                        report=dict(small_nr=small,style=style,compared_frames=61,max_byte_difference=0)
                        reports.append(report);print('PASS exact baseline pixels:',report,flush=True)
                finally:
                    if worker:
                        processor.stop_worker(worker,stop);(dest/f'{small}-{style}-{tag}-worker.log').write_text('\n'.join(logs),encoding='utf-8')
                        assert worker.returncode==0,'Worker failed'
    (dest/'comparison-report.json').write_text(json.dumps(reports,indent=2))
    print('PASS all models, targets1..6, history0/30/60/90, raw-NR stage, small NR+Halo, full NR and live OFF',flush=True)
finally:
    if saved is None:settings.unlink(missing_ok=True)
    else:settings.write_bytes(saved)
    print('Original Fast settings restored',flush=True)
