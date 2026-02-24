using Microsoft.Win32;

namespace GridNotes;

public static class Startup
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static bool IsEnabled(string appName)
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey, false);
        return key?.GetValue(appName) != null;
    }

    public static void SetEnabled(string appName, string exePath, bool enabled)
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey, true)
                      ?? Registry.CurrentUser.CreateSubKey(RunKey);

        if (enabled)
            key.SetValue(appName, $"\"{exePath}\"");
        else
            key.DeleteValue(appName, false);
    }
}