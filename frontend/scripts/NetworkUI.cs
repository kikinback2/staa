using Godot;

public partial class NetworkUI : Node3D
{
    private Label _statusLabel;
    private LineEdit _ipInput;
    private Button _connectButton;
    private DFVRClient _client;

    public override void _Ready()
    {
        var viewport = GetNode<SubViewport>("SubViewPort");
        _statusLabel = viewport.GetNode<Label>("Control/VBoxContainer/StatusLabel");
        _ipInput = viewport.GetNode<LineEdit>("Control/VBoxContainer/IPInput");
        _connectButton = viewport.GetNode<Button>("Control/VBoxContainer/ConnectButton");
        
        _client = GetNodeOrNull<DFVRClient>("/root/DFVRClient");

        _connectButton.Pressed += OnConnectPressed;
        
        var sprite = GetNode<Sprite3D>("Sprite3D");
        sprite.Texture = viewport.GetTexture();
    }

    private void OnConnectPressed()
    {
        string ip = _ipInput.Text;
        if (!string.IsNullOrEmpty(ip) && _client != null)
        {
            _statusLabel.Text = $"Connecting to {ip}...";
            _client.ConnectToHost(ip, 9000);
        }
    }

    public void UpdateStatus(string status)
    {
        if (_statusLabel != null)
        {
            _statusLabel.Text = status;
        }
    }
}
