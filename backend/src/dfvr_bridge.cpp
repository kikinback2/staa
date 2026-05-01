#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/Gui.h"
#include "modules/Translation.h"
#include "df/viewscreen_adventure_menust.h"
#include "dfvr_bridge.pb.h"

#include <thread>
#include <mutex>
#include <iostream>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("dfvr_bridge");

static std::thread server_thread;
static std::mutex state_mutex;
static bool is_running = false;
static SOCKET server_socket = INVALID_SOCKET;

// Shared state buffer populated by the main thread
static dfvr::GameStatePayload shared_game_state;

// Scrapes the current active menu options in Adventure Mode
// MUST BE CALLED FROM THE MAIN THREAD (e.g. plugin_onupdate)
void ScrapeMenuOptions(dfvr::GameStatePayload& payload) {
    auto screen = Gui::getCurViewscreen(true);
    
    // Check if we are in an adventure mode screen (simplified example)
    if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
        // In a real implementation, we would parse adv_menu->menu_options
        // Here we provide a mock example of populating the Protobuf repeated field.
        dfvr::MenuOption* opt = payload.add_current_menu_options();
        opt->set_option_id(1);
        opt->set_text_content("Ask about surrounding area.");
    }
}

// DFHack Update Hook (Main Thread)
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Clear and re-populate the shared state
    shared_game_state.Clear();
    ScrapeMenuOptions(shared_game_state);
    
    // TODO: Populate Entities and BodyParts here
    
    return CR_OK;
}

// Helper functions for length-prefixed framing
bool ReadExact(SOCKET sock, char* buffer, size_t length) {
    size_t total_read = 0;
    while (total_read < length && is_running) {
        int bytes_read = recv(sock, buffer + total_read, length - total_read, 0);
        if (bytes_read <= 0) return false; // Connection closed or error
        total_read += bytes_read;
    }
    return true;
}

bool WriteExact(SOCKET sock, const char* buffer, size_t length) {
    size_t total_written = 0;
    while (total_written < length && is_running) {
        int bytes_written = send(sock, buffer + total_written, length - total_written, 0);
        if (bytes_written <= 0) return false;
        total_written += bytes_written;
    }
    return true;
}

void HandleClient(SOCKET client_socket) {
    // 1. Read 4-byte length prefix (Little Endian for simplicity, or Network Byte Order)
    uint32_t length_prefix = 0;
    if (!ReadExact(client_socket, reinterpret_cast<char*>(&length_prefix), sizeof(length_prefix))) {
        closesocket(client_socket);
        return;
    }
    
    // Convert from network byte order to host byte order if necessary, but we'll assume little-endian 
    // for this example to match C#'s default BinaryWriter/Reader behavior.
    
    // 2. Read the incoming HandshakeRequest
    std::vector<char> request_buffer(length_prefix);
    if (!ReadExact(client_socket, request_buffer.data(), length_prefix)) {
        closesocket(client_socket);
        return;
    }
    
    // 3. Deserialize HandshakeRequest
    dfvr::HandshakeRequest request;
    if (request.ParseFromArray(request_buffer.data(), length_prefix)) {
        std::cout << "Received Handshake from Client Version: " << request.client_version() << std::endl;
        
        // 4. Prepare HandshakeResponse
        dfvr::HandshakeResponse response;
        response.set_server_version("DFVR Bridge 0.1a");
        response.set_connection_status("OK");
        response.set_world_time(123456789); // Mock world time
        response.set_player_id(1); // Mock player ID
        
        std::string serialized_response;
        response.SerializeToString(&serialized_response);
        
        // 5. Send length-prefixed HandshakeResponse
        uint32_t response_length = static_cast<uint32_t>(serialized_response.size());
        if (WriteExact(client_socket, reinterpret_cast<const char*>(&response_length), sizeof(response_length))) {
            WriteExact(client_socket, serialized_response.data(), response_length);
        }
    }
    
    closesocket(client_socket);
}

// Background thread function serving clients
void ServerLoop() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) return;

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(9000);

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(server_socket);
        return;
    }

    listen(server_socket, SOMAXCONN);

    // Accept loop
    while (is_running) {
        // Use select to make accept non-blocking so we can exit cleanly
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int result = select(server_socket + 1, &readfds, NULL, NULL, &tv);
        if (result > 0 && FD_ISSET(server_socket, &readfds)) {
            SOCKET client_socket = accept(server_socket, NULL, NULL);
            if (client_socket != INVALID_SOCKET) {
                // In a real application, we might spawn a new thread per client or use async I/O.
                // For this handshake milestone, handle the client directly (or could be in a detached thread).
                HandleClient(client_socket);
            }
        }
    }

    closesocket(server_socket);
#ifdef _WIN32
    WSACleanup();
#endif
}

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    is_running = true;
    server_thread = std::thread(ServerLoop);
    out.print("DFVR Bridge Server started on port 9000.\n");
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    is_running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    return CR_OK;
}
