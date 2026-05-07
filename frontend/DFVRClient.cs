using Godot;
using System;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using Google.Protobuf;
using Dfvr;

public partial class DFVRClient : Node
{
    private TcpClient _client;
    private NetworkStream _stream;
    private bool _isRunning = false;

    [Export] public string ServerIp = "127.0.0.1";
    [Export] public int ServerPort = 9000;

    private GameStatePayload _latestPayload;
    private readonly object _payloadLock = new object();

    private Queue<ActionRequest> _pendingActions = new Queue<ActionRequest>();
    private readonly object _actionLock = new object();

    public override void _Ready()
    {
        _ = ConnectAndHandshakeAsync(ServerIp, ServerPort);
    }

    public void SendAction(int optionId)
    {
        lock (_actionLock)
        {
            _pendingActions.Enqueue(new ActionRequest { SelectedOptionId = optionId });
        }
    }

    public void SimulateWeaponHit(int hitBodyPartId)
    {
        GD.Print($"Simulating weapon hit on body part ID: {hitBodyPartId}");
        SendAction(hitBodyPartId);
    }

    private async Task ConnectAndHandshakeAsync(string ip, int port)
    {
        try
        {
            _client = new TcpClient();
            await _client.ConnectAsync(ip, port);
            _stream = _client.GetStream();
            
            GD.Print("Connected to server, initiating handshake...");

            var request = new HandshakeRequest
            {
                ClientVersion = "Godot VR Client 0.1a",
                Status = "Ready"
            };

            byte[] requestBytes = request.ToByteArray();
            byte[] lengthPrefix = BitConverter.GetBytes(requestBytes.Length);
            
            await _stream.WriteAsync(lengthPrefix, 0, lengthPrefix.Length);
            await _stream.WriteAsync(requestBytes, 0, requestBytes.Length);
            
            byte[] responseLengthBytes = new byte[4];
            int bytesRead = await _stream.ReadAsync(responseLengthBytes, 0, 4);
            if (bytesRead < 4) return;

            int responseLength = BitConverter.ToInt32(responseLengthBytes, 0);
            byte[] responseBytes = new byte[responseLength];
            
            int totalRead = 0;
            while (totalRead < responseLength)
            {
                int read = await _stream.ReadAsync(responseBytes, totalRead, responseLength - totalRead);
                if (read == 0) throw new Exception("Connection closed while reading response.");
                totalRead += read;
            }

            var response = HandshakeResponse.Parser.ParseFrom(responseBytes);
            GD.Print($"Handshake Successful! Server Version: {response.ServerVersion}, Time: {response.WorldTime}");

            _isRunning = true;
            _ = Task.Run(NetworkLoopAsync);
        }
        catch (Exception e)
        {
            GD.PrintErr($"Handshake failed: {e.Message}");
        }
    }

    private async Task NetworkLoopAsync()
    {
        byte[] lengthBytes = new byte[4];

        while (_isRunning)
        {
            try
            {
                lock (_actionLock)
                {
                    while (_pendingActions.Count > 0)
                    {
                        var action = _pendingActions.Dequeue();
                        byte[] actionBytes = action.ToByteArray();
                        byte[] lengthPrefix = BitConverter.GetBytes(actionBytes.Length);
                        
                        _stream.Write(lengthPrefix, 0, lengthPrefix.Length);
                        _stream.Write(actionBytes, 0, actionBytes.Length);
                    }
                }

                if (_stream.DataAvailable)
                {
                    int lengthRead = 0;
                    while (lengthRead < 4)
                    {
                        int read = await _stream.ReadAsync(lengthBytes, lengthRead, 4 - lengthRead);
                        if (read == 0) throw new Exception("Connection closed while reading length prefix.");
                        lengthRead += read;
                    }

                    int payloadLength = BitConverter.ToInt32(lengthBytes, 0);
                    if (payloadLength <= 0 || payloadLength > 10 * 1024 * 1024) throw new Exception($"Invalid payload length: {payloadLength}");

                    byte[] payloadBytes = new byte[payloadLength];
                    int totalRead = 0;
                    while (totalRead < payloadLength)
                    {
                        int r = await _stream.ReadAsync(payloadBytes, totalRead, payloadLength - totalRead);
                        if (r == 0) throw new Exception("Connection closed while reading payload.");
                        totalRead += r;
                    }

                    var payload = GameStatePayload.Parser.ParseFrom(payloadBytes);
                    if (payload != null)
                    {
                        lock (_payloadLock)
                        {
                            _latestPayload = payload;
                        }
                    }
                }
                else
                {
                    await Task.Delay(10);
                }
            }
            catch (Exception e)
            {
                GD.PrintErr($"Network Error: {e.Message}");
                _isRunning = false;
            }
        }
    }

    public override void _Process(double delta)
    {
        GameStatePayload payloadToProcess = null;

        lock (_payloadLock)
        {
            if (_latestPayload != null)
            {
                payloadToProcess = _latestPayload;
                _latestPayload = null;
            }
        }

        if (payloadToProcess != null)
        {
            ProcessPayload(payloadToProcess);
        }
    }

    private void ProcessPayload(GameStatePayload payload)
    {
        foreach (var entity in payload.Entities)
        {
            UpdateEntityInGodot(entity);
        }
        
        foreach (var block in payload.MapBlocks)
        {
            UpdateMapBlockInGodot(block);
        }
    }

    private void UpdateEntityInGodot(EntityData entity)
    {
        string nodeName = $"Unit_{entity.EntityId}";
        Node3D unitNode = GetNodeOrNull<Node3D>(nodeName);

        if (unitNode == null)
        {
            unitNode = new Node3D();
            unitNode.Name = nodeName;
            AddChild(unitNode);
            GD.Print($"Spawned unit: {entity.Name}");
        }

        unitNode.Position = new Vector3(entity.Position.X, entity.Position.Y, entity.Position.Z);

        foreach (var part in entity.BodyParts)
        {
            UpdateBodyPartHitbox(unitNode, part);
        }
    }

    private void UpdateBodyPartHitbox(Node3D parent, BodyPart part)
    {
        string partName = $"Part_{part.Id}";
        Area3D partNode = parent.GetNodeOrNull<Area3D>(partName);
        
        if (partNode == null)
        {
            partNode = new Area3D();
            partNode.Name = partName;
            partNode.CollisionLayer = 2; // Enemy layer
            partNode.CollisionMask = 0;
            
            var collisionShape = new CollisionShape3D();
            var shape = new CapsuleShape3D();
            collisionShape.Shape = shape;
            
            partNode.AddChild(collisionShape);
            parent.AddChild(partNode);
            
            partNode.SetMeta("body_part_id", part.Id);
            partNode.SetMeta("entity_id", parent.Name.Replace("Unit_", ""));
        }
        
        float scale = (float)Math.Pow(part.SizeVolume / 5000.0, 1.0/3.0);
        var capShape = (CapsuleShape3D)((CollisionShape3D)partNode.GetChild(0)).Shape;
        capShape.Radius = scale * 0.25f;
        capShape.Height = scale * 1.0f;
        
        partNode.Position = new Vector3(part.RelativePosition.X, part.RelativePosition.Y, part.RelativePosition.Z);
    }

    private void UpdateMapBlockInGodot(MapBlock block)
    {
        string blockName = $"Block_{block.Position.X}_{block.Position.Z}";
        StaticBody3D blockNode = GetNodeOrNull<StaticBody3D>(blockName);
        
        if (blockNode == null)
        {
            blockNode = new StaticBody3D();
            blockNode.Name = blockName;
            AddChild(blockNode);
        }
        else
        {
            foreach (Node child in blockNode.GetChildren())
            {
                child.QueueFree();
            }
        }

        blockNode.Position = new Vector3(block.Position.X, block.Position.Y, block.Position.Z);
        
        var surfaceTool = new SurfaceTool();
        surfaceTool.Begin(Mesh.PrimitiveType.Triangles);
        
        bool[,] visited = new bool[16, 16];
        
        for (int z = 0; z < 16; z++)
        {
            for (int x = 0; x < 16; x++)
            {
                if (visited[x, z]) continue;
                
                int tileType = block.Tiles[z * 16 + x];
                if (tileType <= 0) continue;
                
                int endX = x;
                while (endX + 1 < 16 && !visited[endX + 1, z] && block.Tiles[z * 16 + (endX + 1)] == tileType)
                {
                    endX++;
                }
                
                int endZ = z;
                bool canExpandZ = true;
                while (canExpandZ && endZ + 1 < 16)
                {
                    for (int ix = x; ix <= endX; ix++)
                    {
                        if (visited[ix, endZ + 1] || block.Tiles[(endZ + 1) * 16 + ix] != tileType)
                        {
                            canExpandZ = false;
                            break;
                        }
                    }
                    if (canExpandZ) endZ++;
                }
                
                for (int iz = z; iz <= endZ; iz++)
                {
                    for (int ix = x; ix <= endX; ix++)
                    {
                        visited[ix, iz] = true;
                    }
                }
                
                AddCubeToSurfaceTool(surfaceTool, new Vector3(x, 0, z), new Vector3(endX + 1, 1, endZ + 1));
            }
        }
        
        surfaceTool.Index();
        var arrayMesh = surfaceTool.Commit();
        
        if (arrayMesh != null)
        {
            var meshInstance = new MeshInstance3D();
            meshInstance.Mesh = arrayMesh;
            blockNode.AddChild(meshInstance);
            
            var collisionShape = new CollisionShape3D();
            var concaveShape = arrayMesh.CreateTrimeshShape();
            collisionShape.Shape = concaveShape;
            blockNode.AddChild(collisionShape);
        }
    }
    
    private void AddCubeToSurfaceTool(SurfaceTool st, Vector3 min, Vector3 max)
    {
        // Top
        st.SetNormal(new Vector3(0, 1, 0));
        st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z));
        
        // Bottom
        st.SetNormal(new Vector3(0, -1, 0));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z)); st.AddVertex(new Vector3(min.X, min.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z));
        
        // Front
        st.SetNormal(new Vector3(0, 0, 1));
        st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z));
        
        // Back
        st.SetNormal(new Vector3(0, 0, -1));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, min.Z));
        
        // Left
        st.SetNormal(new Vector3(-1, 0, 0));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, min.Z));
        
        // Right
        st.SetNormal(new Vector3(1, 0, 0));
        st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z));
        st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
    }

    public override void _ExitTree()
    {
        _isRunning = false;
        _stream?.Close();
        _client?.Close();
    }
}
