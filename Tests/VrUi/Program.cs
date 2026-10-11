using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Threading;
using DLSS_5_MANAGER;
using DLSS_5_MANAGER.ViewModels;
using DLSS_5_MANAGER.Views;
using Microsoft.FSharp.Core;

class Checks {
    static void Check(bool value, string name) {if(!value) throw new Exception(name); Console.WriteLine("PASS "+name);}
    static JsonNode Read(string path) => JsonNode.Parse(File.ReadAllText(path))!;
    static async Task<int> Main() {
        try {
            AppBuilder.Configure<App>().UseSkia().WithInterFont().UseHeadless(new AvaloniaHeadlessPlatformOptions {UseHeadlessDrawing=false}).SetupWithoutStarting();
            var root=Path.GetFullPath("outputs/vr-feasibility/ui-tests");Directory.CreateDirectory(root);
            var dir=Path.Combine(root,Guid.NewGuid().ToString("N"));Directory.CreateDirectory(dir);
            var control=Path.Combine(dir,"control.json");var status=Path.Combine(dir,"status.json");
            var vm=new ScreenViewModel(FSharpOption<string>.Some(Path.Combine(dir,"desktop")),FSharpOption<string>.Some(dir));
            vm.TargetIndex=0;Check(vm.IsDesktop && vm.CanChangeTarget,"desktop default and stopped target editable");
            vm.FastMultiPassEnabled=true;vm.FastReferenceTarget=3;
            vm.TargetIndex=1;Check(vm.IsVr && !vm.Nr && vm.VrScale==.25,"VR independent defaults NR off scale25");
            Check(vm.FastModeHint.Contains("1-pass NR"),"VR Fast OFF hint correctly describes one-pass NR");
            var view=new ScreenView{DataContext=vm}; var window=new Window{Content=view,Width=1040,Height=1600};window.Show();Dispatcher.UIThread.RunJobs();
            Check(!view.FindControl<Border>("ProcessingCard")!.IsVisible && !view.FindControl<Border>("CaptureCard")!.IsVisible,"VR desktop processing and capture hidden");
            Check(!view.FindControl<Border>("SourceCard")!.IsVisible && view.FindControl<Border>("FastMultiPassCard")!.IsVisible,"VR source hidden and existing Fast visible");
            vm.Start();Check(vm.IsRunning && vm.CanStop && !vm.CanStart && !vm.Nr && !vm.IsReady && vm.CanChangeTarget,"VR Start permits mode switching with truthful readiness");
            var before=Read(control)["lease_until_unix_ms"]!.GetValue<long>();
            Check(Read(control)["active"]!.GetValue<bool>() && !Read(control)["nr"]!.GetValue<bool>(),"Start control active true NR false");
            Check(view.FindControl<Button>("DesktopModeButton")!.IsEnabled && view.FindControl<Button>("VrModeButton")!.IsEnabled,"both explicit mode buttons enabled while VR running");
            vm.Nr=true;await vm.SwitchTargetAsync(0);Dispatcher.UIThread.RunJobs();
            Check(vm.IsDesktop && !vm.IsRunning && vm.CanStart && !Read(control)["active"]!.GetValue<bool>() && !Read(control)["nr"]!.GetValue<bool>(),"running VR to Desktop switch stops NR and expires VR lease");
            Check(view.FindControl<Border>("ProcessingCard")!.IsVisible,"desktop controls restored after live switch");
            await vm.SwitchTargetAsync(1);vm.Start();Check(vm.IsVr && vm.IsRunning,"Desktop to VR return starts cleanly");
            Check(Read(control)["auto_budget"]==null && Read(control)["pair_budget_ms"]==null,"obsolete deadline controls omitted from new UI configuration");
            vm.PassCount=3;Check(vm.PassCount==3 && Read(control)["passes"]!.GetValue<int>()==3,"VR normal multi-pass control routed to driver");
            vm.FastMultiPassEnabled=true;Check(vm.PassCount==3 && vm.FastModeHint.Contains("3×"),"Fast retains normal pass preference and restores it on OFF");
            vm.FastMultiPassEnabled=false;Dispatcher.UIThread.RunJobs();Check(view.FindControl<StackPanel>("VrMultiPassControls")!.IsEnabled,"normal VR multi-pass enabled with Fast OFF");
            vm.Nr=true;vm.FastMultiPassEnabled=true;vm.FastReferenceTarget=4.7;vm.FastStabilityAmount=.4;vm.VrScale=.3;vm.Intensity=.75;vm.ModelIndex=2;
            var cfg=Read(control);Check(cfg["nr"]!.GetValue<bool>() && cfg["fast"]!.GetValue<bool>() && cfg["target"]!.GetValue<int>()==5 && cfg["stability"]!.GetValue<double>()==.4,"hot NR Fast target and stability atomic control update");
            Check(cfg["work_scale"]!.GetValue<double>()==.3 && cfg["intensity"]!.GetValue<double>()==.75 && cfg["style"]!.GetValue<int>()==2,"VR scale and effect routed live");
            Thread.Sleep(850);Check(Read(control)["lease_until_unix_ms"]!.GetValue<long>()>before,"400ms background heartbeat renews lease");
            void Status(string state,bool nr,bool fast,long age=0) => File.WriteAllText(status,new JsonObject { ["version"]=1,["updated_unix_ms"]=DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()-age,["driver_pid"]=Environment.ProcessId,["state"]=state,["message"]="test state",["nr_applied"]=nr,["fast_applied"]=fast,["processed"]=10,["skipped"]=2,["left_us"]=7100,["right_us"]=6900,["work_width"]=624,["work_height"]=672 }.ToJsonString());
            Status("ready",false,false);Check(vm.IsReady && vm.Nr && vm.VrActualState.Contains("DLSS 5 off"),"ready requested NR on actual NR off distinguished");
            Status("starting",false,false);Check(!vm.IsReady && vm.IsPreparing,"asynchronous preparation visible");
            Status("applied",true,true);Check(vm.IsReady && vm.VrActualState.Contains("DLSS 5 applied") && vm.VrActualState.Contains("Fast applied"),"fresh driver confirms applied NR Fast");
            Check(vm.VrTelemetry.Contains("7.10 ms") && vm.VrTelemetry.Contains("624"),"actual eye timing and work resolution visible");
            vm.FastReferenceTarget=4;Dispatcher.UIThread.RunJobs();
            using(var bitmap=new Avalonia.Media.Imaging.RenderTargetBitmap(new PixelSize(1040,1600),new Vector(96,96))){bitmap.Render(view);bitmap.Save(Path.Combine(root,"vr-live-flow-ui.png"));}
            Status("budget_exceeded",false,false);Check(!vm.IsReady && vm.Status=="budget_exceeded" && vm.VrActualState.Contains("DLSS 5 off"),"budget fallback remains truthful");
            Status("error",false,false);Check(!vm.IsReady && vm.Status=="error","driver errors disable readiness");
            Status("applied",true,true,3100);Check(!vm.IsReady && vm.VrActualState.Contains("unavailable"),"stale applied state cannot label current applied");
            File.WriteAllText(status,"{}");Check(!vm.IsReady && vm.VrActualState.Contains("unavailable"),"malformed status safely unavailable");
            vm.FastReferenceTarget=99;vm.FastStabilityAmount=2;vm.VrScale=2;
            Check(vm.FastReferenceTarget==6 && vm.FastStabilityAmount==.9 && vm.VrScale==.65,"VR limits target6 stability90 scale65");
            await vm.StopAsync();Check(!vm.IsRunning && !vm.CanStop && vm.CanStart && !vm.Nr && vm.CanChangeTarget,"Stop releases running and target lock and restores Start");
            var off=File.ReadAllText(control);var offCfg=Read(control);Check(!offCfg["active"]!.GetValue<bool>() && !offCfg["nr"]!.GetValue<bool>() && offCfg["lease_until_unix_ms"]!.GetValue<long>()<=DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),"Stop writes immediate inactive NR off expired lease");
            Thread.Sleep(850);Check(File.ReadAllText(control)==off && Directory.GetFiles(dir,"*.tmp").Length==0,"Stop heartbeat retired and atomic temp cleaned");
            vm.TargetIndex=0;Console.WriteLine($"desktop preserved: vr={vm.IsVr} enabled={vm.FastMultiPassEnabled} target={vm.FastReferenceTarget}");Check(vm.FastMultiPassEnabled && vm.FastReferenceTarget==3,"desktop Fast settings preserved independently");
            window.Close();Console.WriteLine("ALL VR UI BRIDGE CHECKS PASSED");return 0;
        }catch(Exception e){Console.Error.WriteLine(e);return 1;}
    }
}
