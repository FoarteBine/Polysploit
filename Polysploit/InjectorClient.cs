using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Polysploit
{
    public class InjectorClient : IDisposable
    {
        private Process? _process;
        private StreamWriter? _stdin;
        private readonly BlockingCollection<string> _outputLines = new(new ConcurrentQueue<string>());
        private CancellationTokenSource? _readerCts;
        private Task? _readerTask;
        private readonly SemaphoreSlim _lock = new(1, 1);

        public void Start()
        {
            if (_process is { HasExited: false }) return;

            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Injector.exe");
            if (!File.Exists(path))
                throw new FileNotFoundException("Injector.exe not found at " + path);

            _process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = path,
                    UseShellExecute = false,
                    RedirectStandardInput = true,
                    RedirectStandardOutput = true,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                }
            };
            _process.Start();

            _stdin = _process.StandardInput;

            // Consume startup banner synchronously before background reader starts
            for (int i = 0; i < 3; i++)
                _process.StandardOutput.ReadLine();

            StartBackgroundReader();
        }

        private void StartBackgroundReader()
        {
            _readerCts = new CancellationTokenSource();
            _readerTask = Task.Run(() =>
            {
                try
                {
                    var reader = _process!.StandardOutput;
                    while (!_readerCts.Token.IsCancellationRequested)
                    {
                        string? line = reader.ReadLine();
                        if (line == null) break;
                        _outputLines.Add(line);
                    }
                }
                catch (OperationCanceledException) { }
                catch (ObjectDisposedException) { }
            });
        }

        public async Task<string> SendCommand(string command)
        {
            await _lock.WaitAsync();
            try
            {
                EnsureRunning();

                // Clear any leftover lines from previous responses
                while (_outputLines.TryTake(out _)) { }

                await _stdin!.WriteLineAsync(command);
                await _stdin.FlushAsync();

                var response = new StringBuilder();
                using var cts = new CancellationTokenSource();
                cts.CancelAfter(50);

                while (true)
                {
                    try
                    {
                        string line = _outputLines.Take(cts.Token);
                        response.AppendLine(line);
                        cts.CancelAfter(30);
                    }
                    catch (OperationCanceledException)
                    {
                        break;
                    }
                    catch (InvalidOperationException)
                    {
                        break;
                    }
                }

                return response.ToString().TrimEnd();
            }
            finally
            {
                _lock.Release();
            }
        }

        public async Task<List<ClientItem>> GetClientsAsync()
        {
            string output = await SendCommand("list");
            return ParseClientList(output);
        }

        public static List<ClientItem> ParseClientList(string output)
        {
            var clients = new List<ClientItem>();
            var linePattern = new Regex(@"^PID:\s*(\d+)\s*\|\s*Attached:\s*(YES|NO)\s*\|\s*Player:\s*(.*)$");

            foreach (var line in output.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var match = linePattern.Match(line.Trim());
                if (match.Success)
                {
                    int pid = int.Parse(match.Groups[1].Value);
                    bool attached = match.Groups[2].Value == "YES";
                    clients.Add(new ClientItem
                    {
                        Pid = pid,
                        Name = "Polytoria",
                        IsSelected = true,
                        Status = attached ? ClientStatus.Ready : ClientStatus.NotAttached
                    });
                }
            }

            return clients;
        }

        private void EnsureRunning()
        {
            if (_process == null || _process.HasExited)
                throw new InvalidOperationException("Injector process is not running. Call Start() first.");
        }

        public void Dispose()
        {
            _readerCts?.Cancel();
            _outputLines.CompleteAdding();
            try { if (_process is { HasExited: false }) _process.Kill(); } catch { }
            _readerCts?.Dispose();
            _process?.Dispose();
            _stdin?.Dispose();
            _outputLines.Dispose();
            _lock.Dispose();
        }
    }
}
