using Godot;

public partial class PlayerRig : XROrigin3D
{
    public override void _Ready()
    {
        var xrInterface = XRServer.FindInterface("OpenXR");
        if (xrInterface != null && xrInterface.Initialize())
        {
            GetViewport().UseXR = true;
            GD.Print("OpenXR initialized successfully.");
        }
        else
        {
            GD.PrintErr("OpenXR interface not found or failed to initialize. Running in desktop/mock mode.");
        }
    }
}
