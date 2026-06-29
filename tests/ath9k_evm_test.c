/* Host unit test for the ath9k EVM helper. Build/run:
 *   gcc -O2 -Wall -o /tmp/ath9k_evm_test tests/ath9k_evm_test.c && /tmp/ath9k_evm_test
 * The helper below is the canonical source; it is copied verbatim into
 * patches/mac80211/999-ath9k-radiotap-evm.patch (ath9k/common.c hunk). */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

typedef uint8_t u8;
typedef uint32_t u32;

static u8 ath9k_cmn_evm_db(u32 e0, u32 e1, u32 e2, u32 e3, u32 e4)
{
	u32 words[4] = { e0, e1, e2, e3 };
	u32 sum = 0, n = 0;
	int w, b;

	for (w = 0; w < 4; w++)
		for (b = 0; b < 4; b++) {
			u8 v = (words[w] >> (8 * b)) & 0xff;

			if (v == 0 || v == 0x80)
				continue;	/* empty / not measured */
			if (v > 0x80)
				return 0;	/* failed marker -> drop frame */
			sum += v;
			n++;
		}
	for (b = 0; b < 2; b++) {		/* evm4: low 2 bytes only */
		u8 v = (e4 >> (8 * b)) & 0xff;

		if (v == 0 || v == 0x80)
			continue;
		if (v > 0x80)
			return 0;
		sum += v;
		n++;
	}

	if (n < 3)
		return 0;			/* too few pilots -> absent */

	return (u8)(sum / n);
}

/* pack 4 bytes little-endian into a descriptor word */
static u32 W(u8 a, u8 b, u8 c, u8 d) { return a | (b << 8) | (c << 16) | ((u32)d << 24); }

int main(void)
{
	/* legacy frame: all 0x80 -> no valid pilots -> absent */
	assert(ath9k_cmn_evm_db(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 0);

	/* control frame (short): 4 clean pilots [15,19,22,23] in evm0 -> 19 dB */
	assert(ath9k_cmn_evm_db(W(15,19,22,23), 0x00000000u, 0x00000000u, 0x80808080u, 0x8080u) == 19);

	/* video frame (long): a failed marker (>0x80) anywhere drops the whole frame.
	 * 0xfe in evm0 -> absent, regardless of the rest. */
	assert(ath9k_cmn_evm_db(W(0xfe,6,0xff,0xfe), W(0xff,0xfe,0,9), 0u, 0x80808080u, 0x8080u) == 0);

	/* failed marker in a LATER word still drops the frame even after clean pilots:
	 * evm0 clean [15,19,22,23], then 0xff in evm1 -> absent */
	assert(ath9k_cmn_evm_db(W(15,19,22,23), W(0xff,0,0,0), 0u, 0x80808080u, 0x8080u) == 0);

	/* too few pilots: only 2 valid -> absent (>=3 required) */
	assert(ath9k_cmn_evm_db(W(15,19,0x80,0x80), 0x00000000u, 0x00000000u, 0x80808080u, 0x8080u) == 0);

	/* skip 0x00 holes: [15,0,19,22] -> 3 valid -> (15+19+22)/3 = 18 dB */
	assert(ath9k_cmn_evm_db(W(15,0,19,22), 0x00000000u, 0x00000000u, 0x80808080u, 0x8080u) == 18);

	/* evm4 contributes its low 2 bytes: evm0 [15,19] + evm4 [22,23] -> 4 valid -> 19 dB */
	assert(ath9k_cmn_evm_db(W(15,19,0x80,0x80), 0x00000000u, 0x00000000u, 0x80808080u, W(22,23,0,0)) == 19);

	printf("ath9k_evm_test: all passed\n");
	return 0;
}
