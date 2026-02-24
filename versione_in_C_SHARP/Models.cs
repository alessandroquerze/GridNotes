namespace GridNotes;

public sealed class Tile
{
    public Guid Id { get; set; } = Guid.NewGuid();

    // coordinate e dimensioni in "celle"
    public int X { get; set; }
    public int Y { get; set; }
    public int W { get; set; } = 4;
    public int H { get; set; } = 4;

    public string Text { get; set; } = "";
}

public sealed class AppState
{
    public int CellSize { get; set; } = 48;
    public bool StartWithWindows { get; set; } = false;
    public int WindowWidth { get; set; } = 900;
    public int WindowHeight { get; set; } = 600;

    public List<Tile> Tiles { get; set; } = new();
}