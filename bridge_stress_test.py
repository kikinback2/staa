import socket
import struct
import time
import sys

# Assume the user compiles the proto:
# protoc -I=proto --python_out=. proto/dfvr_bridge.proto
try:
    import dfvr_bridge_pb2
except ImportError:
    print("Error: dfvr_bridge_pb2 not found. Please compile the protobuf first:")
    print("protoc -I=proto --python_out=. proto/dfvr_bridge.proto")
    sys.exit(1)

HOST = '127.0.0.1'
PORT = 9000

def create_handshake_request():
    req = dfvr_bridge_pb2.HandshakeRequest()
    req.client_version = "Stress Test 1.0"
    req.status = "Ready"
    return req.SerializeToString()

def connect_and_disconnect(count=100):
    print(f"--- Test 1: Rapid Connect/Disconnect ({count} times) ---")
    for i in range(count):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(1.0)
                s.connect((HOST, PORT))
                # Disconnect immediately
        except Exception as e:
            print(f"Failed at iteration {i}: {e}")
            return False
    print("Passed.")
    return True

def valid_handshake():
    print("--- Test 2: Valid Handshake ---")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(2.0)
            s.connect((HOST, PORT))
            
            payload = create_handshake_request()
            length_prefix = struct.pack('<I', len(payload))
            s.sendall(length_prefix + payload)
            
            # Read response length
            resp_len_data = s.recv(4)
            if len(resp_len_data) < 4:
                print("Failed to read response length.")
                return False
            resp_len = struct.unpack('<I', resp_len_data)[0]
            
            # Read response payload
            resp_data = s.recv(resp_len)
            resp = dfvr_bridge_pb2.HandshakeResponse()
            resp.ParseFromString(resp_data)
            print(f"Server Version: {resp.server_version}")
            print(f"Connection Status: {resp.connection_status}")
            print("Passed.")
            return True
    except Exception as e:
        print(f"Exception during handshake: {e}")
        return False

def fragmentation_test():
    print("--- Test 3: Fragmentation Test ---")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(2.0)
            s.connect((HOST, PORT))
            
            payload = create_handshake_request()
            length_prefix = struct.pack('<I', len(payload))
            
            # Send length prefix and HALF the payload
            half_len = len(payload) // 2
            s.sendall(length_prefix + payload[:half_len])
            
            print("Sent partial payload. Waiting 1 second...")
            time.sleep(1.0)
            
            # Send the rest
            s.sendall(payload[half_len:])
            
            # Read response length
            resp_len_data = s.recv(4)
            if len(resp_len_data) == 4:
                print("Successfully received response despite fragmentation.")
                print("Passed.")
                return True
            else:
                print("Failed to get response.")
                return False
    except Exception as e:
        print(f"Exception: {e}")
        return False

def malformed_packet_test():
    print("--- Test 4: Malformed Packet (Massive Allocation) ---")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(2.0)
            s.connect((HOST, PORT))
            
            # Send 2GB length prefix
            massive_length = 2 * 1024 * 1024 * 1024
            length_prefix = struct.pack('<I', massive_length)
            s.sendall(length_prefix)
            
            # Expect the server to drop the connection
            try:
                data = s.recv(4)
                if not data:
                    print("Server successfully dropped the connection on massive length.")
                    print("Passed.")
                    return True
                else:
                    print("Server sent data back? Failed.")
                    return False
            except socket.timeout:
                print("Server timed out / hung. Failed.")
                return False
            except ConnectionResetError:
                print("Connection reset by peer. Passed.")
                return True
    except Exception as e:
        print(f"Exception: {e}")
        return False

if __name__ == '__main__':
    print(f"Testing DFVR Bridge at {HOST}:{PORT}\n")
    connect_and_disconnect(100)
    print()
    valid_handshake()
    print()
    fragmentation_test()
    print()
    malformed_packet_test()
    print()
    print("Stress test complete.")
