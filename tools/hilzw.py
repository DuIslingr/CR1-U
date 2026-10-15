"""HI Corporation LZW decoder (reimplementation of HI_LZW_DecodeByte).

The format matches GIF-style LZW:
  byte 0         : minimum code size N (2..11)
  then sub-blocks: [len u8][len bytes] ... terminated by a 0-length block
  codes are read LSB-first, start at N+1 bits, grow to a max of 12 bits;
  clear = 1<<N, end = clear+1.
"""


def _subblocks(buf, pos):
    """Concatenate GIF-style sub-blocks starting at pos. Returns (data, end_pos)."""
    out = bytearray()
    while pos < len(buf):
        n = buf[pos]
        pos += 1
        if n == 0:
            break
        out += buf[pos:pos + n]
        pos += n
    return bytes(out), pos


def decode(buf, pos=0):
    """Decode an LZW stream at buf[pos]. Returns (decoded_bytes, end_pos)."""
    min_size = buf[pos]
    if not 2 <= min_size <= 11:
        raise ValueError(f"bad LZW min code size {min_size} at {pos:#x}")
    data, end = _subblocks(buf, pos + 1)

    clear = 1 << min_size
    eoi = clear + 1
    out = bytearray()
    bitpos = 0
    nbits = len(data) * 8
    size = min_size + 1
    dict_ = [bytes([i]) for i in range(clear)] + [b"", b""]
    prev = None

    while bitpos + size <= nbits:
        # LSB-first code read
        code = 0
        for i in range(size):
            byte = data[(bitpos + i) >> 3]
            code |= ((byte >> ((bitpos + i) & 7)) & 1) << i
        bitpos += size

        if code == clear:
            dict_ = dict_[:clear + 2]
            size = min_size + 1
            prev = None
            continue
        if code == eoi:
            break

        if code < len(dict_):
            entry = dict_[code]
            if prev is not None and len(dict_) < 4096:
                dict_.append(prev + entry[:1])
        elif prev is not None:
            entry = prev + prev[:1]
            dict_.append(entry)
        else:
            raise ValueError(f"invalid LZW code {code} with empty prefix")

        out += entry
        prev = entry
        if len(dict_) == (1 << size) and size < 12:
            size += 1

    return bytes(out), end
