"""Wraps raw XMA2 packets (2048 bytes each, as the game gives them to the XMA hardware) in a RIFF/WAVE
file that FFmpeg's xma2 decoder reads (format tag 0x166 with the 34-byte XMA2WAVEFORMATEX tail), so that an
independent decoder can be run on exactly what a context was fed.

   python -I xma2_riff.py <in.xma> <rate> <channels> <out.wav>      (builds the container only)

XMA2WAVEFORMATEX (little endian in a PC .wav; the console's .xma files carry the same fields big endian):
   WORD  NumStreams, DWORD ChannelMask, DWORD SamplesEncoded, DWORD BytesPerBlock, DWORD PlayBegin,
   DWORD PlayLength, DWORD LoopBegin, DWORD LoopLength, BYTE LoopCount, BYTE EncoderVersion, WORD BlockCount
"""
import struct
import sys

PACKET = 2048


def build(packets: bytes, rate: int, channels: int, samples_encoded: int = 0) -> bytes:
    if len(packets) % PACKET:
        packets = packets + b"\0" * (PACKET - len(packets) % PACKET)
    n_packets = len(packets) // PACKET
    streams = (channels + 1) // 2
    channel_mask = 0x3 if channels == 2 else (0x4 if channels == 1 else (1 << channels) - 1)
    block_count = 1
    # one block holding every packet: BytesPerBlock is the block size in bytes (24 bits in the console's format)
    bytes_per_block = len(packets)
    extra = struct.pack("<HIIIIIIIBBH", streams, channel_mask, samples_encoded, bytes_per_block, 0,
                        samples_encoded, 0, 0, 0, 4, block_count)
    assert len(extra) == 34, len(extra)
    block_align = PACKET
    fmt = struct.pack("<HHIIHHH", 0x166, channels, rate, rate * channels * 2, block_align, 16, len(extra)) + extra
    body = b"WAVE"
    body += b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(packets)) + packets
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main():
    src, rate, channels, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    with open(src, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        f.write(build(data, rate, channels))
    print(f"{len(data) // PACKET} packets -> {dst}")


if __name__ == "__main__":
    main()
