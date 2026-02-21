using System;
using System.Windows.Threading;

namespace GridNotes;

public sealed class Debouncer
{
    private readonly DispatcherTimer _timer;
    private Action? _action;

    public Debouncer(TimeSpan delay)
    {
        _timer = new DispatcherTimer { Interval = delay };
        _timer.Tick += (_, _) =>
        {
            _timer.Stop();
            _action?.Invoke();
        };
    }

    public void Run(Action action)
    {
        _action = action;
        _timer.Stop();
        _timer.Start();
    }
}