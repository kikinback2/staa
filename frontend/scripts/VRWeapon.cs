using Godot;

public partial class VRWeapon : Area3D
{
    private DFVRClient _client;

    public override void _Ready()
    {
        _client = GetNodeOrNull<DFVRClient>("/root/DFVRClient");
        BodyEntered += OnBodyEntered;
        AreaEntered += OnAreaEntered;
    }

    private void OnBodyEntered(Node3D body)
    {
        CheckHit(body);
    }

    private void OnAreaEntered(Area3D area)
    {
        CheckHit(area);
    }

    private void CheckHit(Node3D target)
    {
        if (target.HasMeta("body_part_id"))
        {
            int bodyPartId = (int)target.GetMeta("body_part_id");
            GD.Print($"Weapon struck body part ID: {bodyPartId}");
            if (_client != null)
            {
                _client.SimulateWeaponHit(bodyPartId);
            }
        }
    }
}
