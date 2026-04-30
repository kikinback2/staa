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
        ConnectToServer("127.0.0.1", 9000);
    }

    private void ConnectToServer(string ip, int port)
    {
        try
        {
            _client = new TcpClient(ip, port);
            _stream = _client.GetStream();
            _isRunning = true;

            _networkThread = new Thread(NetworkLoop);
            _networkThread.Start();
            
            GD.Print("Connected to DFVR Bridge.");
        }
        catch (Exception e)
        {
            GD.PrintErr($"Failed to connect: {e.Message}");
        }
    }

    private void NetworkLoop()
    {
        // Simple loop to read from the stream
        // In a robust implementation, use proper message framing (e.g., length-prefixed)
        while (_isRunning)
        {
            try
            {
                if (_stream.DataAvailable)
                {
                    // Read data and parse Protobuf
                    // Note: Google.Protobuf can parse length-prefixed streams: GameStatePayload.Parser.ParseDelimitedFrom(_stream)
                    // For this example, we'll assume ParseFrom works on the raw stream if it's the only thing sent
                    
                    var payload = GameStatePayload.Parser.ParseDelimitedFrom(_stream);
                    
                    if (payload != null)
                    {
                        lock (_payloadLock)
                        {
                            _latestPayload = payload;
                        }
                    }
                }
            }
            catch (Exception e)
            {
                GD.PrintErr($"Network Error: {e.Message}");
                _isRunning = false;
            }

            Thread.Sleep(50); // 20 TPS
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
        // Example: Iterate entities and update their Godot nodes
        foreach (var entity in payload.Entities)
        {
            // Find or instantiate entity node
            // e.g., UpdateEntity(entity);
            
            // GD.Print($"Processing Entity: {entity.Name}");
        }
        
        // Output logs to LLM service or UI
        if (!string.IsNullOrEmpty(payload.LatestLogText))
        {
            // GD.Print($"DF Log: {payload.LatestLogText}");
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
