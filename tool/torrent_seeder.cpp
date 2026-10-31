// A one-file swarm for testing BitTorrent where no test can reach a real one:
// the Android emulator, through `adb reverse`.
//
//     torrent_seeder <dir> <port> <seconds> [bytes-per-second] [name]
//
// Writes a 4 MiB payload of known bytes into <dir>, seeds it on 127.0.0.1:<port>
// with no tracker, DHT, LSD or port mapping, and prints one line -- a magnet
// link that names this seeder as its peer (`x.pe`, BEP 9), so a client
// needs nothing else to find it. Exits by itself after <seconds>, so a check
// that dies cannot leave it running. A rate, when given, caps the upload, so
// a check can leave the app while a download is still moving. Built by
// tool/emulator_checks.py with the host's libtorrent-rasterbar, the engine
// the desktop build links. The .torrent itself is written beside the payload
// as <name>.torrent, for a check that serves it the way a site does; a name
// other than the default gives a different swarm. A .torrent carries no peer,
// so with HYDRA_SEED_CONNECT=host:port set the seeder dials that address every
// two seconds -- `adb forward` to the client's listening port -- as the
// desktop suite's seeder does.
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lt = libtorrent;

int main(int argc, char **argv) {
	if (argc < 4 || argc > 6) {
		std::fprintf(stderr, "usage: torrent_seeder <dir> <port> <seconds> [bytes-per-second] [name]\n");
		return 2;
	}
	const std::string dir = argv[1];
	const int port = std::atoi(argv[2]);
	const int seconds = std::atoi(argv[3]);
	const int rate = argc >= 5 ? std::atoi(argv[4]) : 0;
	if (port <= 0 || port > 65535 || seconds <= 0 || seconds > 3600) {
		std::fprintf(stderr, "port 1-65535, seconds 1-3600\n");
		return 2;
	}

	// The payload: bytes a client can check without a copy of the file.
	const std::string name = argc == 6 ? argv[5] : "hydra-check.bin";
	if (name.empty() || name.find('/') != std::string::npos) {
		std::fprintf(stderr, "a name, not a path\n");
		return 2;
	}
	{
		std::ofstream out(dir + "/" + name, std::ios::binary);
		std::vector<char> blob(4 << 20);
		for (size_t i = 0; i < blob.size(); ++i)
			blob[i] = char((i * 31 + 7) & 0xff);
		out.write(blob.data(), std::streamsize(blob.size()));
		if (!out) {
			std::fprintf(stderr, "could not write the payload in %s\n", dir.c_str());
			return 1;
		}
	}

	lt::file_storage fs;
	lt::add_files(fs, dir + "/" + name);
	lt::create_torrent ct(fs, 256 * 1024);
	lt::set_piece_hashes(ct, dir);
	const std::vector<char> buf = ct.generate_buf();
	auto ti = std::make_shared<lt::torrent_info>(buf, lt::from_span);
	{
		std::ofstream t(dir + "/" + name + ".torrent", std::ios::binary);
		t.write(buf.data(), std::streamsize(buf.size()));
	}

	lt::settings_pack sp;
	sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:" + std::to_string(port));
	sp.set_bool(lt::settings_pack::enable_dht, false);
	sp.set_bool(lt::settings_pack::enable_lsd, false);
	sp.set_bool(lt::settings_pack::enable_upnp, false);
	sp.set_bool(lt::settings_pack::enable_natpmp, false);
	// `adb reverse` carries TCP only, so the client's uTP attempt has to fail
	// fast and leave TCP to succeed.
	sp.set_bool(lt::settings_pack::enable_incoming_utp, false);
	sp.set_bool(lt::settings_pack::enable_outgoing_utp, false);
	if (rate > 0)
		sp.set_int(lt::settings_pack::upload_rate_limit, rate);
	lt::session ses(lt::session_params{ sp });

	lt::add_torrent_params atp;
	atp.ti = ti;
	atp.save_path = dir;
	atp.flags |= lt::torrent_flags::seed_mode;
	lt::torrent_handle th = ses.add_torrent(atp);

	std::printf("%s&x.pe=127.0.0.1:%d\n", lt::make_magnet_uri(*ti).c_str(), port);
	std::fflush(stdout);
	const char *dial = std::getenv("HYDRA_SEED_CONNECT");
	std::string host;
	int dial_port = 0;
	if (dial) {
		const std::string d = dial;
		const size_t colon = d.rfind(':');
		if (colon != std::string::npos) {
			host = d.substr(0, colon);
			dial_port = std::atoi(d.c_str() + colon + 1);
		}
	}
	for (int left = seconds; left > 0; left -= 2) {
		if (!host.empty() && dial_port > 0)
			th.connect_peer(lt::tcp::endpoint(lt::make_address(host),
			                                  std::uint16_t(dial_port)));
		std::this_thread::sleep_for(std::chrono::seconds(left < 2 ? left : 2));
	}
	return 0;
}
