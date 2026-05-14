using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace CaseChampGui.Views;

public partial class AuthWelcomeView : UserControl
{
    public AuthWelcomeView()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);
}
