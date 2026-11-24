using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace OpenScct.UnrealBridge.Online;

/// <summary>
/// The enhanced maps whose cooked Unreal levels this game has: <c>bundledMaps</c> of the release's version.json
/// (Tools/Package-Release.ps1), or in a development checkout every <c>Content/Imported/Maps/&lt;Map&gt;/&lt;Map&gt;.umap</c>.
/// Their simulation files come from OpenSCCT-Content, so these maps may be downloaded from its index.
/// </summary>
public static class BundledMaps
{
    private static readonly Lazy<HashSet<string>> s_codes = new(Read);

    public static bool Contains(string code) => s_codes.Value.Contains(code);

    public static IReadOnlyCollection<string> Codes => s_codes.Value;

    private static HashSet<string> Read()
    {
        var codes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        string? bridge = Path.GetDirectoryName(typeof(BundledMaps).Assembly.Location);
        if (bridge is null) return codes;
        try
        {
            // <release>/OpenSCCTUnreal/Binaries/Bridge: version.json is in <release>.
            string info = Path.GetFullPath(Path.Combine(bridge, "..", "..", "..", "version.json"));
            if (File.Exists(info))
            {
                using JsonDocument doc = JsonDocument.Parse(File.ReadAllBytes(info));
                if (doc.RootElement.TryGetProperty("bundledMaps", out JsonElement maps) && maps.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement map in maps.EnumerateArray())
                        if (map.GetString() is { Length: > 0 } code) codes.Add(code);
                }
                return codes;
            }

            // <project>/Binaries/Bridge in a checkout: the levels imported on this machine.
            string imported = Path.GetFullPath(Path.Combine(bridge, "..", "..", "Content", "Imported", "Maps"));
            if (Directory.Exists(imported))
            {
                foreach (string folder in Directory.EnumerateDirectories(imported))
                {
                    string code = Path.GetFileName(folder);
                    if (File.Exists(Path.Combine(folder, code + ".umap"))) codes.Add(code);
                }
            }
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException)
        {
            codes.Clear();
        }
        return codes;
    }
}
