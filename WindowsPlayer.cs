using System.Linq;
using System.Windows;
using System.Windows.Media;

namespace GridNotes;

public static class WindowPlacer
{
    public static void PlaceOnSecondScreenCenter(Window w, double widthDip, double heightDip)
    {
        var screens = System.Windows.Forms.Screen.AllScreens;
        var target = screens.FirstOrDefault(s => !s.Primary) ?? System.Windows.Forms.Screen.PrimaryScreen;

        var boundsPx = target.WorkingArea;

        var dpi = VisualTreeHelper.GetDpi(w);
        double sx = dpi.DpiScaleX;
        double sy = dpi.DpiScaleY;

        double leftDip = boundsPx.Left / sx;
        double topDip = boundsPx.Top / sy;
        double widthWorkDip = boundsPx.Width / sx;
        double heightWorkDip = boundsPx.Height / sy;

        w.WindowStartupLocation = WindowStartupLocation.Manual;
        w.Width = widthDip;
        w.Height = heightDip;

        w.Left = leftDip + (widthWorkDip - widthDip) / 2.0;
        w.Top = topDip + (heightWorkDip - heightDip) / 2.0;
    }
}