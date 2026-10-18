using System.Security.Cryptography;
using System.Text.Json;
using OsuIndexer;
using Realms;
using Realms.Schema;

if (args.Length != 0 && (args.Length != 2 || args[0] != "--fixture-output"))
{
    Console.Error.WriteLine("usage: osu-indexer-tests [--fixture-output NEW_DIRECTORY]");
    return 2;
}

string root = Path.Combine(Path.GetTempPath(), "osu-indexer-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    RunTests(root);
    if (args.Length == 2)
    {
        string destination = Path.GetFullPath(args[1]);
        if (Directory.Exists(destination) || File.Exists(destination))
        {
            throw new IOException("Fixture output must be a new directory.");
        }
        Directory.CreateDirectory(destination);
        foreach (string name in new[] { "client.realm", "game.ini" })
        {
            File.Copy(Path.Combine(root, name), Path.Combine(destination, name));
        }
        foreach (string source in Directory.GetFiles(Path.Combine(root, "files"), "*", SearchOption.AllDirectories))
        {
            string target = Path.Combine(destination, Path.GetRelativePath(root, source));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(source, target);
        }
        Console.WriteLine("Synthetic fixture exported.");
    }
    Console.WriteLine("all indexer tests passed");
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}
finally
{
    Directory.Delete(root, recursive: true);
}

static void Check(bool condition, string description)
{
    if (!condition)
    {
        throw new InvalidOperationException("FAIL: " + description);
    }
}

static RealmSchema FixtureSchema() => new RealmSchema.Builder
{
    new ObjectSchema.Builder("BeatmapSet")
    {
        Property.Primitive("ID", RealmValueType.Guid, isPrimaryKey: true),
        Property.Primitive("DeletePending", RealmValueType.Bool),
        Property.Primitive("OnlineID", RealmValueType.Int),
        Property.ObjectList("Files", "RealmNamedFileUsage"),
        Property.ObjectList("Beatmaps", "Beatmap"),
    },
    new ObjectSchema.Builder("Beatmap") { Property.Object("Metadata", "BeatmapMetadata") },
    new ObjectSchema.Builder("BeatmapMetadata")
    {
        Property.Primitive("Title", RealmValueType.String),
        Property.Primitive("TitleUnicode", RealmValueType.String),
        Property.Primitive("Artist", RealmValueType.String),
        Property.Primitive("AudioFile", RealmValueType.String),
    },
    new ObjectSchema.Builder("RealmNamedFileUsage", ObjectSchema.ObjectType.EmbeddedObject)
    {
        Property.Primitive("Filename", RealmValueType.String),
        Property.Object("File", "File"),
    },
    new ObjectSchema.Builder("File") { Property.Primitive("Hash", RealmValueType.String) },
}.Build();

static void AddSet(Realm realm, string hash, string audioName, bool deleted = false, int difficulties = 1)
{
    IRealmObjectBase set = realm.DynamicApi.CreateObject("BeatmapSet", Guid.NewGuid());
    set.DynamicApi.Set("DeletePending", deleted);
    set.DynamicApi.Set("OnlineID", 42);
    var file = realm.DynamicApi.CreateObject("File");
    file.DynamicApi.Set("Hash", hash);
    var usages = set.DynamicApi.GetList<IRealmObjectBase>("Files");
    var usage = realm.DynamicApi.AddEmbeddedObjectToList(usages);
    usage.DynamicApi.Set("Filename", "sub/audio.mp3");
    usage.DynamicApi.Set("File", RealmValue.Object(file));
    for (int i = 0; i < difficulties; ++i)
    {
        var metadata = realm.DynamicApi.CreateObject("BeatmapMetadata");
        metadata.DynamicApi.Set("Title", "Example");
        metadata.DynamicApi.Set("TitleUnicode", "例");
        metadata.DynamicApi.Set("Artist", "Test Artist");
        metadata.DynamicApi.Set("AudioFile", audioName);
        var beatmap = realm.DynamicApi.CreateObject("Beatmap");
        beatmap.DynamicApi.Set("Metadata", RealmValue.Object(metadata));
        set.DynamicApi.GetList<IRealmObjectBase>("Beatmaps").Add(beatmap);
    }
}

static Dictionary<string, string> Fingerprint(string root) => Directory.GetFiles(root, "*", SearchOption.AllDirectories)
    .ToDictionary(file => Path.GetRelativePath(root, file),
        // Realm's .note is a notification FIFO on Linux, not readable file content.
        file => file.EndsWith(".realm.note", StringComparison.Ordinal) ? "notification-pipe" :
            Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))));

static void RunTests(string root)
{
    Console.WriteLine("creating synthetic Realm fixture...");
    string hash = new('a', 64);
    string audio = Path.Combine(root, "files", "a", "aa", hash);
    Directory.CreateDirectory(Path.GetDirectoryName(audio)!);
    File.WriteAllText(audio, "synthetic content; the indexer must not try decoding it");
    File.WriteAllText(Path.Combine(root, "game.ini"), "Version = test-fixture\nToken = never-export-this\n");

    var configuration = new RealmConfiguration(Path.Combine(root, "client.realm"))
    {
        Schema = FixtureSchema(),
        IsDynamic = true,
    };
    using (var realm = Realm.GetInstance(configuration))
    {
        realm.Write(() =>
        {
            AddSet(realm, hash.ToUpperInvariant(), "SUB\\Audio.MP3", difficulties: 3);
            AddSet(realm, hash, "sub/audio.mp3"); // Same audio in another set must be retained.
            AddSet(realm, hash, "sub/audio.mp3", deleted: true);
            AddSet(realm, new string('b', 64), "sub/audio.mp3"); // Missing content.
            AddSet(realm, "../../invalid", "sub/audio.mp3");
            AddSet(realm, hash, "no-matching-file.mp3");
        });
    }

    Console.WriteLine("reading fixture through the offline adapter...");
    var before = Fingerprint(root);
    var result = RealmIndexer.Read(root);
    var after = Fingerprint(root);
    Check(before.Count == after.Count && before.All(item => after.GetValueOrDefault(item.Key) == item.Value),
        "source directory contents remain unchanged");
    Check(result.Tracks.Count == 2, "deduplicate difficulties but preserve distinct sets and exclude deleted sets");
    Check(result.Warnings.Count == 3, "warn for missing file, invalid hash, and unresolved filename");
    Check(result.LazerVersion == "test-fixture", "read only version from game.ini");
    Check(result.Tracks.All(track => track.audio_hash == hash && track.audio_path == audio),
        "normalize hashes and resolve audio paths");
    Check(result.Tracks[0].title_unicode == "例", "retain Unicode metadata");
    Check(result.Tracks[0].online_set_id == 42, "read online set ID");
    using var output = new StringWriter();
    result.Write(output);
    Check(!output.ToString().Contains("never-export-this"), "do not export authentication configuration");
    string[] records = output.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries);
    Check(records.Length == 6, "emit header, tracks, and warnings as NDJSON");
    foreach (string line in records)
    {
        using var json = JsonDocument.Parse(line);
        Check(json.RootElement.TryGetProperty("type", out _), "all records have a type");
    }

    string missing = Path.Combine(root, "not-created");
    try
    {
        RealmIndexer.Read(missing);
        throw new InvalidOperationException("FAIL: missing database must fail");
    }
    catch (FileNotFoundException) { }
    Check(!Directory.Exists(missing), "missing source directory is never created");

    string empty = Path.Combine(root, "empty");
    Console.WriteLine("checking empty and incompatible schemas...");
    Directory.CreateDirectory(empty);
    using (var realm = Realm.GetInstance(new RealmConfiguration(Path.Combine(empty, "client.realm"))
        { IsDynamic = true, Schema = FixtureSchema() })) { }
    Check(RealmIndexer.Read(empty).Tracks.Count == 0, "support an empty library");

    string bad = Path.Combine(root, "incompatible");
    Directory.CreateDirectory(bad);
    using (var realm = Realm.GetInstance(new RealmConfiguration(Path.Combine(bad, "client.realm"))
        { IsDynamic = true, Schema = new RealmSchema.Builder
            { new ObjectSchema.Builder("Unrelated") { Property.Primitive("Value", RealmValueType.Int) } }.Build() })) { }
    try
    {
        RealmIndexer.Read(bad);
        throw new InvalidOperationException("FAIL: incompatible schema must fail");
    }
    catch (InvalidDataException) { }
}
