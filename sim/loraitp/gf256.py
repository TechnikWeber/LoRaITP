"""
Reed-Solomon erasure coding over GF(256).  SPEC.md 5.2.

The generator is systematic, [I ; C], with C a Cauchy matrix. Cauchy is
used rather than Vandermonde because every k x k submatrix of [I ; C] is
guaranteed invertible, which is exactly the property erasure decoding
needs: any k of the k + r frames must reconstruct the block, and we do
not get to choose which k arrive.

Because the PHY CRC turns a corrupted frame into a missing frame, we
only ever face erasures at known positions - never errors at unknown
ones. That is a much easier problem, and it is why r parity chunks
recover exactly r losses.

Pure Python and not fast: inverting a k x k matrix is O(k^3) field
operations. Fine for k in the tens, slow for k = 128. The simulator is
for correctness, not throughput.
"""

_PRIM = 0x11D  # x^8 + x^4 + x^3 + x^2 + 1, the conventional choice

_EXP = [0] * 512
_LOG = [0] * 256

def _build_tables():
    x = 1
    for i in range(255):
        _EXP[i] = x
        _LOG[x] = i
        x <<= 1
        if x & 0x100:
            x ^= _PRIM
    for i in range(255, 512):
        _EXP[i] = _EXP[i - 255]

_build_tables()


def mul(a, b):
    if a == 0 or b == 0:
        return 0
    return _EXP[_LOG[a] + _LOG[b]]


def div(a, b):
    if b == 0:
        raise ZeroDivisionError("GF(256) division by zero")
    if a == 0:
        return 0
    return _EXP[(_LOG[a] - _LOG[b]) % 255]


def inv(a):
    if a == 0:
        raise ZeroDivisionError("GF(256) inverse of zero")
    return _EXP[(255 - _LOG[a]) % 255]


def cauchy_matrix(rows, cols):
    """r x k Cauchy matrix over GF(256). Requires rows + cols <= 256."""
    if rows + cols > 256:
        raise ValueError("Cauchy matrix needs rows + cols <= 256; "
                         "this is the k + r <= 255 limit in SPEC.md 5.2")
    xs = list(range(rows))
    ys = list(range(rows, rows + cols))
    return [[inv(x ^ y) for y in ys] for x in xs]


def encode(data_chunks, n_parity):
    """
    data_chunks: list of k equal-length bytes objects.
    Returns n_parity parity chunks of the same length.
    """
    k = len(data_chunks)
    if n_parity == 0:
        return []
    length = len(data_chunks[0])
    if any(len(c) != length for c in data_chunks):
        raise ValueError("all chunks must be zero-padded to equal length")

    c = cauchy_matrix(n_parity, k)
    parity = []
    for row in c:
        out = bytearray(length)
        for j, coeff in enumerate(row):
            if coeff == 0:
                continue
            src = data_chunks[j]
            # Precompute the multiplication table for this coefficient;
            # it turns the inner loop into a lookup.
            lg = _LOG[coeff]
            table = bytes(0 if v == 0 else _EXP[_LOG[v] + lg]
                          for v in range(256))
            for i in range(length):
                out[i] ^= table[src[i]]
        parity.append(bytes(out))
    return parity


def _invert(matrix):
    """Gauss-Jordan inversion over GF(256)."""
    n = len(matrix)
    a = [list(row) + [1 if i == j else 0 for j in range(n)]
         for i, row in enumerate(matrix)]
    for col in range(n):
        pivot = next((r for r in range(col, n) if a[r][col] != 0), None)
        if pivot is None:
            raise ValueError("singular matrix - should be impossible for "
                             "a Cauchy generator; check the row indices")
        a[col], a[pivot] = a[pivot], a[col]
        p = inv(a[col][col])
        a[col] = [mul(v, p) for v in a[col]]
        for r in range(n):
            if r != col and a[r][col] != 0:
                f = a[r][col]
                a[r] = [v ^ mul(f, w) for v, w in zip(a[r], a[col])]
    return [row[n:] for row in a]


def decode(k, n_parity, received):
    """
    Reconstruct k data chunks.

    received: dict {index -> chunk}, where index 0..k-1 are source chunks
              and k..k+n_parity-1 are parity chunks.

    Returns the list of k data chunks, or None if fewer than k frames
    are present - which is the condition SPEC.md 5.3 makes the receiver
    test before it bothers to try.
    """
    if len(received) < k:
        return None
    if all(i in received for i in range(k)):
        return [received[i] for i in range(k)]

    c = cauchy_matrix(n_parity, k) if n_parity else []
    idx = sorted(received)[:k]

    rows = []
    for i in idx:
        if i < k:
            rows.append([1 if j == i else 0 for j in range(k)])
        else:
            rows.append(list(c[i - k]))

    inv_m = _invert(rows)
    length = len(received[idx[0]])
    out = []
    for row in inv_m:
        buf = bytearray(length)
        for pos, coeff in enumerate(row):
            if coeff == 0:
                continue
            src = received[idx[pos]]
            lg = _LOG[coeff]
            table = bytes(0 if v == 0 else _EXP[_LOG[v] + lg]
                          for v in range(256))
            for i in range(length):
                buf[i] ^= table[src[i]]
        out.append(bytes(buf))
    return out
