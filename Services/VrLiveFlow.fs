namespace DLSS_5_MANAGER.Services

open System
open System.IO
open System.Text.Json.Nodes
open System.Threading
open System.Diagnostics

/// File lease bridge. No compositor or registry interaction occurs in the manager.
type VrLiveFlow(?rootDirectory: string, ?clock: unit -> int64) =
    let root = defaultArg rootDirectory (Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "vr-feasibility", "driver-live-flow")))
    let now = defaultArg clock (fun () -> DateTimeOffset.UtcNow.ToUnixTimeMilliseconds())
    let gate = obj()
    let mutable active = false
    let mutable writeError = ""
    let settings = JsonObject()
    let changed = Event<unit>()
    let mutable timer: Timer option = None
    let atomicWrite (path: string) (json: JsonObject) =
        Directory.CreateDirectory(root) |> ignore
        let temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp"
        try
            File.WriteAllText(temp, json.ToJsonString())
            File.Move(temp, path, true)
        finally
            if File.Exists(temp) then File.Delete(temp)
    let write () =
        try
            settings["active"] <- JsonValue.Create(active)
            settings["lease_until_unix_ms"] <- JsonValue.Create(if active then now() + 3000L else now())
            atomicWrite (Path.Combine(root, "control.json")) settings
            writeError <- ""
        with ex -> writeError <- ex.Message
    do
        settings["version"] <- JsonValue.Create(1)
        settings["nr"] <- JsonValue.Create(false)
        settings["fast"] <- JsonValue.Create(false)
        settings["target"] <- JsonValue.Create(2)
        settings["stability"] <- JsonValue.Create(0.9)
        settings["work_scale"] <- JsonValue.Create(0.25)
        settings["passes"] <- JsonValue.Create(1)
        settings["intensity"] <- JsonValue.Create(1.0)
        settings["local_tone"] <- JsonValue.Create(0.5)
        settings["local_structure"] <- JsonValue.Create(1.0)
        settings["skin_structure"] <- JsonValue.Create(-1.0)
        settings["style"] <- JsonValue.Create(1)
    member _.RootDirectory = root
    member _.Changed = changed.Publish
    member _.Active = lock gate (fun () -> active)
    member _.Error = lock gate (fun () -> writeError)
    member _.GetBool(key: string) = lock gate (fun () -> settings[key].GetValue<bool>())
    member _.GetNumber(key: string) = lock gate (fun () -> Double.Parse(settings[key].ToJsonString(), Globalization.CultureInfo.InvariantCulture))
    member _.Set(key: string, value: JsonNode) =
        lock gate (fun () -> settings[key] <- value; if active then write())
        changed.Trigger()
    member _.Start() =
        lock gate (fun () ->
            if not active then
                active <- true
                settings["nr"] <- JsonValue.Create(false)
                write()
                timer <- Some(new Timer((fun _ -> lock gate (fun () -> if active then write()); changed.Trigger()), null, 400, 400)))
        changed.Trigger()
    member _.Stop() =
        lock gate (fun () ->
            timer |> Option.iter (fun t -> t.Dispose())
            timer <- None
            if active then
                active <- false
                settings["nr"] <- JsonValue.Create(false)
                write())
        changed.Trigger()
    member _.ReadStatus() =
        try
            use file = new FileStream(Path.Combine(root, "status.json"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite ||| FileShare.Delete)
            use reader = new StreamReader(file)
            let status = JsonNode.Parse(reader.ReadToEnd()).AsObject()
            let age = now() - status["updated_unix_ms"].GetValue<int64>()
            let state = status["state"].GetValue<string>()
            let driverPid = status["driver_pid"].GetValue<int>()
            let alive =
                try
                    use driver = Process.GetProcessById(driverPid)
                    not driver.HasExited
                with _ -> false
            status["nr_applied"].GetValue<bool>() |> ignore
            status["fast_applied"].GetValue<bool>() |> ignore
            status["message"].GetValue<string>() |> ignore
            if status["version"].GetValue<int>() = 1 && age >= 0L && age < 3000L && driverPid > 0 && alive && List.contains state ["waiting_source"; "starting"; "ready"; "applied"; "off"; "budget_exceeded"; "error"] then Some status else None
        with _ -> None
    interface IDisposable with member this.Dispose() = this.Stop()
