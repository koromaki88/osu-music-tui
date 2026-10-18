using OsuIndexer;

if (args.Length == 1 && args[0] == "--help")
{
    Console.WriteLine("usage: osu-indexer [--osu-dir DIRECTORY]");
    Console.WriteLine("Close lazer before indexing. Writes NDJSON to stdout and diagnostics to stderr.");
    return 0;
}

try
{
    string directory;
    if (args.Length == 0)
    {
        directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".local", "share", "osu");
    }
    else if (args.Length == 2 && args[0] == "--osu-dir")
    {
        directory = args[1];
    }
    else
    {
        Console.Error.WriteLine("usage: osu-indexer [--osu-dir DIRECTORY]");
        return 2;
    }

    // Build the entire result first: an indexing failure must not emit a plausible
    // header-only library that a caller could mistake for a successful refresh.
    IndexResult result = RealmIndexer.Read(directory);
    result.Write(Console.Out);
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine($"indexer error: {error.Message}");
    return 1;
}
