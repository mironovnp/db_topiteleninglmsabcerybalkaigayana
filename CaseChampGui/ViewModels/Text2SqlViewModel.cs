using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class Text2SqlViewModel : ObservableObject
{
    private readonly IText2SqlService _service;
    private string _inputText = string.Empty;

    public Text2SqlViewModel(IText2SqlService service)
    {
        _service = service;
    }

    public string InputText
    {
        get => _inputText;
        set => SetProperty(ref _inputText, value);
    }

    public bool IsEnabled => _service.IsEnabled;

    public string HintTitle => "Text2SQL появится здесь";

    public string HintSubtitle =>
        "Скоро в этом окне можно будет писать запросы на русском языке, " +
        "а они будут автоматически переводиться в SQL. Сейчас раздел работает в режиме предпросмотра.";
}
