// Small helpers shared by the online module: hex and base64, hashing, random
// numbers, and the clock.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::online {

// 16 hex digits, upper case: how 64-bit ids (XUIDs, session ids, machine ids)
// travel in JSON, where a double could not hold them.
std::string Hex64(uint64_t v);
// Parses Hex64 output (any case, optional 0x); 0 when the text is not hex.
uint64_t ParseHex64(std::string_view s);

std::string HexBytes(const uint8_t* data, size_t size);
std::vector<uint8_t> ParseHexBytes(std::string_view s);

std::string Base64Encode(const uint8_t* data, size_t size);
std::vector<uint8_t> Base64Decode(std::string_view s);

// FNV-1a: stable across runs and machines, which is what identities derived
// from the player's name need.
uint64_t Fnv1a64(std::string_view s);

uint64_t RandomU64();
void RandomBytes(uint8_t* out, size_t size);

// Milliseconds of a steady clock.
uint64_t NowMs();

// "host:port" into its parts; the default port when there is none.
bool SplitHostPort(std::string_view text, std::string* host, uint16_t* port,
                   uint16_t default_port);

// Dotted IPv4 text of a host-order address, and back (0 when not an address).
std::string Ipv4ToString(uint32_t host_order);
uint32_t ParseIpv4(std::string_view text);

std::string Trim(std::string_view s);
std::string Lower(std::string_view s);

}  // namespace mcla::online
