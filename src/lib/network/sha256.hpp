/*
Copyright (c) 2026 acrion innovations GmbH

Small dependency-free SHA-256 implementation used to verify hosted partial
query artifacts before they become visible to the graph.
*/
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zelph::network::sha256
{
    class Hasher
    {
    public:
        Hasher()
            : _state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}
        {
        }

        void update(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            _total_bytes += size;
            while (size > 0)
            {
                const size_t available = _buffer.size() - _buffer_size;
                const size_t take = size < available ? size : available;
                for (size_t i = 0; i < take; ++i) _buffer[_buffer_size + i] = bytes[i];
                _buffer_size += take;
                bytes += take;
                size -= take;
                if (_buffer_size == _buffer.size())
                {
                    transform(_buffer.data());
                    _buffer_size = 0;
                }
            }
        }

        void update(std::string_view value) { update(value.data(), value.size()); }

        std::array<uint8_t, 32> finish()
        {
            const uint64_t bits = _total_bytes * 8;
            _buffer[_buffer_size++] = 0x80;
            if (_buffer_size > 56)
            {
                while (_buffer_size < 64) _buffer[_buffer_size++] = 0;
                transform(_buffer.data());
                _buffer_size = 0;
            }
            while (_buffer_size < 56) _buffer[_buffer_size++] = 0;
            for (int i = 7; i >= 0; --i)
                _buffer[_buffer_size++] = static_cast<uint8_t>((bits >> (i * 8)) & 0xffu);
            transform(_buffer.data());

            std::array<uint8_t, 32> out{};
            for (size_t i = 0; i < _state.size(); ++i)
            {
                out[i * 4]     = static_cast<uint8_t>(_state[i] >> 24);
                out[i * 4 + 1] = static_cast<uint8_t>(_state[i] >> 16);
                out[i * 4 + 2] = static_cast<uint8_t>(_state[i] >> 8);
                out[i * 4 + 3] = static_cast<uint8_t>(_state[i]);
            }
            return out;
        }

    private:
        static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

        void transform(const uint8_t* block)
        {
            static constexpr std::array<uint32_t, 64> k{
                0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
                0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
                0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
                0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
                0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
                0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
                0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
                0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

            std::array<uint32_t, 64> w{};
            for (size_t i = 0; i < 16; ++i)
                w[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
                     | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
                     | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
                     | static_cast<uint32_t>(block[i * 4 + 3]);
            for (size_t i = 16; i < 64; ++i)
            {
                const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a=_state[0], b=_state[1], c=_state[2], d=_state[3];
            uint32_t e=_state[4], f=_state[5], g=_state[6], h=_state[7];
            for (size_t i = 0; i < 64; ++i)
            {
                const uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
                const uint32_t ch = (e & f) ^ ((~e) & g);
                const uint32_t t1 = h + s1 + ch + k[i] + w[i];
                const uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = s0 + maj;
                h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            _state[0]+=a; _state[1]+=b; _state[2]+=c; _state[3]+=d;
            _state[4]+=e; _state[5]+=f; _state[6]+=g; _state[7]+=h;
        }

        std::array<uint32_t, 8> _state;
        std::array<uint8_t, 64> _buffer{};
        size_t _buffer_size = 0;
        uint64_t _total_bytes = 0;
    };

    inline std::string hex(const std::array<uint8_t, 32>& digest)
    {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const uint8_t byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
        return out.str();
    }

    inline std::string bytes(std::string_view value)
    {
        Hasher hasher;
        hasher.update(value);
        return hex(hasher.finish());
    }

    inline std::string file(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot hash file: " + path.string());
        Hasher hasher;
        std::array<char, 1 << 16> buffer{};
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) hasher.update(buffer.data(), static_cast<size_t>(count));
        }
        return hex(hasher.finish());
    }

    inline std::string normalize(std::string value)
    {
        if (value.rfind("sha256:", 0) == 0) value.erase(0, 7);
        for (char& c : value)
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        return value;
    }
} // namespace zelph::network::sha256
