#include "modules/Units.h"
#include "modules/Maps.h"
#include "df/world.h"
#include "df/unit.h"
#include "df/unit_body_part_status.h"
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
void ScrapeMenuOptions(dfvr::GameStatePayload& payload) {
    auto screen = Gui::getCurViewscreen(true);
    if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
        // Mock example - in a real build we'd iterate over adv_menu->menu_options
        dfvr::MenuOption* opt = payload.add_current_menu_options();
        opt->set_option_id(1);
        opt->set_text_content("Talk");
    }
}

// Scrapes units and their body parts
void ScrapeEntities(dfvr::GameStatePayload& payload) {
    for (auto unit : world->units.active) {
        if (!Units::isAlive(unit) || !unit->flags1.bits.active) continue;

        auto entity = payload.add_entities();
        entity->set_entity_id(unit->id);
        
        std::string name = Translation::TranslateName(&unit->name, false);
        if (name.empty()) name = "Unknown Creature";
        entity->set_name(name);

        auto pos = entity->mutable_position();
        pos->set_x(static_cast<float>(unit->pos.x));
        pos->set_y(static_cast<float>(unit->pos.z)); 
        pos->set_z(static_cast<float>(unit->pos.y));

        // Traverse the actual body plan for hitboxing
        if (unit->body.body_plan) {
            const auto& body_parts = unit->body.body_plan->body_parts;
            for (int i = 0; i < (int)body_parts.size(); ++i) {
                auto part_raw = body_parts[i];
                
                // Skip internal parts for basic hitboxing
                if (part_raw->flags.is_set(df::body_part_raw_flags::INTERNAL)) continue;

                dfvr::BodyPart* part = entity->add_body_parts();
                part->set_id(i);
                part->set_name(part_raw->name_singular[0]->value);
                
                // Mock relative position for now (Skeletal data isn't in DF)
                // In Godot, we'll arrange these into a humanoid or creature rig.
                part->mutable_relative_position()->set_y(0.0f); 
                
                // Use relsize for collider scaling
                part->set_size_volume(static_cast<float>(part_raw->relsize));
            }
        }
    }
}

// Scrapes map tiles around the player
void ScrapeMapBlocks(dfvr::GameStatePayload& payload) {
    df::unit* player = world->units.active.size() > 0 ? world->units.active[0] : nullptr;
    if (!player) return;

    int player_x = player->pos.x;
    int player_y = player->pos.y;
    int player_z = player->pos.z;

    // Send the current 16x16 block the player is in
    int bx = (player_x / 16) * 16;
    int by = (player_y / 16) * 16;

    auto block = payload.add_map_blocks();
    block->mutable_position()->set_x((float)bx);
    block->mutable_position()->set_y((float)player_z);
    block->mutable_position()->set_z((float)by);

    for (int ty = 0; ty < 16; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
            df::coord pos(bx + tx, by + ty, player_z);
            if (Maps::isValidTilePos(pos)) {
                auto tile_type = Maps::getTileType(pos);
                block->add_tiles(static_cast<int32_t>(tile_type));
            } else {
                block->add_tiles(0); // Void
            }
        }
    }
}

// DFHack Update Hook (Main Thread)
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Clear and re-populate the shared state
    shared_game_state.Clear();
    shared_game_state.set_frame_tick(world->frame_counter);
    
    ScrapeMenuOptions(shared_game_state);
    ScrapeEntities(shared_game_state);
    ScrapeMapBlocks(shared_game_state);
    
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
    // 1. Read 4-byte length prefix
    uint32_t length_prefix = 0;
    if (!ReadExact(client_socket, reinterpret_cast<char*>(&length_prefix), sizeof(length_prefix))) {
        closesocket(client_socket);
        return;
    }
    
    // 2. Read and deserialize HandshakeRequest
    std::vector<char> request_buffer(length_prefix);
    if (!ReadExact(client_socket, request_buffer.data(), length_prefix)) {
        closesocket(client_socket);
        return;
    }
    
    dfvr::HandshakeRequest request;
    if (request.ParseFromArray(request_buffer.data(), length_prefix)) {
        std::cout << "Received Handshake from Client Version: " << request.client_version() << std::endl;
        
        // 3. Send HandshakeResponse
        dfvr::HandshakeResponse response;
        response.set_server_version("DFVR Bridge 0.1a");
        response.set_connection_status("OK");
        response.set_world_time(123456789);
        response.set_player_id(1);
        
        std::string serialized_response;
        response.SerializeToString(&serialized_response);
        uint32_t response_length = static_cast<uint32_t>(serialized_response.size());
        
        if (WriteExact(client_socket, reinterpret_cast<const char*>(&response_length), sizeof(response_length))) {
            WriteExact(client_socket, serialized_response.data(), response_length);
        } else {
            closesocket(client_socket);
            return;
        }

        // 4. ENTER STREAMING MODE
        std::cout << "Client handshaked. Entering streaming mode..." << std::endl;
        while (is_running) {
            std::string payload_data;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                shared_game_state.SerializeToString(&payload_data);
            }

            uint32_t payload_length = static_cast<uint32_t>(payload_data.size());
            if (!WriteExact(client_socket, reinterpret_cast<const char*>(&payload_length), sizeof(payload_length))) break;
            if (!WriteExact(client_socket, payload_data.data(), payload_length)) break;

            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20 Hz
        }
    }
    
    std::cout << "Client disconnected." << std::endl;
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
