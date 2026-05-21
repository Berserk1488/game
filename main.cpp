#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr uint16_t MAGIC = 0x4750; // 'GP' = Game Protocol
static constexpr uint8_t VERSION = 1;
static constexpr size_t HEADER_SIZE = 14;
static constexpr int DEFAULT_TIMEOUT_MS = 800;
static constexpr int MAX_RETRIES = 8;

enum MsgType : uint8_t {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_MOVE = 3,
    MSG_RESULT = 4,
    MSG_ACK = 5,
    MSG_ERROR = 6,
    MSG_BYE = 7
};

struct Packet {
    uint8_t type = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;
    std::string payload;
};

static std::string typeName(uint8_t t) {
    switch (t) {
        case MSG_HELLO: return "HELLO";
        case MSG_WELCOME: return "WELCOME";
        case MSG_MOVE: return "MOVE";
        case MSG_RESULT: return "RESULT";
        case MSG_ACK: return "ACK";
        case MSG_ERROR: return "ERROR";
        case MSG_BYE: return "BYE";
        default: return "UNKNOWN";
    }
}

static std::vector<uint8_t> serialize(const Packet& p) {
    if (p.payload.size() > 65535) {
        throw std::runtime_error("payload is too large");
    }

    std::vector<uint8_t> buf(HEADER_SIZE + p.payload.size());
    uint16_t magic = htons(MAGIC);
    uint32_t seq = htonl(p.seq);
    uint32_t ack = htonl(p.ack);
    uint16_t len = htons(static_cast<uint16_t>(p.payload.size()));

    std::memcpy(buf.data() + 0, &magic, 2);
    buf[2] = VERSION;
    buf[3] = p.type;
    std::memcpy(buf.data() + 4, &seq, 4);
    std::memcpy(buf.data() + 8, &ack, 4);
    std::memcpy(buf.data() + 12, &len, 2);
    std::memcpy(buf.data() + HEADER_SIZE, p.payload.data(), p.payload.size());

    return buf;
}

static bool parsePacket(const uint8_t* data, size_t n, Packet& p) {
    if (n < HEADER_SIZE) return false;

    uint16_t magic = 0;
    std::memcpy(&magic, data + 0, 2);
    magic = ntohs(magic);
    if (magic != MAGIC) return false;
    if (data[2] != VERSION) return false;

    uint16_t len = 0;
    std::memcpy(&len, data + 12, 2);
    len = ntohs(len);
    if (n != HEADER_SIZE + len) return false;

    uint32_t seq = 0;
    uint32_t ack = 0;
    std::memcpy(&seq, data + 4, 4);
    std::memcpy(&ack, data + 8, 4);

    p.type = data[3];
    p.seq = ntohl(seq);
    p.ack = ntohl(ack);
    p.payload.assign(reinterpret_cast<const char*>(data + HEADER_SIZE), len);
    return true;
}

static sockaddr_in makeAddr(const std::string& host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        return addr;
    }

    hostent* he = gethostbyname(host.c_str());
    if (!he || he->h_addrtype != AF_INET) {
        throw std::runtime_error("cannot resolve host: " + host);
    }
    std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    return addr;
}

static sockaddr_in bindUdp(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return addr;
}

static std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

static bool sameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family &&
           a.sin_addr.s_addr == b.sin_addr.s_addr &&
           a.sin_port == b.sin_port;
}

class UdpPeer {
public:
    explicit UdpPeer(uint16_t localPort) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in local = bindUdp(localPort);
        if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            close(sock_);
            throw std::runtime_error("bind() failed; port may be busy");
        }
    }

    ~UdpPeer() {
        if (sock_ >= 0) close(sock_);
    }

    void sendRaw(const Packet& p, const sockaddr_in& to) {
        auto bytes = serialize(p);
        ssize_t sent = sendto(sock_, bytes.data(), bytes.size(), 0,
                              reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (sent < 0 || static_cast<size_t>(sent) != bytes.size()) {
            throw std::runtime_error("sendto() failed");
        }

        std::cout << "[send] " << typeName(p.type)
                  << " seq=" << p.seq
                  << " ack=" << p.ack
                  << " to=" << addrToString(to);
        if (!p.payload.empty()) std::cout << " payload=\"" << p.payload << "\"";
        std::cout << "\n";
    }

    bool recvRaw(Packet& p, sockaddr_in& from, int timeoutMs) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_, &fds);

        timeval tv{};
        timeval* tvPtr = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvPtr = &tv;
        }

        int ready = select(sock_ + 1, &fds, nullptr, nullptr, tvPtr);
        if (ready < 0) {
            if (errno == EINTR) return false;
            throw std::runtime_error("select() failed");
        }
        if (ready == 0) return false;

        uint8_t buf[2048];
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) {
            throw std::runtime_error("recvfrom() failed");
        }

        if (!parsePacket(buf, static_cast<size_t>(n), p)) {
            std::cout << "[drop] invalid packet from " << addrToString(from) << "\n";
            return false;
        }

        std::cout << "[recv] " << typeName(p.type)
                  << " seq=" << p.seq
                  << " ack=" << p.ack
                  << " from=" << addrToString(from);
        if (!p.payload.empty()) std::cout << " payload=\"" << p.payload << "\"";
        std::cout << "\n";

        return true;
    }

    void sendAck(uint32_t seq, const sockaddr_in& to) {
        Packet ack{};
        ack.type = MSG_ACK;
        ack.seq = 0;
        ack.ack = seq;
        sendRaw(ack, to);
    }

    bool sendReliable(uint8_t type, uint32_t seq, const std::string& payload,
                      const sockaddr_in& peer) {
        Packet out{};
        out.type = type;
        out.seq = seq;
        out.ack = 0;
        out.payload = payload;

        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            sendRaw(out, peer);

            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(DEFAULT_TIMEOUT_MS);

            while (std::chrono::steady_clock::now() < deadline) {
                Packet in{};
                sockaddr_in from{};
                int left = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()).count()
                );
                if (left < 1) left = 1;

                if (!recvRaw(in, from, left)) continue;
                if (!sameEndpoint(from, peer)) {
                    std::cout << "[drop] packet from another endpoint\n";
                    continue;
                }

                if (in.type == MSG_ACK && in.ack == seq) {
                    std::cout << "[ok] ACK received for seq=" << seq << "\n";
                    return true;
                }

                if (in.type != MSG_ACK) {
                    // Пока ждём ACK, полезные сообщения не обрабатываем,
                    // но дубли подтверждаем, чтобы второй узел не завис.
                    sendAck(in.seq, peer);
                    std::cout << "[info] data packet received while waiting ACK; ACK resent\n";
                }
            }

            std::cout << "[timeout] no ACK for seq=" << seq
                      << ", retry " << attempt << "/" << MAX_RETRIES << "\n";
        }

        return false;
    }

    Packet recvReliable(uint32_t expectedSeq, const sockaddr_in& peer) {
        while (true) {
            Packet in{};
            sockaddr_in from{};
            if (!recvRaw(in, from, -1)) continue;

            if (!sameEndpoint(from, peer)) {
                std::cout << "[drop] packet from another endpoint: "
                          << addrToString(from) << "\n";
                continue;
            }

            if (in.type == MSG_ACK) {
                std::cout << "[info] standalone ACK ignored\n";
                continue;
            }

            if (in.seq == expectedSeq) {
                sendAck(in.seq, peer);
                return in;
            }

            if (in.seq < expectedSeq) {
                std::cout << "[dup] duplicate " << typeName(in.type)
                          << " seq=" << in.seq << "; resend ACK\n";
                sendAck(in.seq, peer);
                continue;
            }

            std::cout << "[order] expected seq=" << expectedSeq
                      << ", got seq=" << in.seq << "; packet ignored\n";
        }
    }

    Packet recvReliableAny(uint32_t expectedSeq, sockaddr_in& peerOut) {
        while (true) {
            Packet in{};
            sockaddr_in from{};
            if (!recvRaw(in, from, -1)) continue;
            if (in.type == MSG_ACK) continue;

            if (in.seq == expectedSeq) {
                peerOut = from;
                sendAck(in.seq, from);
                return in;
            }

            std::cout << "[order] expected seq=" << expectedSeq
                      << ", got seq=" << in.seq << "; packet ignored\n";
        }
    }

private:
    int sock_ = -1;
};

static std::string normalizeMove(const std::string& s) {
    if (s == "r" || s == "rock" || s == "камень") return "rock";
    if (s == "p" || s == "paper" || s == "бумага") return "paper";
    if (s == "s" || s == "scissors" || s == "ножницы") return "scissors";
    return "";
}

static std::string askMove(const std::string& prompt) {
    while (true) {
        std::cout << prompt << " [r=rock, p=paper, s=scissors, q=quit]: ";
        std::string s;
        std::cin >> s;
        if (s == "q" || s == "quit") return "quit";
        std::string m = normalizeMove(s);
        if (!m.empty()) return m;
        std::cout << "Unknown move. Try again.\n";
    }
}

static int cmpMoves(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if ((a == "rock" && b == "scissors") ||
        (a == "scissors" && b == "paper") ||
        (a == "paper" && b == "rock")) {
        return 1;
    }
    return -1;
}

static void runMaster(uint16_t port) {
    UdpPeer udp(port);
    std::cout << "Master mode. Waiting HELLO on UDP port " << port << "...\n";

    sockaddr_in player{};
    Packet hello = udp.recvReliableAny(1, player);
    if (hello.type != MSG_HELLO) {
        std::cout << "First packet is not HELLO. Exit.\n";
        return;
    }

    std::cout << "Player connected: " << addrToString(player) << "\n";

    uint32_t sendSeq = 1;
    uint32_t recvSeq = 2;

    if (!udp.sendReliable(MSG_WELCOME, sendSeq++, "accepted", player)) {
        std::cout << "Cannot deliver WELCOME.\n";
        return;
    }

    int masterScore = 0;
    int playerScore = 0;

    for (int round = 1; round <= 3; ++round) {
        std::cout << "\n=== Round " << round << "/3 ===\n";
        std::cout << "Waiting player's MOVE...\n";

        Packet movePkt = udp.recvReliable(recvSeq++, player);
        if (movePkt.type == MSG_BYE) {
            std::cout << "Player left the game.\n";
            return;
        }
        if (movePkt.type != MSG_MOVE) {
            std::cout << "Unexpected packet. Exit.\n";
            return;
        }

        std::string playerMove = normalizeMove(movePkt.payload);
        if (playerMove.empty()) {
            udp.sendReliable(MSG_ERROR, sendSeq++, "bad move", player);
            return;
        }

        std::string masterMove = askMove("Your move");
        if (masterMove == "quit") {
            udp.sendReliable(MSG_BYE, sendSeq++, "master quit", player);
            return;
        }

        int r = cmpMoves(masterMove, playerMove);
        std::string winner;
        if (r > 0) {
            ++masterScore;
            winner = "master";
        } else if (r < 0) {
            ++playerScore;
            winner = "player";
        } else {
            winner = "draw";
        }

        std::string result =
            "round=" + std::to_string(round) +
            ";master=" + masterMove +
            ";player=" + playerMove +
            ";winner=" + winner +
            ";score=" + std::to_string(masterScore) + ":" + std::to_string(playerScore);

        if (!udp.sendReliable(MSG_RESULT, sendSeq++, result, player)) {
            std::cout << "Cannot deliver RESULT.\n";
            return;
        }
    }

    udp.sendReliable(MSG_BYE, sendSeq++, "game over", player);
    std::cout << "Game over.\n";
}

static void runPlayer(const std::string& host, uint16_t port, uint16_t localPort) {
    UdpPeer udp(localPort);
    sockaddr_in master = makeAddr(host, port);

    std::cout << "Player mode. Master endpoint: " << addrToString(master) << "\n";

    uint32_t sendSeq = 1;
    uint32_t recvSeq = 1;

    if (!udp.sendReliable(MSG_HELLO, sendSeq++, "want-game", master)) {
        std::cout << "Cannot deliver HELLO.\n";
        return;
    }

    Packet welcome = udp.recvReliable(recvSeq++, master);
    if (welcome.type != MSG_WELCOME) {
        std::cout << "Master rejected or sent unexpected packet.\n";
        return;
    }

    std::cout << "Connected: " << welcome.payload << "\n";

    for (int round = 1; round <= 3; ++round) {
        std::cout << "\n=== Round " << round << "/3 ===\n";
        std::string move = askMove("Your move");
        if (move == "quit") {
            udp.sendReliable(MSG_BYE, sendSeq++, "player quit", master);
            return;
        }

        if (!udp.sendReliable(MSG_MOVE, sendSeq++, move, master)) {
            std::cout << "Cannot deliver MOVE.\n";
            return;
        }

        Packet result = udp.recvReliable(recvSeq++, master);
        if (result.type == MSG_BYE) {
            std::cout << "Master finished game: " << result.payload << "\n";
            return;
        }
        if (result.type == MSG_ERROR) {
            std::cout << "Protocol error: " << result.payload << "\n";
            return;
        }
        if (result.type != MSG_RESULT) {
            std::cout << "Unexpected packet.\n";
            return;
        }

        std::cout << "Result: " << result.payload << "\n";
    }

    Packet bye = udp.recvReliable(recvSeq++, master);
    if (bye.type == MSG_BYE) {
        std::cout << "Game finished: " << bye.payload << "\n";
    }
}

static void usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --master <port>\n"
        << "  " << argv0 << " --player <master_ip> <master_port> [local_port]\n\n"
        << "Examples:\n"
        << "  " << argv0 << " --master 5000\n"
        << "  " << argv0 << " --player 192.168.1.34 5000\n";
}

int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--master") {
            uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
            runMaster(port);
            return 0;
        }

        if (argc >= 4 && std::string(argv[1]) == "--player") {
            std::string host = argv[2];
            uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));
            uint16_t localPort = 0;
            if (argc >= 5) {
                localPort = static_cast<uint16_t>(std::stoi(argv[4]));
            }
            runPlayer(host, port, localPort);
            return 0;
        }

        usage(argv[0]);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 2;
    }
}
