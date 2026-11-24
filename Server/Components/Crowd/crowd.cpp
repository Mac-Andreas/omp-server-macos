/* =========================================================================
 *  omp-crowd relay - the server's side of the launcher's Local crowd.
 *
 *  Players' games (omp-crowd.dll) each run GTA's own crowd. When launcher
 *  players are together, one of them owns the crowd around them and the
 *  others see copies (GTA Online's way, shared). This component never runs
 *  any AI and creates nothing in the game: it only links each game's DLL to
 *  its player, relays updates between players in range, and decides who owns
 *  what.
 *
 *  Its own UDP port (the game's port + 1), listed in the server's query
 *  rules as "crowd" so the launcher can find it.
 *
 *  0.1: the link - HELLO (a player's name; it must come from that player's
 *  IP), WELCOME / REFUSED, and a keepalive.
 *  0.2: groups and owners - linked players within 100 m (same world and
 *  interior) form a group; its owner keeps the role while still in it, else
 *  it goes to whoever linked first (a tie: the higher FPS). The owner's crowd
 *  is forwarded to the others; a viewer's hit on a copy goes to the owner.
 *  OMP_CROWD_SOLO_OWNER=1 (testing): a lone player is an owner too.
 *  0.3: the server decides - query rule "crowd" = yes/no (off until a script
 *  turns it on), "crowd_port"; CONFIG tells each game whether it may run its
 *  crowd and how much; Pawn natives (include omp-crowd.inc) for gamemodes and
 *  filterscripts: Crowd_Enable, Crowd_IsEnabled, Crowd_SetDensity,
 *  Crowd_SetLimits, Crowd_SetPlayerEnabled, Crowd_IsPlayerLinked.
 *
 *  Packets: 'O','C','R','W', version, type, then the body.
 *    1 HELLO    u8 nameLen, name
 *    2 WELCOME  u16 playerId, u32 session
 *    3 REFUSED  u8 reason (1 no such player, 2 not from their address, 3 bad packet)
 *    4 PING     u32 session [, u16 fps]
 *    5 PONG     u32 session
 *    6 ROLE     u8 role (0 alone, 1 owner, 2 viewer), u16 ownerId, u8 members
 *    7 CROWD    to the relay: u32 session, payload; from it: u16 ownerId, payload
 *               (payload: u16 seq, u8 count, count x 38-byte entries - see the DLL)
 *    9 CONFIG   u8 on, f32 pedDensity, f32 carDensity, u8 maxPeds, u8 maxCars
 *    8 HIT      to the relay: u32 session, u16 netId, u16 damage x10, u8 weapon;
 *               to the owner: u16 fromId, u16 netId, u16 damage x10, u8 weapon
 * ========================================================================= */

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Pawn/Impl/pawn_natives.hpp>
#include <Server/Components/Pawn/Impl/pawn_impl.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr uint8_t kMagic[4] = { 'O', 'C', 'R', 'W' };
constexpr uint8_t kVersion = 1;
enum : uint8_t { P_HELLO = 1, P_WELCOME = 2, P_REFUSED = 3, P_PING = 4, P_PONG = 5, P_ROLE = 6, P_CROWD = 7, P_HIT = 8, P_CONFIG = 9 };
constexpr auto kPeerTimeout = std::chrono::seconds(10);
constexpr float kGroupM = 100.0f;
constexpr size_t kMaxCrowd = 1200;
}

class CrowdRelay final : public IComponent, public CoreEventHandler, public PlayerConnectEventHandler, public PawnEventHandler
{
	PROVIDE_UID(0x4f6d70437277644c);   // "OmpCrwdL"
	static CrowdRelay* instance_;

	struct Peer
	{
		sockaddr_in addr {};
		int playerId = -1;
		uint32_t session = 0;
		TimePoint lastSeen {};
		TimePoint linkedAt {};
		uint16_t fps = 0;
		int group = -1;          // index of its group this round
		uint8_t role = 0;        // what it was last told
		int ownerId = -1;
		uint8_t members = 0;
		TimePoint roleSent {};
		TimePoint lastCrowd {};
		int crowdThisSec = 0;
		// stats
		long crowdPackets = 0, crowdBytes = 0, crowdObjects = 0;
	};

	ICore* core_ = nullptr;
	int sock_ = -1;
	uint16_t port_ = 0;
	std::vector<Peer> peers_;
	std::mt19937 rng_ { std::random_device {}() };
	TimePoint nextGroups_ {}, nextStats_ {};
	bool soloOwner_ = false;
	IPawnComponent* pawn_ = nullptr;
	// What the server's scripts decided.
	bool enabled_ = false;
	float pedDensity_ = 0.6f, carDensity_ = 0.5f;
	uint8_t maxPeds_ = 20, maxCars_ = 8;
	std::vector<int> playerOff_;   // players a script turned it off for

	bool allowedFor(int playerId) const
	{
		if (!enabled_) return false;
		for (int id : playerOff_) if (id == playerId) return false;
		return true;
	}
	void sendConfig(const Peer& p)
	{
		uint8_t out[11];
		out[0] = allowedFor(p.playerId) ? 1 : 0;
		std::memcpy(out + 1, &pedDensity_, 4);
		std::memcpy(out + 5, &carDensity_, 4);
		out[9] = maxPeds_; out[10] = maxCars_;
		send(p.addr, P_CONFIG, out, 11);
	}
	void configAll() { for (auto& p : peers_) sendConfig(p); }
	void publishRules()
	{
		for (INetwork* net : core_->getNetworks())
			if (auto* q = queryExtension<INetworkQueryExtension>(net))
			{
				q->addRule("crowd", enabled_ ? "yes" : "no");
				q->addRule("crowd_port", std::to_string(port_));
			}
	}

	void send(const sockaddr_in& to, uint8_t type, const uint8_t* body, size_t n)
	{
		uint8_t buf[kMaxCrowd + 16];
		if (n > sizeof(buf) - 6) return;
		std::memcpy(buf, kMagic, 4);
		buf[4] = kVersion;
		buf[5] = type;
		if (n) std::memcpy(buf + 6, body, n);
		sendto(sock_, buf, 6 + n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
	}

	void refuse(const sockaddr_in& to, uint8_t reason) { send(to, P_REFUSED, &reason, 1); }

	Peer* findPeer(const sockaddr_in& from)
	{
		for (auto& p : peers_)
			if (p.addr.sin_addr.s_addr == from.sin_addr.s_addr && p.addr.sin_port == from.sin_port) return &p;
		return nullptr;
	}

	// A player's game says who it is: it must be a connected player of that name, and the
	// packet must come from the address that player is connected from.
	void onHello(const sockaddr_in& from, const uint8_t* body, size_t n, TimePoint now)
	{
		if (n < 1 || n < size_t(1 + body[0]) || body[0] == 0 || body[0] > 24) { refuse(from, 3); return; }
		std::string name(reinterpret_cast<const char*>(body + 1), body[0]);
		IPlayer* who = nullptr;
		for (IPlayer* pl : core_->getPlayers().entries())
		{
			if (pl->isBot()) continue;
			StringView pn = pl->getName();
			if (pn.size() == name.size() && strncasecmp(pn.data(), name.data(), name.size()) == 0) { who = pl; break; }
		}
		if (!who) { refuse(from, 1); return; }
		const PeerNetworkData& nd = who->getNetworkData();
		if (nd.networkID.address.ipv6 || nd.networkID.address.v4 != from.sin_addr.s_addr) { refuse(from, 2); return; }
		Peer* p = findPeer(from);
		if (!p)
		{
			// One link per player: a new one replaces the old.
			for (size_t i = 0; i < peers_.size(); i++)
				if (peers_[i].playerId == who->getID()) { peers_.erase(peers_.begin() + i); break; }
			peers_.push_back(Peer {});
			p = &peers_.back();
			p->addr = from;
			p->playerId = who->getID();
			p->session = rng_() | 1u;
			p->linkedAt = now;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			core_->printLn("[crowd] %.*s (id %d) linked from %s:%u", int(name.size()), name.data(), p->playerId, ip, unsigned(ntohs(from.sin_port)));
		}
		p->lastSeen = now;
		uint8_t out[6];
		uint16_t id = uint16_t(p->playerId);
		std::memcpy(out, &id, 2);
		std::memcpy(out + 2, &p->session, 4);
		send(from, P_WELCOME, out, 6);
		sendConfig(*p);
	}

	void onPing(const sockaddr_in& from, const uint8_t* body, size_t n, TimePoint now)
	{
		Peer* p = findPeer(from);
		uint32_t s = 0;
		if (n >= 4) std::memcpy(&s, body, 4);
		if (!p || s != p->session) { refuse(from, 1); return; }
		p->lastSeen = now;
		if (n >= 6) std::memcpy(&p->fps, body + 4, 2);
		send(from, P_PONG, body, 4);
	}

	int p_id(int i) const { return peers_[i].playerId; }

	Peer* byPlayer(int id)
	{
		for (auto& p : peers_) if (p.playerId == id) return &p;
		return nullptr;
	}

	// Groups: linked players within kGroupM of each other (in the same world and interior), by
	// a grid so it stays cheap with many players; then each group's owner.
	void regroup(TimePoint now)
	{
		struct Pos { float x, y; int vw, interior; bool ok; };
		std::vector<Pos> pos(peers_.size());
		for (size_t i = 0; i < peers_.size(); i++)
		{
			IPlayer* pl = core_->getPlayers().get(peers_[i].playerId);
			pos[i].ok = pl != nullptr;
			if (!pl) continue;
			Vector3 v = pl->getPosition();
			pos[i] = { v.x, v.y, pl->getVirtualWorld(), int(pl->getInterior()), true };
		}
		std::vector<int> parent(peers_.size());
		for (size_t i = 0; i < parent.size(); i++) parent[i] = int(i);
		auto find = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
		std::unordered_map<long long, std::vector<int>> grid;
		auto cellKey = [](int cx, int cy) { return (long long)cx * 1000003LL + cy; };
		for (size_t i = 0; i < peers_.size(); i++)
			if (pos[i].ok) grid[cellKey(int(std::floor(pos[i].x / kGroupM)), int(std::floor(pos[i].y / kGroupM)))].push_back(int(i));
		for (size_t i = 0; i < peers_.size(); i++)
		{
			if (!pos[i].ok) continue;
			int cx = int(std::floor(pos[i].x / kGroupM)), cy = int(std::floor(pos[i].y / kGroupM));
			for (int dx = -1; dx <= 1; dx++)
				for (int dy = -1; dy <= 1; dy++)
				{
					auto it = grid.find(cellKey(cx + dx, cy + dy));
					if (it == grid.end()) continue;
					for (int j : it->second)
					{
						if (j <= int(i) || pos[j].vw != pos[i].vw || pos[j].interior != pos[i].interior) continue;
						float ddx = pos[j].x - pos[i].x, ddy = pos[j].y - pos[i].y;
						if (ddx * ddx + ddy * ddy <= kGroupM * kGroupM) parent[find(j)] = find(int(i));
					}
				}
		}
		std::unordered_map<int, std::vector<int>> groups;
		for (size_t i = 0; i < peers_.size(); i++) if (pos[i].ok) groups[find(int(i))].push_back(int(i));
		for (auto& kv : groups)
		{
			auto& m = kv.second;
			int owner = -1;
			for (int i : m) if (peers_[i].role == 1 && peers_[i].ownerId == peers_[i].playerId) { owner = i; break; }   // the owner stays the owner
			if (owner < 0)
				for (int i : m)
					if (owner < 0 || peers_[i].linkedAt < peers_[owner].linkedAt || (peers_[i].linkedAt == peers_[owner].linkedAt && peers_[i].fps > peers_[owner].fps)) owner = i;
			for (int i : m)
			{
				Peer& p = peers_[i];
				uint8_t role = (m.size() == 1) ? (soloOwner_ ? 1 : 0) : (i == owner ? 1 : 2);
				if (!allowedFor(p_id(i))) role = 0;
				int ownerId = peers_[owner].playerId;
				p.group = kv.first;
				if (role != p.role || ownerId != p.ownerId || uint8_t(m.size()) != p.members || now - p.roleSent > std::chrono::seconds(5))
				{
					if (role != p.role) core_->printLn("[crowd] player %d: %s (owner %d, %u in the group)", p.playerId, role == 1 ? "owner" : role == 2 ? "viewer" : "alone", ownerId, unsigned(m.size()));
					p.role = role; p.ownerId = ownerId; p.members = uint8_t(m.size()); p.roleSent = now;
					uint8_t out[4] = { role, uint8_t(ownerId & 0xFF), uint8_t(ownerId >> 8), p.members };
					send(p.addr, P_ROLE, out, 4);
					sendConfig(p);
				}
			}
		}
	}

	// The owner's crowd, to everyone else in its group.
	void onCrowd(const sockaddr_in& from, const uint8_t* body, size_t n, TimePoint now)
	{
		Peer* p = findPeer(from);
		uint32_t s = 0;
		if (n >= 4) std::memcpy(&s, body, 4);
		if (!p || s != p->session || p->role != 1 || n < 7 || n - 4 > kMaxCrowd - 2) return;
		if (now - p->lastCrowd > std::chrono::seconds(1)) { p->lastCrowd = now; p->crowdThisSec = 0; }
		if (++p->crowdThisSec > 12) return;
		p->crowdPackets++; p->crowdBytes += long(n); p->crowdObjects += body[6];
		uint8_t out[kMaxCrowd];
		uint16_t id = uint16_t(p->playerId);
		std::memcpy(out, &id, 2);
		std::memcpy(out + 2, body + 4, n - 4);
		for (auto& o : peers_)
			if (&o != p && o.group == p->group && o.role == 2 && o.ownerId == p->playerId) send(o.addr, P_CROWD, out, n - 2);
	}

	// A viewer hit a copy: the owner's game applies it to the real one.
	void onHit(const sockaddr_in& from, const uint8_t* body, size_t n)
	{
		Peer* p = findPeer(from);
		uint32_t s = 0;
		if (n >= 9) std::memcpy(&s, body, 4);
		if (!p || s != p->session || p->role != 2 || n < 9) return;
		Peer* owner = byPlayer(p->ownerId);
		if (!owner) return;
		uint8_t out[7];
		uint16_t id = uint16_t(p->playerId);
		std::memcpy(out, &id, 2);
		std::memcpy(out + 2, body + 4, 5);
		send(owner->addr, P_HIT, out, 7);
	}

public:
	StringView componentName() const override { return "omp-crowd relay"; }
	SemanticVersion componentVersion() const override { return SemanticVersion(0, 3, 0, 0); }

	void onLoad(ICore* c) override
	{
		core_ = c;
		core_->getEventDispatcher().addEventHandler(this);
		core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
	}

	void onInit(IComponentList* components) override
	{
		pawn_ = components->queryComponent<IPawnComponent>();
		if (pawn_)
		{
			setAmxFunctions(pawn_->getAmxFunctions());
			setAmxLookups(components);
			pawn_->getEventDispatcher().addEventHandler(this);
		}
	}
	void onAmxLoad(IPawnScript& script) override { pawn_natives::AmxLoad(script.GetAMX()); }
	void onAmxUnload(IPawnScript&) override { }

	// For the natives.
	static CrowdRelay* get() { return instance_; }
	static void setInstance(CrowdRelay* r) { instance_ = r; }
	void setEnabled(bool on)
	{
		if (on == enabled_) return;
		enabled_ = on;
		publishRules();
		configAll();
		core_->printLn("[crowd] the crowd is %s by a script", on ? "ON" : "OFF");
	}
	bool isEnabled() const { return enabled_; }
	void setDensity(float peds, float cars)
	{
		pedDensity_ = peds < 0 ? 0 : peds > 1 ? 1 : peds;
		carDensity_ = cars < 0 ? 0 : cars > 1 ? 1 : cars;
		configAll();
	}
	void setLimits(int peds, int cars)
	{
		maxPeds_ = uint8_t(peds < 0 ? 0 : peds > 40 ? 40 : peds);
		maxCars_ = uint8_t(cars < 0 ? 0 : cars > 20 ? 20 : cars);
		configAll();
	}
	void setPlayerEnabled(int id, bool on)
	{
		for (size_t i = 0; i < playerOff_.size(); i++) if (playerOff_[i] == id) { playerOff_.erase(playerOff_.begin() + i); break; }
		if (!on) playerOff_.push_back(id);
		if (Peer* p = byPlayer(id)) sendConfig(*p);
	}
	bool isLinked(int id) { return byPlayer(id) != nullptr; }

	void onReady() override
	{
		int* gamePort = core_->getConfig().getInt("network.port");
		port_ = uint16_t((gamePort ? *gamePort : 7777) + 1);
		sock_ = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons(port_);
		if (sock_ < 0 || bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		{
			core_->logLn(LogLevel::Error, "[crowd] couldn't open UDP port %u - the Local crowd relay is off", unsigned(port_));
			if (sock_ >= 0) close(sock_);
			sock_ = -1;
			return;
		}
		fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL, 0) | O_NONBLOCK);
		const char* solo = getenv("OMP_CROWD_SOLO_OWNER");
		soloOwner_ = solo && solo[0] == '1';
		publishRules();
		core_->printLn("[crowd] omp-crowd relay 0.3.0 on UDP %u (query rules crowd=no until a script turns it on, crowd_port)%s", unsigned(port_), soloOwner_ ? " - testing: a lone player owns its crowd" : "");
	}

	void onTick(Microseconds, TimePoint now) override
	{
		if (sock_ < 0) return;
		uint8_t buf[kMaxCrowd + 64];
		for (int k = 0; k < 256; k++)
		{
			sockaddr_in from {};
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
			if (n <= 0) break;
			if (n < 6 || std::memcmp(buf, kMagic, 4) != 0 || buf[4] != kVersion) continue;
			switch (buf[5])
			{
			case P_HELLO: onHello(from, buf + 6, size_t(n - 6), now); break;
			case P_PING: onPing(from, buf + 6, size_t(n - 6), now); break;
			case P_CROWD: onCrowd(from, buf + 6, size_t(n - 6), now); break;
			case P_HIT: onHit(from, buf + 6, size_t(n - 6)); break;
			default: break;
			}
		}
		if (now >= nextGroups_) { regroup(now); nextGroups_ = now + std::chrono::milliseconds(500); }
		if (now >= nextStats_)
		{
			for (auto& p : peers_)
				if (p.crowdPackets)
				{
					core_->printLn("[crowd] owner %d sent %ld crowd packets in 10 s (%.1f objects each, %ld B/s)", p.playerId, p.crowdPackets, double(p.crowdObjects) / double(p.crowdPackets), p.crowdBytes / 10);
					p.crowdPackets = p.crowdBytes = p.crowdObjects = 0;
				}
			nextStats_ = now + std::chrono::seconds(10);
		}
		for (size_t i = 0; i < peers_.size();)
		{
			if (now - peers_[i].lastSeen > kPeerTimeout)
			{
				core_->printLn("[crowd] player %d's link timed out", peers_[i].playerId);
				peers_.erase(peers_.begin() + i);
			}
			else i++;
		}
	}

	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason) override
	{
		for (size_t i = 0; i < playerOff_.size(); i++) if (playerOff_[i] == player.getID()) { playerOff_.erase(playerOff_.begin() + i); break; }
		for (size_t i = 0; i < peers_.size(); i++)
			if (peers_[i].playerId == player.getID()) { peers_.erase(peers_.begin() + i); break; }
	}

	void onFree(IComponent*) override { }

	void free() override
	{
		if (core_)
		{
			core_->getEventDispatcher().removeEventHandler(this);
			core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
		}
		if (pawn_) pawn_->getEventDispatcher().removeEventHandler(this);
		instance_ = nullptr;
		if (sock_ >= 0) close(sock_);
		delete this;
	}

	void reset() override { peers_.clear(); }
};

CrowdRelay* CrowdRelay::instance_ = nullptr;

// ---- Pawn natives (omp-crowd.inc) -------------------------------------------------------------
SCRIPT_API(Crowd_Enable, bool(bool enabled)) { if (auto r = CrowdRelay::get()) { r->setEnabled(enabled); return true; } return false; }
SCRIPT_API(Crowd_IsEnabled, bool()) { auto r = CrowdRelay::get(); return r && r->isEnabled(); }
SCRIPT_API(Crowd_SetDensity, bool(float peds, float cars)) { if (auto r = CrowdRelay::get()) { r->setDensity(peds, cars); return true; } return false; }
SCRIPT_API(Crowd_SetLimits, bool(int maxPeds, int maxCars)) { if (auto r = CrowdRelay::get()) { r->setLimits(maxPeds, maxCars); return true; } return false; }
SCRIPT_API(Crowd_SetPlayerEnabled, bool(IPlayer& player, bool enabled)) { if (auto r = CrowdRelay::get()) { r->setPlayerEnabled(player.getID(), enabled); return true; } return false; }
SCRIPT_API(Crowd_IsPlayerLinked, bool(IPlayer& player)) { auto r = CrowdRelay::get(); return r && r->isLinked(player.getID()); }

COMPONENT_ENTRY_POINT()
{
	auto* r = new CrowdRelay();
	CrowdRelay::setInstance(r);
	return r;
}
