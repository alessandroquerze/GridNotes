using System.IO;
using System.Text.Json;

namespace GridNotes;

public static class Storage
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    public static string StatePath =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "GridNotes",
            "state.json");

    public static AppState Load()
    {
        try
        {
            if (!File.Exists(StatePath))
                return new AppState();

            var json = File.ReadAllText(StatePath);
            return JsonSerializer.Deserialize<AppState>(json, JsonOptions) ?? new AppState();
        }
        catch
        {
            return new AppState();
        }
    }

    public static void Save(AppState state)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StatePath)!);
        var json = JsonSerializer.Serialize(state, JsonOptions);
        File.WriteAllText(StatePath, json);
    }
}