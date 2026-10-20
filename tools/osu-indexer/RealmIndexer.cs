using Realms;

namespace OsuIndexer;

public static class RealmIndexer
{
    public static IndexResult Read(string directory)
    {
        string root = Path.GetFullPath(directory);
        string database = Path.Combine(root, "client.realm");
        if (!File.Exists(database))
        {
            throw new FileNotFoundException("client.realm not found; pass --osu-dir with the lazer data directory.");
        }

        // Never open the source with Realm itself: even SDK lock/management files
        // belong in our temporary directory. This is an offline copy, not a safe
        // concurrent snapshot; the user must close lazer first.
        // CreateTempSubdirectory creates a unique private directory (0700 on Unix),
        // so the copied database is not exposed to other local users.
        string temporary = Directory.CreateTempSubdirectory("osu-indexer-").FullName;
        try
        {
            var before = new FileInfo(database);
            long size = before.Length;
            DateTime modified = before.LastWriteTimeUtc;
            string copy = Path.Combine(temporary, "client.realm");
            File.Copy(database, copy);
            var after = new FileInfo(database);
            if (size != after.Length || modified != after.LastWriteTimeUtc)
            {
                throw new IOException("client.realm changed while copying; close lazer and retry.");
            }

            var configuration = new RealmConfiguration(copy)
            {
                IsDynamic = true,
                IsReadOnly = true,
                ShouldDeleteIfMigrationNeeded = false,
            };
            using var realm = Realm.GetInstance(configuration);
            ValidateSchema(realm);
            return Extract(realm, root, ReadVersion(root));
        }
        finally
        {
            // The only deletion target is the fresh, app-owned temporary directory.
            Directory.Delete(temporary, recursive: true);
        }
    }

    private static void ValidateSchema(Realm realm)
    {
        var required = new Dictionary<string, string[]>
        {
            ["BeatmapSet"] = ["ID", "DeletePending", "Files", "Beatmaps"],
            ["Beatmap"] = ["Metadata"],
            ["BeatmapMetadata"] = ["AudioFile", "Title", "Artist"],
            ["RealmNamedFileUsage"] = ["Filename", "File"],
            ["File"] = ["Hash"],
        };
        foreach (var (table, columns) in required)
        {
            var schema = realm.Schema.FirstOrDefault(item => item.Name == table);
            if (schema is null || columns.Any(column => !schema.TryFindProperty(column, out _)))
            {
                throw new InvalidDataException($"Unsupported lazer schema: missing table or properties in {table}.");
            }
        }
    }

    private static IndexResult Extract(Realm realm, string root, string version)
    {
        var tracks = new List<TrackRecord>();
        var warnings = new List<string>();
        var seen = new HashSet<(Guid, string)>();
        foreach (IRealmObject set in realm.DynamicApi.All("BeatmapSet"))
        {
            if (set.DynamicApi.Get<bool>("DeletePending"))
            {
                continue;
            }

            Guid setId = set.DynamicApi.Get<Guid>("ID");
            var files = new Dictionary<string, IRealmObjectBase>(StringComparer.OrdinalIgnoreCase);
            foreach (var usage in set.DynamicApi.GetList<IRealmObjectBase>("Files"))
            {
                string name = usage.DynamicApi.Get<string>("Filename").Replace('\\', '/');
                if (!files.TryAdd(name, usage))
                {
                    warnings.Add($"Set {setId}: ambiguous filename mapping; first match retained.");
                }
            }

            foreach (var beatmap in set.DynamicApi.GetList<IRealmObjectBase>("Beatmaps"))
            {
                var metadata = beatmap.DynamicApi.Get<IRealmObjectBase?>("Metadata");
                if (metadata is null)
                {
                    warnings.Add($"Set {setId}: beatmap has no metadata.");
                    continue;
                }

                string audioName = metadata.DynamicApi.Get<string>("AudioFile");
                if (!files.TryGetValue(audioName.Replace('\\', '/'), out var usage))
                {
                    warnings.Add($"Set {setId}: audio filename has no file-store mapping.");
                    continue;
                }

                var file = usage.DynamicApi.Get<IRealmObjectBase?>("File");
                string hash = file is null ? "" : file.DynamicApi.Get<string>("Hash");
                if (hash.Length != 64 || hash.Any(character => !char.IsAsciiHexDigit(character)))
                {
                    warnings.Add($"Set {setId}: invalid or missing audio SHA-256 hash.");
                    continue;
                }

                hash = hash.ToLowerInvariant();
                if (!seen.Add((setId, hash)))
                {
                    continue;
                }

                string path = Path.Combine(root, "files", hash[..1], hash[..2], hash);
                try
                {
                    using var audio = File.OpenRead(path);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    warnings.Add($"Set {setId}: audio file {hash} is missing or unreadable.");
                    continue;
                }

                string title = metadata.DynamicApi.Get<string>("Title");
                string artist = metadata.DynamicApi.Get<string>("Artist");
                if (string.IsNullOrWhiteSpace(title) || string.IsNullOrWhiteSpace(artist))
                {
                    warnings.Add($"Set {setId}: title or artist is empty.");
                    continue;
                }

                var author = Optional<IRealmObjectBase?>(metadata, "Author");
                long? onlineId = Optional<long?>(set, "OnlineID");
                double? bpm = Optional<double?>(beatmap, "BPM");
                DateTimeOffset? added = Optional<DateTimeOffset?>(set, "DateAdded");
                tracks.Add(new TrackRecord($"{setId:D}:{hash}", setId.ToString("D"),
                    onlineId > 0 ? onlineId : null, title,
                    Optional<string>(metadata, "TitleUnicode") ?? "", artist,
                    Optional<string>(metadata, "ArtistUnicode") ?? "",
                    author is null ? "" : Optional<string>(author, "Username") ?? "",
                    audioName, hash, path)
                {
                    bpm = bpm.HasValue && double.IsFinite(bpm.Value) && bpm.Value > 0 ? bpm : null,
                    date_added = added.HasValue && added.Value != DateTimeOffset.MinValue
                        ? added.Value.ToUniversalTime().ToString("O", System.Globalization.CultureInfo.InvariantCulture) : null,
                });
            }
        }

        tracks.Sort((left, right) => StringComparer.Ordinal.Compare(left.stable_id, right.stable_id));
        return new IndexResult(version, tracks, warnings);
    }

    private static T? Optional<T>(IRealmObjectBase item, string property)
    {
        return item.ObjectSchema!.TryFindProperty(property, out _)
            ? item.DynamicApi.Get<T>(property) : default;
    }

    private static string ReadVersion(string root)
    {
        string configuration = Path.Combine(root, "game.ini");
        if (!File.Exists(configuration))
        {
            return "unknown";
        }

        foreach (string line in File.ReadLines(configuration))
        {
            int equals = line.IndexOf('=');
            if (equals >= 0 && line[..equals].Trim() == "Version")
            {
                return line[(equals + 1)..].Trim();
            }
        }
        return "unknown";
    }
}
