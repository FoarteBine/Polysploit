using System.ComponentModel;
using System.Windows.Media;

namespace Polysploit
{
    public class ClientItem : INotifyPropertyChanged
    {
        private bool _isSelected = true;
        private ClientStatus _status = ClientStatus.NotAttached;

        public int Pid { get; set; }
        public string Name { get; set; } = "";

        public string DisplayText => $"{Name} ({Pid})";

        public bool IsSelected
        {
            get => _isSelected;
            set { _isSelected = value; OnPropertyChanged(); }
        }

        public ClientStatus Status
        {
            get => _status;
            set { _status = value; OnPropertyChanged(); OnPropertyChanged(nameof(StatusColor)); }
        }

        public Brush StatusColor => _status switch
        {
            ClientStatus.Ready => Brushes.LimeGreen,
            ClientStatus.Error => Brushes.Red,
            ClientStatus.Attaching => Brushes.Gold,
            ClientStatus.NotAttached => Brushes.Gray,
            _ => Brushes.Gray
        };

        public event PropertyChangedEventHandler? PropertyChanged;
        private void OnPropertyChanged([System.Runtime.CompilerServices.CallerMemberName] string? name = null)
            => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    public enum ClientStatus
    {
        NotAttached,
        Attaching,
        Ready,
        Error
    }
}
