using Godot;
using System;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Google.Protobuf;
using Dfvr; // Assuming this is the generated namespace

public partial class DFVRClient : Node
{
    private TcpClient _client;
    private NetworkStream _stream;
    private Thread _networkThread;
    private bool _isRunning = false;

    // A thread-safe queue or single payload buffer
    private GameStatePayload _latestPayload;
    private readonly object _payloadLock = new object();

    public override void _Ready()
    {
        _ = ConnectAndHandshakeAsync("127.0.0.1", 9000);
    }

    private async Task ConnectAndHandshakeAsync(string ip, int port)
    {
        try
        {
            _client = new TcpClient();
            await _client.ConnectAsync(ip, port);
            _stream = _client.GetStream();
            
            GD.Print("Connected to server, initiating handshake...");

            // 1. Prepare HandshakeRequest
            var request = new HandshakeRequest
            {
                ClientVersion = "Godot VR Client 0.1a",
                Status = "Ready"
            };

            // 2. Send length-prefixed HandshakeRequest
            byte[] requestBytes = request.ToByteArray();
            byte[] lengthPrefix = BitConverter.GetBytes(requestBytes.Length);
            
            // BitConverter uses system endianness. We assume Little Endian on Windows to match C++.
            await _stream.WriteAsync(lengthPrefix, 0, lengthPrefix.Length);
            await _stream.WriteAsync(requestBytes, 0, requestBytes.Length);
            
            GD.Print("HandshakeRequest sent. Awaiting response...");

            // 3. Receive length-prefixed HandshakeResponse
            byte[] responseLengthBytes = new byte[4];
            int bytesRead = await _stream.ReadAsync(responseLengthBytes, 0, 4);
            if (bytesRead < 4)
            {
                GD.PrintErr("Failed to read handshake response length.");
                return;
            }

            int responseLength = BitConverter.ToInt32(responseLengthBytes, 0);
            byte[] responseBytes = new byte[responseLength];
            
            int totalRead = 0;
            while (totalRead < responseLength)
            {
                int read = await _stream.ReadAsync(responseBytes, totalRead, responseLength - totalRead);
                if (read == 0) throw new Exception("Connection closed while reading response.");
                totalRead += read;
            }

            // 4. Deserialize HandshakeResponse
            var response = HandshakeResponse.Parser.ParseFrom(responseBytes);
            GD.Print($"Handshake Successful! Server Version: {response.ServerVersion}, Time: {response.WorldTime}");

            // 5. Start main network loop for GameStatePayloads
            _isRunning = true;
            _networkThread = new Thread(NetworkLoop);
            _networkThread.Start();
        }
        catch (Exception e)
        {
            GD.PrintErr($"Handshake failed: {e.Message}");
        }
    }

    private void NetworkLoop()
    {
        while (_isRunning)
        {
            try
            {
                // 1. Read 4-byte length prefix
                byte[] lengthBytes = new byte[4];
                int read = _stream.Read(lengthBytes, 0, 4);
                if (read == 0) break; // Connection closed
                if (read < 4) continue; // Partial read, should handle better in production

                int payloadLength = BitConverter.ToInt32(lengthBytes, 0);
                if (payloadLength <= 0) continue;

                // 2. Read full payload
                byte[] payloadBytes = new byte[payloadLength];
                int totalRead = 0;
                while (totalRead < payloadLength)
                {
                    int r = _stream.Read(payloadBytes, totalRead, payloadLength - totalRead);
                    if (r == 0) break;
                    totalRead += r;
                }

                // 3. Deserialize GameStatePayload
                var payload = GameStatePayload.Parser.ParseFrom(payloadBytes);
                
                if (payload != null)
                {
                    lock (_payloadLock)
                    {
                        _latestPayload = payload;
                    }
                }
            }
            catch (Exception e)
            {
                GD.PrintErr($"Network Error: {e.Message}");
                _isRunning = false;
            }

            // No Sleep here, let Read block
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
                _latestPayload = null; // Clear so we don't process it twice
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
            unitNode = new Node3D(); // Container for body parts
            unitNode.Name = nodeName;
            AddChild(unitNode);
            GD.Print($"Spawned unit: {entity.Name}");
        }

        // Update Global Position
        unitNode.Position = new Vector3(entity.Position.X, entity.Position.Y, entity.Position.Z);

        // Update Anatomical Hitboxes
        foreach (var part in entity.BodyParts)
        {
            UpdateBodyPartHitbox(unitNode, part);
        }
    }

    private void UpdateBodyPartHitbox(Node3D parent, BodyPart part)
    {
        string partName = $"Part_{part.Id}";
        MeshInstance3D partNode = parent.GetNodeOrNull<MeshInstance3D>(partName);
        
        if (partNode == null)
        {
            partNode = new MeshInstance3D();
            partNode.Name = partName;
            
            // Use a sphere as a generic hitbox proxy
            var sphere = new SphereMesh();
            partNode.Mesh = sphere;
            parent.AddChild(partNode);
        }
        
        // Simple volumetric scaling (cube root of relsize)
        float scale = (float)Math.Pow(part.SizeVolume / 5000.0, 1.0/3.0);
        partNode.Scale = new Vector3(scale, scale, scale);
        
        // Relative position within the unit
        partNode.Position = new Vector3(part.RelativePosition.X, part.RelativePosition.Y, part.RelativePosition.Z);
    }

    private void UpdateMapBlockInGodot(MapBlock block)
    {
        // Use MultiMesh for efficient rendering of 16x16 blocks
        string blockName = $"Block_{block.Position.X}_{block.Position.Z}";
        MultiMeshInstance3D blockNode = GetNodeOrNull<MultiMeshInstance3D>(blockName);
        
        if (blockNode == null)
        {
            blockNode = new MultiMeshInstance3D();
            blockNode.Name = blockName;
            
            var multiMesh = new MultiMesh();
            multiMesh.TransformFormat = MultiMesh.TransformFormatEnum.Transform3D;
            multiMesh.Mesh = new BoxMesh(); // Generic 1x1x1 cube
            multiMesh.InstanceCount = 256; 
            
            blockNode.Multimesh = multiMesh;
            AddChild(blockNode);
        }
        
        blockNode.Position = new Vector3(block.Position.X, block.Position.Y, block.Position.Z);
        
        for (int i = 0; i < block.Tiles.Count; i++)
        {
            int x = i % 16;
            int z = i / 16;
            int tileType = block.Tiles[i];
            
            // Basic logic: if tileType is non-zero, it's solid
            Transform3D transform = new Transform3D(Basis.Identity, new Vector3(x, 0, z));
            
            if (tileType <= 0) 
            {
                // Hide empty tiles by scaling to zero
                transform.Basis = Basis.Identity.Scaled(Vector3.Zero);
            }
            
            blockNode.Multimesh.SetInstanceTransform(i, transform);
        }
    }

    public override void _ExitTree()
    {
        _isRunning = false;
        _stream?.Close();
        _client?.Close();
        if (_networkThread != null && _networkThread.IsAlive)
        {
            _networkThread.Join();
        }
    }
}
