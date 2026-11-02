#include "listener.h"
#include "snapshot_parser.h"
#include "md_packet_validator.h"
#include "multicast_socket.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <queue>
#include <unordered_map>

struct BufferedMessage {
    char   data[1500];
    size_t length;
};

void run_listener(SymbolManager& sm, const Config::MarketData& md, std::atomic<bool>& global_shutdown) {
    // order_id → symbol_id routing for delete/modify/trade messages
    std::unordered_map<uint64_t, uint32_t> order_to_symbol;

    std::queue<BufferedMessage> live_buffer;
    std::vector<Snapshot> snapshots;

    std::string live_addr   = md.live_addr;
    const int   live_port   = md.port;
    std::string replay_addr = md.replay_addr;
    const int   replay_port = md.port;
    std::string local_ip    = md.local_iface;

    int live_sock   = create_multicast_socket(live_addr.c_str(),   live_port,   local_ip.c_str());
    int replay_sock = create_multicast_socket(replay_addr.c_str(), replay_port, local_ip.c_str());

    enum Feed { REPLAY, LIVE, FEED_COUNT };
    constexpr const char* FEED_NAMES[FEED_COUNT] = { "replay", "live" };

    pollfd fds[FEED_COUNT];

    fds[REPLAY] = pollfd{replay_sock, POLLIN, 0};
    fds[LIVE] = pollfd{live_sock, POLLIN, 0};

    char     buf[1500];
    bool     caught_up        = false;
    bool     received_snapshot = false;
    uint32_t messages_processed = 0;
    int      no_replay_count  = 0;

    // ── Inline helpers ────────────────────────────────────────────────────────

    // Route a new_order into SymbolManager and record order→symbol mapping
    auto dispatch_new_order = [&](const new_order* msg) {
        sm.on_new_order(msg->symbol, msg);
        order_to_symbol[msg->order_id] = msg->symbol;
    };

    auto dispatch_delete_order = [&](delete_order* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_delete_order(it->second, msg);
        order_to_symbol.erase(it);
    };

    auto dispatch_modify_order = [&](modify_order* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_modify_order(it->second, msg);
    };

    auto dispatch_trade = [&](trade* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_trade(it->second, msg);
    };

    std::cout << "[Listener] Starting market data feed...\n";

    while (true) {
        int timeout = caught_up ? -1 : 10;
        int ready = poll(fds, FEED_COUNT, timeout);

        if (ready < 0) {
            std::cerr << "[Listener] poll failed: " << strerror(errno) << "\n";
            global_shutdown = true;
            return;
        }

        // ── Catch-up: switch to live feed once replay goes quiet ─────────────
        if (ready == 0 && !caught_up && received_snapshot) {
            if (++no_replay_count > 100) {
                std::cout << "[Listener] Caught up — draining "
                          << live_buffer.size() << " buffered messages\n";
                caught_up = true;

                while (!live_buffer.empty()) {
                    BufferedMessage& bm = live_buffer.front();
                    memcpy(buf, bm.data, bm.length);

                    md_header* hdr = reinterpret_cast<md_header*>(buf);
                    MdPacketResult r = validate_packet(buf, bm.length);

                    if (r == MdPacketResult::OK) {
                        switch (hdr->msg_type) {
                            case MSG_TYPE::NEW_ORDER:
                                dispatch_new_order(reinterpret_cast<new_order*>(buf));
                                break;
                            case MSG_TYPE::DELETE_ORDER:
                                dispatch_delete_order(reinterpret_cast<delete_order*>(buf));
                                break;
                            case MSG_TYPE::MODIFY_ORDER:
                                dispatch_modify_order(reinterpret_cast<modify_order*>(buf));
                                break;
                            case MSG_TYPE::TRADE:
                                dispatch_trade(reinterpret_cast<trade*>(buf));
                                break;
                            default:
                                break;
                        }
                    }
                    else {
                        std::cerr << "[Listener] Dropped buffered live packet ("
                                  << bm.length << " bytes): " << to_string(r) << "\n";
                    }
                    live_buffer.pop();
                }

                std::cout << "[Listener] Now on live feed.\n";
            }
            continue;
        }

        // ── Process incoming packets ──────────────────────────────────────────
        for (int i = 0; i < FEED_COUNT; ++i) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                global_shutdown = true;
                std::cerr << "[Listener] FATAL: " << FEED_NAMES[i] 
                          << " feed socket error (revents=0x" << std::hex << fds[i].revents
                          << std::dec << ")\n";
                return;
            }

            if (!(fds[i].revents & POLLIN)) continue;

            ssize_t bytes = read(fds[i].fd, buf, sizeof(buf));
            if (bytes <= 0) continue;

            // Buffer live messages until we've caught up with replay
            if (!caught_up && i == LIVE) {
                if (live_buffer.size() > 100000) {
                    std::cerr << "[Listener] FATAL: live buffer overflow\n";
                    global_shutdown = true;
                    return;
                }
                BufferedMessage bm;
                memcpy(bm.data, buf, bytes);
                bm.length = bytes;
                live_buffer.push(bm);
                continue;
            }

            if (!has_md_header(static_cast<size_t>(bytes))) {
                std::cerr << "[Listener] Dropped " << FEED_NAMES[i] << " packet: "
                          << bytes << " bytes, shorter than md_header ("
                          << sizeof(md_header) << ")\n";
                continue;
            }
            
            md_header* hdr = reinterpret_cast<md_header*>(buf);

            // ── Snapshot ──────────────────────────────────────────────────────
            if (hdr->magic_number == SNAPSHOT_MAGIC_NUMBER) {
                SnapshotParseResult result = parse_snapshot(buf, static_cast<size_t>(bytes), snapshots);
                
                no_replay_count = 0;

                if (result != SnapshotParseResult::OK) {
                    // Book untouched, received_snapshot unchanged: wait for the next one.
                    std::cerr << "[Listener] Dropped malformed snapshot packet ("
                              << bytes << " bytes): " << to_string(result) << "\n";
                    continue;
                }

                for (const auto& snapshot: snapshots) {
                    sm.reset_book(snapshot.symbol);

                    // Remove stale order→symbol mappings for this symbol
                    for (auto it = order_to_symbol.begin(); it != order_to_symbol.end(); ) {
                        if (it->second == snapshot.symbol) it = order_to_symbol.erase(it);
                        else ++it;
                    }

                    std::cout << "[Listener] Snapshot: symbol=" << snapshot.symbol
                              << " seq=" << snapshot.last_md_seq_num
                              << " orders=" << snapshot.orders.size() << "\n";

                    for (const auto& order: snapshot.orders) {
                        dispatch_new_order(&order);
                    }

                }

                received_snapshot = true;
                continue;
            }

            // ── Normal market data ────────────────────────────────────────────
            MdPacketResult r = validate_packet(buf, static_cast<size_t>(bytes));
            if (r != MdPacketResult::OK) {
                std::cerr << "[Listener] Dropped " << FEED_NAMES[i] << " packet ("
                          << bytes << " bytes): " << to_string(r) << "\n";
                continue;
            }

            switch (hdr->msg_type) {
                case MSG_TYPE::NEW_ORDER:
                    dispatch_new_order(reinterpret_cast<new_order*>(buf));
                    break;
                case MSG_TYPE::DELETE_ORDER:
                    dispatch_delete_order(reinterpret_cast<delete_order*>(buf));
                    break;
                case MSG_TYPE::MODIFY_ORDER:
                    dispatch_modify_order(reinterpret_cast<modify_order*>(buf));
                    break;
                case MSG_TYPE::TRADE:
                    dispatch_trade(reinterpret_cast<trade*>(buf));
                    break;
                case MSG_TYPE::HEARTBEAT:
                    no_replay_count = 0;
                    break;
                default:
                    break;
            }

            no_replay_count = 0;
            ++messages_processed;

            if (messages_processed % 5000 == 0) {
                std::cout << "[Listener] " << messages_processed
                          << " messages processed\n";
            }
        }
    }

    close(live_sock);
    close(replay_sock);
}
