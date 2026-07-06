using System.Collections.ObjectModel;
using System.Windows;

namespace Polysploit
{
    public partial class ClientsWindow : Window
    {
        private readonly InjectorClient _injector;
        public ObservableCollection<ClientItem> Clients { get; } = new();

        public ClientsWindow(InjectorClient injector)
        {
            _injector = injector;
            InitializeComponent();
            ClientsList.ItemsSource = Clients;
            Loaded += ClientsWindow_Loaded;
            Closed += (_, _) =>
            {
                foreach (var c in Clients)
                    c.PropertyChanged -= Client_PropertyChanged;
            };
        }

        private async void ClientsWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                var list = await _injector.GetClientsAsync();
                foreach (var c in list)
                {
                    c.PropertyChanged += Client_PropertyChanged;
                    Clients.Add(c);
                }
            }
            catch
            {
                Clients.Add(new ClientItem
                {
                    Pid = 0,
                    Name = "(error fetching clients)",
                    IsSelected = false,
                    Status = ClientStatus.Error
                });
            }
        }

        public void UpdateClients(List<ClientItem> updated)
        {
            Dispatcher.Invoke(() =>
            {
                foreach (var u in updated)
                {
                    var existing = Clients.FirstOrDefault(c => c.Pid == u.Pid);
                    if (existing != null)
                        existing.Status = u.Status;
                }
            });
        }

        private void Client_PropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e) { }

        private void OK_Click(object sender, RoutedEventArgs e) => Close();
        private void Cancel_Click(object sender, RoutedEventArgs e) => Close();
    }
}
