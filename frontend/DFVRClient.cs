using Godot;
using System;
using System.Net;
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
    private bool _isConnected = false;

    [Export] public string ServerIp = "127.0.0.1";
    [Export] public int ServerPort = 9000;
    [Export] public int UdpDiscoveryPort = 9001;

    private UdpClient _udpClient;
    private bool _listeningForUdp = true;

    private GameStatePayload _latestPayload;
    private readonly object _payloadLock = new object();

    private Queue<ActionRequest> _pendingActions = new Queue<ActionRequest>();
    private readonly object _actionLock = new object();

    public override void _Ready()
    {
        // Start UDP broadcast listener for zero-config connection
        _ = Task.Run(UdpDiscoveryListenerAsync);

        // Attempt initial connection to configured ServerIp
        _ = ConnectAndHandshakeAsync(ServerIp, ServerPort);
    }

    private async Task UdpDiscoveryListenerAsync()
    {
        try
        {
            _udpClient = new UdpClient(UdpDiscoveryPort);
            _udpClient.EnableBroadcast = true;
            IPEndPoint remoteEndPoint = new IPEndPoint(IPAddress.Any, UdpDiscoveryPort);

            GD.Print($"Listening for UDP host discovery on port {UdpDiscoveryPort}...");

            while (_listeningForUdp)
            {
                var result = await _udpClient.ReceiveAsync();
                string message = System.Text.Encoding.UTF8.GetString(result.Buffer);
                
                if (message.StartsWith("DFVR_HOST:"))
                {
                    string hostIp = result.RemoteEndPoint.Address.ToString();
                    GD.Print($"Discovered DFVR Host via UDP at IP: {hostIp}");
                    
                    if (!_isConnected)
                    {
                        ServerIp = hostIp;
                        CallDeferred(nameof(ConnectToDiscoveredHost), hostIp, ServerPort);
                        break; // Stop listening once connected
                    }
                }
            }
        }
        catch (Exception e)
        {
            GD.PrintErr($"UDP Discovery error: {e.Message}");
        }
    }

    private void ConnectToDiscoveredHost(string ip, int port)
    {
        if (!_isConnected)
        {
            var networkUi = GetNodeOrNull<Node3D>("/root/Main/NetworkUI");
            if (networkUi != null)
            {
                // Update network UI status if method exists
                networkUi.Call("UpdateStatus", $"Discovered Host at {ip}! Connecting...");
            }
            _ = ConnectAndHandshakeAsync(ip, port);
        }
    }

    public void ConnectToHost(string ip, int port)
    {
        ServerIp = ip;
        ServerPort = port;
        _ = ConnectAndHandshakeAsync(ip, port);
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
        if (_isConnected) return;

        try
        {
            _client = new TcpClient();
            await _client.ConnectAsync(ip, port);
            _stream = _client.GetStream();
            
            GD.Print($"Connected to server at {ip}:{port}, initiating handshake...");

            var request = new HandshakeRequest
            {
                ClientVersion = "Meta Quest 3 VR Client 1.0",
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

            _isConnected = true;
            _isRunning = true;

            var networkUi = GetNodeOrNull<Node3D>("/root/Main/NetworkUI");
            if (networkUi != null)
            {
                networkUi.Call("UpdateStatus", $"Connected to {ip}:{port} (v{response.ServerVersion})");
            }

            _ = Task.Run(NetworkLoopAsync);
        }
        catch (Exception e)
        {
            GD.PrintErr($"Handshake failed: {e.Message}");
            var networkUi = GetNodeOrNull<Node3D>("/root/Main/NetworkUI");
            if (networkUi != null)
            {
                networkUi.Call("UpdateStatus", $"Connection Failed: {e.Message}");
            }
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
                _isConnected = false;
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
        if (payload.Type == PayloadType.FullState || payload.Type == PayloadType.DynamicOnly)
        {
            foreach (var entity in payload.Entities)
            {
                UpdateEntityInGodot(entity);
            }
        }
        
        if (payload.Type == PayloadType.FullState || payload.Type == PayloadType.MapOnly)
        {
            foreach (var block in payload.MapBlocks)
            {
                UpdateMapBlockInGodot(block);
            }
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
            
            var meshInstance = new MeshInstance3D();
            var mesh = new CapsuleMesh();
            meshInstance.Mesh = mesh;
            
            partNode.AddChild(collisionShape);
            partNode.AddChild(meshInstance);
            parent.AddChild(partNode);
            
            partNode.SetMeta("body_part_id", part.Id);
            partNode.SetMeta("entity_id", parent.Name.Replace("Unit_", ""));
        }
        
        float scale = (float)Math.Pow(part.SizeVolume / 5000.0, 1.0/3.0);
        var capShape = (CapsuleShape3D)((CollisionShape3D)partNode.GetChild(0)).Shape;
        capShape.Radius = scale * 0.25f;
        capShape.Height = scale * 1.0f;

        var capMesh = (CapsuleMesh)((MeshInstance3D)partNode.GetChild(1)).Mesh;
        capMesh.Radius = scale * 0.25f;
        capMesh.Height = scale * 1.0f;
        
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
        
        int[] tileData = new int[block.Tiles.Count];
        block.Tiles.CopyTo(tileData, 0);

        _ = Task.Run(() => GenerateMeshForBlockAsync(blockNode, tileData));
    }

    private void GenerateMeshForBlockAsync(StaticBody3D blockNode, int[] tiles)
    {
        var surfaceTool = new SurfaceTool();
        surfaceTool.Begin(Mesh.PrimitiveType.Triangles);
        
        bool[,] visited = new bool[16, 16];
        
        for (int z = 0; z < 16; z++)
        {
            for (int x = 0; x < 16; x++)
            {
                if (visited[x, z]) continue;
                
                int tileType = tiles[z * 16 + x];
                if (tileType <= 0) continue;
                
                int endX = x;
                while (endX + 1 < 16 && !visited[endX + 1, z] && tiles[z * 16 + (endX + 1)] == tileType)
                {
                    endX++;
                }
                
                int endZ = z;
                bool canExpandZ = true;
                while (canExpandZ && endZ + 1 < 16)
                {
                    for (int ix = x; ix <= endX; ix++)
                    {
                        if (visited[ix, endZ + 1] || tiles[(endZ + 1) * 16 + ix] != tileType)
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
            CallDeferred(nameof(ApplyMeshToBlock), blockNode, arrayMesh);
        }
    }

    private void ApplyMeshToBlock(StaticBody3D blockNode, ArrayMesh arrayMesh)
    {
        if (!IsInstanceValid(blockNode)) return;

        var meshInstance = new MeshInstance3D();
        meshInstance.Mesh = arrayMesh;
        blockNode.AddChild(meshInstance);
        
        var collisionShape = new CollisionShape3D();
        var concaveShape = arrayMesh.CreateTrimeshShape();
        collisionShape.Shape = concaveShape;
        blockNode.AddChild(collisionShape);
    }
    
    private void AddCubeToSurfaceTool(SurfaceTool st, Vector3 min, Vector3 max)
    {
        st.SetNormal(new Vector3(0, 1, 0));
        st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z));
        
        st.SetNormal(new Vector3(0, -1, 0));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z)); st.AddVertex(new Vector3(min.X, min.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z));
        
        st.SetNormal(new Vector3(0, 0, 1));
        st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z));
        
        st.SetNormal(new Vector3(0, 0, -1));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, min.Y, min.Z));
        
        st.SetNormal(new Vector3(-1, 0, 0));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, min.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z));
        st.AddVertex(new Vector3(min.X, min.Y, min.Z)); st.AddVertex(new Vector3(min.X, max.Y, max.Z)); st.AddVertex(new Vector3(min.X, max.Y, min.Z));
        
        st.SetNormal(new Vector3(1, 0, 0));
        st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z)); st.AddVertex(new Vector3(max.X, min.Y, max.Z));
        st.AddVertex(new Vector3(max.X, min.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, min.Z)); st.AddVertex(new Vector3(max.X, max.Y, max.Z));
    }

    public override void _ExitTree()
    {
        _isRunning = false;
        _listeningForUdp = false;
        _udpClient?.Close();
        _stream?.Close();
        _client?.Close();
    }
}
