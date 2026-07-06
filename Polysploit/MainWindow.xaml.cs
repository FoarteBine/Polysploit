using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Input;

namespace Polysploit
{
    public partial class MainWindow : Window
    {
        private string? _currentFilePath;
        private ClientsWindow? _clientsWindow;
        private readonly InjectorClient _injector = new();

        public MainWindow()
        {
            InitializeComponent();
            Loaded += MainWindow_Loaded;
            Closed += (_, _) => _injector.Dispose();
        }

        private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try { _injector.Start(); }
            catch (Exception ex) { Output("Injector: " + ex.Message); }

            var env = await Microsoft.Web.WebView2.Core.CoreWebView2Environment.CreateAsync(
                userDataFolder: Path.Combine(Path.GetTempPath(), "PolysploitWebView"));
            await MonacoEditor.EnsureCoreWebView2Async(env);

            string monacoPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "monaco", "index.html");
            if (File.Exists(monacoPath))
            {
                MonacoEditor.Source = new Uri(monacoPath);
            }
            else
            {
                Output("Monaco editor not found at: " + monacoPath);
            }
        }

        private void Toolbar_MouseDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left)
                DragMove();
        }

        private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
        private void Close_Click(object sender, RoutedEventArgs e) => Close();

        private async void Execute_Click(object sender, RoutedEventArgs e)
        {
            string? code = await GetEditorContent();
            if (string.IsNullOrWhiteSpace(code))
            {
                Output("No code to execute.");
                return;
            }

            Output(">> Executing...");
            try
            {
                // Strip Lua comments (safe — no execution effect) to avoid protocol issues
                code = System.Text.RegularExpressions.Regex.Replace(code, @"--\[(=*)\[.*?\]\1\]", "", System.Text.RegularExpressions.RegexOptions.Singleline);
                code = System.Text.RegularExpressions.Regex.Replace(code, @"--[^\n\r]*", "", System.Text.RegularExpressions.RegexOptions.Multiline);
                // Force single line for the line-based protocol
                string flat = code.Replace("\r\n", " ").Replace("\n", " ").Replace("\r", " ");
                var clients = await _injector.GetClientsAsync();
                int done = 0;
                foreach (var c in clients)
                {
                    if (c.Status != ClientStatus.Ready) continue;
                    string result = await _injector.SendCommand($"execute {c.Pid} {flat}");
                    Output(result);
                    done++;
                }
                if (done == 0) Output("No attached clients.");
            }
            catch (Exception ex) { Output("Error: " + ex.Message); }
        }

        private async void Clear_Click(object sender, RoutedEventArgs e)
        {
            await SetEditorContent("");
            OutputBox.Clear();
        }

        private async void OpenFile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new Microsoft.Win32.OpenFileDialog
            {
                Filter = "Lua files (*.lua)|*.lua|All files (*.*)|*.*"
            };
            if (dlg.ShowDialog() == true)
            {
                _currentFilePath = dlg.FileName;
                var content = File.ReadAllText(dlg.FileName);
                await SetEditorContent(content);
                Output(">> Opened: " + dlg.FileName);
            }
        }

        private async void SaveFile_Click(object sender, RoutedEventArgs e)
        {
            var content = await GetEditorContent();
            if (content == null) return;

            if (_currentFilePath != null)
            {
                File.WriteAllText(_currentFilePath, content);
                Output(">> Saved: " + _currentFilePath);
            }
            else
            {
                var dlg = new Microsoft.Win32.SaveFileDialog
                {
                    Filter = "Lua files (*.lua)|*.lua|All files (*.*)|*.*",
                    FileName = "script.lua"
                };
                if (dlg.ShowDialog() == true)
                {
                    _currentFilePath = dlg.FileName;
                    File.WriteAllText(dlg.FileName, content);
                    Output(">> Saved: " + dlg.FileName);
                }
            }
        }

        private async void ExecuteFile_Click(object sender, RoutedEventArgs e)
        {
            string? code;
            if (_currentFilePath != null)
            {
                code = File.ReadAllText(_currentFilePath);
                await SetEditorContent(code);
            }
            else
            {
                code = await GetEditorContent();
            }

            if (string.IsNullOrWhiteSpace(code))
            {
                Output("No file loaded.");
                return;
            }

            Output(">> Executing file...");
            try
            {
                string flat = code.Replace("\r\n", " ").Replace("\n", " ");
                var clients = await _injector.GetClientsAsync();
                int done = 0;
                foreach (var c in clients)
                {
                    if (c.Status != ClientStatus.Ready) continue;
                    string result = await _injector.SendCommand($"execute {c.Pid} {flat}");
                    Output(result);
                    done++;
                }
                if (done == 0) Output("No attached clients.");
            }
            catch (Exception ex) { Output("Error: " + ex.Message); }
        }

        private void Clients_Click(object sender, RoutedEventArgs e)
        {
            if (_clientsWindow != null)
            {
                _clientsWindow.Focus();
                return;
            }

            _clientsWindow = new ClientsWindow(_injector);
            _clientsWindow.Owner = this;
            _clientsWindow.Closed += (_, _) => _clientsWindow = null;
            _clientsWindow.Show();
        }

        private async void Attach_Click(object sender, RoutedEventArgs e)
        {
            Output(">> Attaching...");
            try
            {
                string result = await _injector.SendCommand("attach");
                Output(result);
                // refresh client list after attach
                var clients = await _injector.GetClientsAsync();
                if (_clientsWindow != null)
                    _clientsWindow.UpdateClients(clients);
            }
            catch (Exception ex) { Output("Error: " + ex.Message); }
        }

        private async void Unload_Click(object sender, RoutedEventArgs e)
        {
            Output(">> Unloading DLL...");
            try
            {
                string result = await _injector.SendCommand("unload");
                Output(result);
            }
            catch (Exception ex) { Output("Error: " + ex.Message); }
        }

        private void Output(string message)
        {
            Dispatcher.Invoke(() =>
            {
                OutputBox.AppendText(message + "\n");
                OutputBox.ScrollToEnd();
            });
        }
        private async Task<string?> GetEditorContent()
        {
            try
            {
                var result = await MonacoEditor.ExecuteScriptAsync("GetText()");
                if (result == null) return null;
                return JsonSerializer.Deserialize<string>(result);
            }
            catch
            {
                return null;
            }
        }

        private async Task SetEditorContent(string content)
        {
            try
            {
                var escaped = JsonSerializer.Serialize(content);
                await MonacoEditor.ExecuteScriptAsync($"SetText({escaped})");
            }
            catch { }
        }

    }
}
