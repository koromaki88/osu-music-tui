using System.Text.Json;

namespace OsuIndexer;

public sealed record TrackRecord(
    string stable_id,
    string beatmap_set_id,
    long? online_set_id,
    string title,
    string title_unicode,
    string artist,
    string artist_unicode,
    string creator,
    string audio_filename,
    string audio_hash,
    string audio_path)
{
    public string type => "track";

    // Beatmap.Length is gameplay length, not necessarily the audio duration.
    // Probe the actual media later rather than publishing a misleading value.
    public long? duration_ms => null;
    public double? bpm { get; init; }
    public string? date_added { get; init; }
}

public sealed record IndexResult(string LazerVersion, IReadOnlyList<TrackRecord> Tracks,
    IReadOnlyList<string> Warnings)
{
    public void Write(TextWriter output)
    {
        output.WriteLine(JsonSerializer.Serialize(new
        {
            type = "header",
            schema_version = 1,
            lazer_version = LazerVersion,
        }));
        foreach (TrackRecord track in Tracks)
        {
            output.WriteLine(JsonSerializer.Serialize(track));
        }
        foreach (string warning in Warnings)
        {
            output.WriteLine(JsonSerializer.Serialize(new { type = "warning", message = warning }));
        }
    }
}
