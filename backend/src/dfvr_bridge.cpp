#include "modules/Units.h"
#include "modules/Maps.h"
#include "df/world.h"
#include "df/unit.h"
#include "df/unit_body_part_status.h"
#include "df/viewscreen_adventure_menust.h"
#include "dfvr_bridge.pb.h"
#include "Core.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <chrono>

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
static std::condition_variable state_cv;
static bool state_updated = false;
static bool is_running = false;
static SOCKET server_socket = INVALID_SOCKET;

// Thread-safe action queue
static std::vector<dfvr::ActionRequest> pending_actions;
static std::mutex actions_mutex;

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

// Map Block Delta Tracking
struct BlockCoord {
    int x, y, z;
    bool operator==(const BlockCoord& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const BlockCoord& o) const { return !(*this == o); }
};
static BlockCoord last_player_block = {-999, -999, -999};

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
                
                // Heuristic mapping for relative position based on standard humanoid names
                float y_offset = 0.0f;
                float x_offset = 0.0f;
                std::string part_name = part_raw->name_singular[0]->value;
                if (part_name.find("head") != std::string::npos || part_name.find("skull") != std::string::npos) {
                    y_offset = 0.6f;
                } else if (part_name.find("foot") != std::string::npos || part_name.find("toe") != std::string::npos) {
                    y_offset = -0.8f;
                } else if (part_name.find("leg") != std::string::npos) {
                    y_offset = -0.4f;
                    x_offset = (part_name.find("right") != std::string::npos) ? 0.2f : -0.2f;
                } else if (part_name.find("arm") != std::string::npos || part_name.find("hand") != std::string::npos) {
                    y_offset = 0.3f;
                    x_offset = (part_name.find("right") != std::string::npos) ? 0.4f : -0.4f;
                }

                part->mutable_relative_position()->set_x(x_offset);
                part->mutable_relative_position()->set_y(y_offset); 
                part->mutable_relative_position()->set_z(0.0f);
                
                // Use relsize for collider scaling
                part->set_size_volume(static_cast<float>(part_raw->relsize));
            }
        }
    }
}

// Scrapes map tiles around the player
// Scrapes map tiles around the player in a 3x3 block grid
bool ScrapeMapBlocks(dfvr::GameStatePayload& payload, bool force_update) {
    df::unit* player = world->units.active.size() > 0 ? world->units.active[0] : nullptr;
    if (!player) return false;

    int player_x = player->pos.x;
    int player_y = player->pos.y;
    int player_z = player->pos.z;

    int bx = (player_x / 16) * 16;
    int by = (player_y / 16) * 16;

    BlockCoord current_block = {bx, by, player_z};
    if (!force_update && current_block == last_player_block) {
        return false; // Map hasn't changed block boundaries
    }
    last_player_block = current_block;

    // Send a 3x3 grid of blocks
    for (int obx = -1; obx <= 1; ++obx) {
        for (int oby = -1; oby <= 1; ++oby) {
            int target_bx = bx + (obx * 16);
            int target_by = by + (oby * 16);

            auto block = payload.add_map_blocks();
            block->mutable_position()->set_x((float)target_bx);
            block->mutable_position()->set_y((float)player_z);
            block->mutable_position()->set_z((float)target_by);

            for (int ty = 0; ty < 16; ++ty) {
                for (int tx = 0; tx < 16; ++tx) {
                    df::coord pos(target_bx + tx, target_by + ty, player_z);
                    if (Maps::isValidTilePos(pos)) {
                        auto tile_type = Maps::getTileType(pos);
                        block->add_tiles(static_cast<int32_t>(tile_type));
                    } else {
                        block->add_tiles(0); // Void
                    }
                }
            }
        }
    }
    return true;
}

// DFHack Update Hook (Main Thread)
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    // Process Actions
    {
        std::lock_guard<std::mutex> lock(actions_mutex);
        for (const auto& action : pending_actions) {
            auto screen = Gui::getCurViewscreen(true);
            if (auto adv_menu = virtual_cast<df::viewscreen_adventure_menust>(screen)) {
                out.print("DFVR: Received ActionRequest to inject option %d\n", action.selected_option_id());
                // Example: In a fully mapped system, we would find the matching option and simulate a keystroke
                // adv_menu->feed_key(interface_key::SELECT); 
            } else {
                out.print("DFVR: Received ActionRequest %d, but not in adventure menu.\n", action.selected_option_id());
            }
        }
        pending_actions.clear();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        
        // Populate Map State
        dfvr::GameStatePayload map_payload;
        bool map_updated = ScrapeMapBlocks(map_payload, false);
        if (map_updated) {
            map_payload.set_type(dfvr::PayloadType::MAP_ONLY);
            map_payload.set_frame_tick(world->frame_counter);
            shared_game_state.CopyFrom(map_payload); // Simplified for now, in a robust system we'd queue multiple payloads
        } else {
            // Populate Dynamic State
            shared_game_state.Clear();
            shared_game_state.set_type(dfvr::PayloadType::DYNAMIC_ONLY);
            shared_game_state.set_frame_tick(world->frame_counter);
            
            ScrapeMenuOptions(shared_game_state);
            ScrapeEntities(shared_game_state);
        }
        
        state_updated = true;
    }
    state_cv.notify_all();
    
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
    // Set timeouts to prevent hanging on ReadExact/WriteExact if client crashes
#ifdef _WIN32
    DWORD timeout = 1000; // 1 second
#else
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
#endif
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // 1. Read 4-byte length prefix
    uint32_t length_prefix = 0;
    if (!ReadExact(client_socket, reinterpret_cast<char*>(&length_prefix), sizeof(length_prefix))) {
        closesocket(client_socket);
        return;
    }
    
    // Prevent massive allocation from malformed packets (10MB limit)
    if (length_prefix == 0 || length_prefix > 10 * 1024 * 1024) {
        std::cerr << "Invalid HandshakeRequest length: " << length_prefix << std::endl;
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
        std::string df_version = "Unknown";
        if (Core::getInstance().vinfo) {
            df_version = Core::getInstance().vinfo->getVersion();
        }
        response.set_server_version("DFHack " + df_version + " - DFVR Bridge 0.1a");
        response.set_connection_status("OK");
        response.set_world_time(world->frame_counter);
        
        int player_id = -1;
        if (!world->units.active.empty()) {
            player_id = world->units.active[0]->id;
        }
        response.set_player_id(player_id);
        
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
            // Check for incoming ActionRequests
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10ms

            int result = select(client_socket + 1, &readfds, NULL, NULL, &tv);
            if (result > 0 && FD_ISSET(client_socket, &readfds)) {
                uint32_t req_length = 0;
                if (ReadExact(client_socket, reinterpret_cast<char*>(&req_length), sizeof(req_length))) {
                    if (req_length > 0 && req_length <= 1024 * 1024) { // 1MB sanity check
                        std::vector<char> req_buf(req_length);
                        if (ReadExact(client_socket, req_buf.data(), req_length)) {
                            dfvr::ActionRequest action;
                            if (action.ParseFromArray(req_buf.data(), req_length)) {
                                std::lock_guard<std::mutex> lock(actions_mutex);
                                pending_actions.push_back(action);
                            }
                        } else break;
                    }
                } else {
                    break; // Client disconnected
                }
            } else if (result < 0) {
                break; // Socket error
            }

            // Check if game state has updated
            std::string payload_data;
            bool send_payload = false;
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                if (state_cv.wait_for(lock, std::chrono::milliseconds(10), []{ return state_updated || !is_running; })) {
                    if (!is_running) break;
                    shared_game_state.SerializeToString(&payload_data);
                    state_updated = false;
                    send_payload = true;
                }
            }

            if (send_payload) {
                uint32_t payload_length = static_cast<uint32_t>(payload_data.size());
                if (!WriteExact(client_socket, reinterpret_cast<const char*>(&payload_length), sizeof(payload_length))) break;
                if (!WriteExact(client_socket, payload_data.data(), payload_length)) break;
            }
        }
    }
    
    std::cout << "Client disconnected." << std::endl;
    closesocket(client_socket);
}

int GetConfigPort() {
    int port = 9000;
    std::ifstream file("dfhack-config/dfvr_bridge.ini");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("port=") == 0) {
                try {
                    port = std::stoi(line.substr(5));
                } catch (...) {}
            }
        }
    }
    return port;
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
    server_addr.sin_port = htons(GetConfigPort());

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
    int port = GetConfigPort();
    out.print("DFVR Bridge Server started on port %d.\n", port);
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    is_running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    return CR_OK;
}
