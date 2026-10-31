// A one-file swarm for testing BitTorrent where no test can reach a real one:
// the Android emulator, through `adb reverse`.
//
//     torrent_seeder <dir> <port> <seconds>
//
// Writes a 4 MiB payload of known bytes into <dir>, seeds it on 127.0.0.1:<port>
// with no tracker, DHT, LSD or port mapping, and prints one line -- a magnet
// link that names this seeder as its peer (`x.pe`, BEP 9), so a client
// needs nothing else to find it. Exits by itself after <seconds>, so a check
// that dies cannot leave it running. Built by tool/emulator_checks.py with
// the host's libtorrent-rasterbar, the engine the desktop build links.
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
	if (argc != 4) {
		std::fprintf(stderr, "usage: torrent_seeder <dir> <port> <seconds>\n");
		return 2;
	}
	const std::string dir = argv[1];
	const int port = std::atoi(argv[2]);
	const int seconds = std::atoi(argv[3]);
	if (port <= 0 || port > 65535 || seconds <= 0 || seconds > 3600) {
		std::fprintf(stderr, "port 1-65535, seconds 1-3600\n");
		return 2;
	}

	// The payload: bytes a client can check without a copy of the file.
	const std::string name = "hydra-check.bin";
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
	lt::session ses(lt::session_params{ sp });

	lt::add_torrent_params atp;
	atp.ti = ti;
	atp.save_path = dir;
	atp.flags |= lt::torrent_flags::seed_mode;
	ses.add_torrent(atp);

	std::printf("%s&x.pe=127.0.0.1:%d\n", lt::make_magnet_uri(*ti).c_str(), port);
	std::fflush(stdout);
	std::this_thread::sleep_for(std::chrono::seconds(seconds));
	return 0;
}
