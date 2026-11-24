using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using OpenScct.Maps;
using OpenScct.Server;

namespace OpenScct.UnrealBridge.Online;

/// <summary>
/// The client's downloads from OpenSCCT-Content (docs/CONTENT-PACKS.md in OpenSCCT), ported from the Godot client's
/// game/Scripts/Online/ContentPacks.cs, which has no Godot in it: the content index, read in the background and kept
/// for ten minutes, and one download job per eligible community map. Downloads go into <see cref="ContentPaths.PacksFolder"/>, whose
/// <c>svmap</c> every map catalog of this client already searches, so a map downloaded here plays offline and online
/// through the runtime map level once the catalogs are read again (<see cref="Generation"/>).
/// <para>Thread-safe: the downloads run on worker threads, Unreal polls <see cref="Job"/> and <see cref="Generation"/>
/// from the game thread.</para>
/// <para>The Godot client's in-browser paths (a same-origin store, bot navigation fetched with the map) are left out:
/// the Unreal client runs on Windows only.</para>
/// </summary>
public static class ContentPacks
{
    /// <summary>Seconds an index is kept before it is fetched again.</summary>
    public const double IndexSeconds = 600;

    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromMinutes(30) };
    private static readonly object Gate = new();
    private static readonly Dictionary<string, DownloadJob> Jobs = new(StringComparer.OrdinalIgnoreCase);
    private static ContentStore? s_store;
    private static string? s_storeRoot;
    private static Task<ContentIndex>? s_index;
    private static DateTime s_indexAt;
    private static int s_generation;

    /// <summary>One map's (or shared pack's, or bot navigation's) download as the menus show it.</summary>
    public sealed class DownloadJob
    {
        internal DownloadJob(ContentMap map)
        {
            Map = map;
            Title = map.DisplayTitle;
        }

        internal DownloadJob(string title) => Title = title;

        /// <summary>The map being downloaded, or null for a shared pack or bot navigation.</summary>
        public ContentMap? Map { get; }

        /// <summary>The map's title, or the shared pack's.</summary>
        public string Title { get; }

        /// <summary>What is being fetched: the shared packs, then the map.</summary>
        public string Step { get; internal set; } = "Starting";

        public long Received { get; internal set; }

        public long Total { get; internal set; }

        public bool Done { get; internal set; }

        /// <summary>Why it failed (or "stopped"), else null.</summary>
        public string? Error { get; internal set; }

        /// <summary>Where the map was installed, once done.</summary>
        public string? InstalledFolder { get; internal set; }

        /// <summary>Still downloading or installing.</summary>
        public bool Running => !Done && Error is null;

        /// <summary>The share received of what is downloading now (0 to 1; 0 while installing).</summary>
        public double Fraction => Total > 0 ? Math.Clamp((double)Received / Total, 0, 1) : 0;

        internal CancellationTokenSource Stop { get; } = new();

        /// <summary>Stops the download (a half-made map is never installed).</summary>
        public void Cancel() => Stop.Cancel();

        /// <summary>"Downloading Aquarius: 12.3 / 31.0 MB", "Installing Aquarius", the error, or "Aquarius downloaded".</summary>
        public string Describe() =>
            Error is { } e ? $"{Title}: {e}" :
            Done ? $"{Title} downloaded" :
            Total > 0 ? string.Create(CultureInfo.InvariantCulture, $"{Step}: {Received / 1048576.0:0.0} / {Total / 1048576.0:0.0} MB") :
            Step;

        /// <summary>
        /// The Content page's short form under a row that already names the map: "Downloading 12.3 / 31.0 MB",
        /// "Installing". A step that fetches a shared pack first keeps that pack's name.
        /// </summary>
        public string DescribeBrief()
        {
            string step = Step == $"Downloading {Title}" ? "Downloading" : Step == $"Installing {Title}" ? "Installing" : Step;
            return Error is not null || Done ? Describe() :
                Total > 0 ? string.Create(CultureInfo.InvariantCulture, $"{step} {Received / 1048576.0:0.0} / {Total / 1048576.0:0.0} MB") :
                step;
        }
    }

    /// <summary>
    /// The store downloads go into: the packs folder as <see cref="BridgeSession.Configure"/> set it. Made again when
    /// that folder changed (a test process moves it with OPENSCCT_UNREAL_PACKS) and nothing is downloading.
    /// </summary>
    public static ContentStore Store
    {
        get
        {
            lock (Gate)
            {
                return StoreLocked();
            }
        }
    }

    private static ContentStore StoreLocked()
    {
        string root = ContentPaths.PacksFolder;
        if (s_store is null || (!string.Equals(s_storeRoot, root, StringComparison.OrdinalIgnoreCase) && !AnyRunningLocked()))
        {
            s_store = new ContentStore(root, Http)
            {
                ResolveCommunityBuilder = () => CommunityBuildContext.ResolveUnreal(BridgeSession.UnrealContentDirectory, BridgeSession.UnrealToolsDirectory),
                AllowBundledMap = map => BundledMaps.Contains(map.Code),
            };
            s_storeRoot = root;
        }

        return s_store;
    }

    private static bool AnyRunningLocked()
    {
        foreach (DownloadJob job in Jobs.Values)
        {
            if (job.Running)
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>Moves on each time a map or shared pack finished installing (the catalogs are then read again).</summary>
    public static int Generation => Volatile.Read(ref s_generation);

    /// <summary>The index once read, else null (still loading, or it failed: <see cref="IndexError"/>).</summary>
    public static ContentIndex? Index
    {
        get
        {
            RefreshIndex();
            lock (Gate)
            {
                return s_index is { IsCompletedSuccessfully: true } t ? t.Result : null;
            }
        }
    }

    /// <summary>Why the index could not be read, else null.</summary>
    public static string? IndexError
    {
        get
        {
            lock (Gate)
            {
                return s_index is { IsFaulted: true } t ? NetworkDiagnostics.Describe(t.Exception!) : null;
            }
        }
    }

    /// <summary>Starts reading the index when there is none, it failed a minute ago, or it is old.</summary>
    public static void RefreshIndex(bool force = false)
    {
        lock (Gate)
        {
            double age = (DateTime.UtcNow - s_indexAt).TotalSeconds;
            bool stale = s_index is null || force || (s_index.IsFaulted && age > 60) || (s_index.IsCompletedSuccessfully && age > IndexSeconds);
            if (!stale || s_index is { IsCompleted: false })
            {
                return;
            }

            s_indexAt = DateTime.UtcNow;
            ContentStore store = StoreLocked();
            s_index = Task.Run(() => FetchIndexLoggedAsync(store));
        }
    }

    private static async Task<ContentIndex> FetchIndexLoggedAsync(ContentStore store)
    {
        try
        {
            return await store.FetchIndexAsync(CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception error)
        {
            NetworkDiagnostics.Report("content index download", error);
            throw;
        }
    }

    /// <summary>The download of a key (a map code, or <see cref="NavigationKey"/>), running or finished, else null.</summary>
    public static DownloadJob? Job(string key)
    {
        lock (Gate)
        {
            return Jobs.TryGetValue(key, out DownloadJob? j) ? j : null;
        }
    }

    /// <summary>The key a map's bot navigation download is kept under.</summary>
    public static string NavigationKey(string map) => "nav:" + map;

    /// <summary>The map of the index for a code, else null.</summary>
    public static ContentMap? Find(string code, string? contentHash = null) => SteamVersusMaps.Contains(code) ? null : Index?.FindMap(code, contentHash);

    /// <summary>
    /// Removes a downloaded map (its folder under packs\svmap and its entry) and forgets its finished download. A
    /// running download is left alone.
    /// </summary>
    public static void Remove(string code)
    {
        lock (Gate)
        {
            if (Jobs.TryGetValue(code, out DownloadJob? job))
            {
                if (job.Running)
                {
                    return;
                }

                Jobs.Remove(code);
            }
        }

        Store.RemoveMap(code);
        Interlocked.Increment(ref s_generation);
    }

    /// <summary>
    /// Downloads a community map for the local Steam conversion.
    /// </summary>
    public static DownloadJob Download(ContentMap map)
    {
        ArgumentNullException.ThrowIfNull(map);
        if (!Store.MayDownload(map))
            return new DownloadJob(map) { Error = "Only reviewed community content and this release's enhanced maps are available for download." };
        if (!ContentPaths.SteamBaseReady)
            return new DownloadJob(map) { Error = "Use Set up from Steam in OpenSCCT Unreal first." };
        DownloadJob job;
        lock (Gate)
        {
            // A finished job is not handed out again: the index or the host may have a newer version by now.
            if (Jobs.TryGetValue(map.Code, out DownloadJob? running) && running.Running)
            {
                return running;
            }

            job = new DownloadJob(map);
            Jobs[map.Code] = job;
        }

        _ = Task.Run(() => RunAsync(job));
        return job;
    }

    /// <summary>Downloads a map's prebuilt bot navigation into the packs folder, where the bots' navigation cache looks for it.</summary>
    public static DownloadJob DownloadBotNavigation(string map, ContentBotNavigation navigation)
    {
        ArgumentNullException.ThrowIfNull(navigation);
        DownloadJob job;
        lock (Gate)
        {
            if (Jobs.TryGetValue(NavigationKey(map), out DownloadJob? running) && running.Running)
            {
                return running;
            }

            job = new DownloadJob("Bot navigation");
            Jobs[NavigationKey(map)] = job;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                job.Step = "Downloading bot navigation";
                await Store.InstallBotNavigationAsync(navigation,
                    path => OpenScct.Bots.AiNavCache.PackMatches(path, map, navigation.SimHash, navigation.ActivationMask),
                    Progress(job), job.Stop.Token).ConfigureAwait(false);
                job.InstalledFolder = ContentStore.BotNavigationPath(Store.Root, navigation.SimHash, navigation.FormatVersion);
                job.Done = true;
            }
            catch (OperationCanceledException) when (job.Stop.IsCancellationRequested)
            {
                job.Error = "stopped";
            }
            catch (Exception e) when (e is HttpRequestException or IOException or InvalidDataException or UnauthorizedAccessException or TaskCanceledException)
            {
                NetworkDiagnostics.Report("bot navigation download: " + map, e);
                job.Error = e is TaskCanceledException ? "the download timed out" : NetworkDiagnostics.Describe(e);
            }
        });
        return job;
    }

    private static async Task RunAsync(DownloadJob job)
    {
        ContentStore store = Store;
        try
        {
            job.Step = $"Downloading {job.Title}";
            await store.InstallMapAsync(job.Map!, ContentKind.Full, Progress(job), job.Stop.Token).ConfigureAwait(false);
            job.InstalledFolder = Path.Combine(store.SvMapRoot, job.Map!.Code);
            job.Done = true;
            Interlocked.Increment(ref s_generation);
        }
        catch (OperationCanceledException) when (job.Stop.IsCancellationRequested)
        {
            job.Error = "stopped";
        }
        catch (Exception e) when (e is HttpRequestException or IOException or InvalidDataException or UnauthorizedAccessException or TaskCanceledException or InvalidOperationException)
        {
            NetworkDiagnostics.Report("content download: " + job.Title, e);
            job.Error = e is TaskCanceledException ? "the download timed out" : NetworkDiagnostics.Describe(e);
        }
    }

    private static Progress<PackProgress> Progress(DownloadJob job) => new(p =>
    {
        job.Received = p.Received;
        job.Total = p.Total;
        if (p.Extracting)
        {
            job.Step = $"Installing {job.Title}";
            job.Total = 0;
        }
    });
}
