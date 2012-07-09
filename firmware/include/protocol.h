/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker common protocol definitions
 *
 * This file is part of the internal implementation of the Sifteo SDK.
 * Confidential, not for redistribution.
 *
 * M. Elizabeth Scott <beth@sifteo.com>
 * Copyright <c> 2011 Sifteo, Inc. All rights reserved.
 */

#ifndef _TC_PROTOCOL_H
#define _TC_PROTOCOL_H

#include <stdint.h>


/**************************************************************************
 *
 * Master -> Cube (RF) packet format
 *
 *   Background
 *  ------------
 *
 * This particular packet format is by far the most critical form of
 * compression for us to optimize during gameplay. This protocol forms
 * the link between our virtual framebuffer on the master, and a
 * cube's actual framebuffer. It needs to be tuned for transmitting
 * tile index data very efficiently.
 *
 * Tile indices are tricky, because they're relatively big 14-bit
 * values. If we had memory for a huffman table or a big LUT, we could
 * reduce the size of our codes considerably by taking advantage of
 * the fact that we're only using a small subset of the tile space at
 * once.
 *
 * But this is impractical, because of memory limitations on both
 * sides, and because we want this protocol to be very fast to
 * encode. So, instead, we take advantage of another properties that
 * these tile indices have:
 *
 *    Tile indices are often numerically close to tiles that are
 *    physically close.
 *
 * This property means that we can get great ratios with coders that
 * support adding a delta to some previous index that we find in a
 * window buffer. Unfortunately, a typical LZ77-style window buffer
 * would take a nontrivial amount of RAM in the decoder, and the
 * encoder would need to spend a lot of CPU searching for occurrances
 * of values in the window.
 *
 * After much experimentation (see attic/map-codec) I think I found a
 * good compromise. This is a codec whose "window" consists of four
 * carefully chosen sample points in the destination buffer: (X is the
 * tile being compressed)
 *
 *   . . . . .
 *   . 3 2 . .
 *   1 0 X . .
 *   . . . . .
 *
 * We can encode copies or small deltas from these sample points very
 * efficiently, with an RLE layer which handles simple horizontal and
 * vertical runs, as well as sequences of stepped tile indices.
 *
 * The specific wire encoding is inspired by a few other algorithms.
 * In particular, the unary-encoded prefixes in Golomb-Rice codes, and
 * of course Huffman encoding's strategy of assigning shorter bit
 * streams to more frequent codes.
 *
 * I also wanted the wire encoding to be very efficient to decode on
 * an 8-bit MCU that has no barrel shifter, and I wanted to have
 * additional codes which support random-access to VRAM, as well as
 * 'escapes' into other communication modes, such as writing to the
 * Flash FIFO.
 *
 *   VRAM
 *  ----------
 *
 * The buffer we're writing to in this codec is termed VRAM, since
 * it's used as the source for all video rendering operations. The
 * exact format is specific to the video mode in use, but typically it
 * consists of 14-bit tile index data, arranged in grids with a stride
 * of 18 tiles:
 *
 *    llllllll0 hhhhhhhh0
 *
 * Where 'l' is the least significant 7 bits, and 'h' is the most
 * significant 7 bits. This layout matches the address latch format
 * used by the cube microcontroller. The LSBs must be zero, or display
 * corruption will result.
 *
 * It is possible to store other data in VRAM too. Depending on the
 * video mode, it may be useful to store full-range 8-bit values.  The
 * protocol supports this, but it isn't what we're optimizing for.
 *
 * Since this codec doesn't know about the specific video mode in use,
 * we always assume a fixed stride, even if the particular mode might
 * be using a different stride in parts of the buffer. This means that
 * our sampling points can be defined as a constant four-entry lookup
 * table to negative offsets in VRAM.
 *
 *   Encoding
 *  ----------
 *
 * The protocol uses a stream of 4-bit nybbles. Everything is nybble
 * aligned, but there is no guaranteed byte-alignment except where
 * specified. Bytes are encoded with the least significant nybble
 * first (Just like in the flash codec's RLE4). Order of bits within a
 * code field may vary according to the code, see bit orders (O:)
 * below.
 *
 * The protocol is stateful. In a maximally-long RF packet, the stream
 * continues, preserving all state, at the beginning of the next packet.
 * A single code may be split across the packet boundary.
 *
 * In case of a non-max-length RF packet, the decoder state is reset
 * at the end of the packet. This is designed to provide a way for the
 * decoder to re-synchronize in case of a transient error. If such
 * a packet ends with a partial code, that partial code is ignored.
 *
 *  I. Primary codes
 *
 * A primary code is one that can be operated on by subsequent
 * 'repeat' codes. There are two state variables associated with
 * primary codes: a two-bit sample number (S), and a 4-bit diff
 * (D). The operation of "writing one delta-word" means to perform a
 * new sample lookup from sample point S, add (D-7) to that sample,
 * write it to the current VRAM location as a 14-bit tile index, then
 * to advance the VRAM location by two bytes.
 *
 *   01ss                  Copy sample #s.
 *                         Sets S=s, D=0, write one delta-word.
 *
 *   10ss dddd             Diff against sample #s, in the range [-7, 8]
 *                         Sets S=s, D=d, write one delta-word.
 *
 *   11xx xxxx xxxx xxxx   Literal 14-bit index.
 *  O: fe 4321 9765 dcba   Writes the index to VRAM, then sets S=0, D=0.
 *
 *  II. Special codes
 *
 * These codes are encoded in a slightly more complex way. First of
 * all, we have a reserved region of the diff codes, which are
 * redundant encodings for the 4-bit 'copy' code:
 *
 *   1000 0111             Sensor timer sync escape
 *
 *   1001 0111             Reserved for future use
 *   1010 0111             Reserved for future use
 *   1011 0111             Reserved for future use
 *
 * Next, the RLE codes. These begin with a 4-bit code that repeats the
 * last primary code. However, consecutive copies of this repeat code
 * are not allowed. Instead, this is used to support additional
 * special code types:
 *
 *   00nn                               Write n+1 delta-words
 *
 *   000n 00nn                          Skip n+1 output words
 *      0   21
 *
 *   0010 00nn nnnn                     Write n+5 delta-words
 *          54 3210
 *
 *   0011 000x xxxx xxxx                Set write address to literal 9-bit word index
 *           9 4321 8765 
 *
 *   0011 0010 xxxx xxxx xxxx xxxx      Write a literal 16-bit word
 *             3210 7654 ba98 fedc      Writes the index to VRAM, then sets S=0, D=0.
 *
 *   0011 0011                          Escape to flash mode
 *
 * Note that the single-nybble RLE code is technically defined as the
 * above nybble plus any non-RLE subsequent nybble. The runs will not
 * actually be emitted until just before that subsequent nybble.
 *
 *  III. Flash escape
 *
 * The flash escape warrants further discussion. If this escape code
 * is seen anywhere in the compression stream, we ignore bits until
 * the next byte boundary, then the remainder of the packet is sent to
 * the flash FIFO. If we would have sent zero bytes to the FIFO (zero
 * or one nybbles between the escape code and the end of the packet)
 * we instead trigger a reset of the flash decoder state machine.
 *
 * Resets happen asynchronously, and are acknowledged by a one-byte
 * increment in the flash decoder's progress counter. No data should
 * be written to the decoder until the reset has been acknowledged.
 *
 *  IV. Sensor timer sync escape
 *
 * Like the Flash Escape, this switches from nybble mode to byte mode,
 * and consumes the remainder of the packet. In this case, only two
 * bytes are read, and the rest of the packet, if any, is discarded.
 *
 * These two bytes are used as a reload value for the master sensor
 * clock, which is momentarily stopped and restarted.
 */

#define RF_VRAM_MAX_RUN    (0x3F + 5)
#define RF_VRAM_DIFF_BASE  7

#define RF_VRAM_STRIDE     18

#define RF_VRAM_SAMPLE_0   1
#define RF_VRAM_SAMPLE_1   2
#define RF_VRAM_SAMPLE_2   (RF_VRAM_STRIDE)
#define RF_VRAM_SAMPLE_3   (RF_VRAM_STRIDE + 1)


/**************************************************************************
 *
 * Cube -> Master (ACK) packet format
 *
 * The overall ACK structure (RF_ACKType) defines all possible data
 * that we may send back in an ACK response. Any bytes at the end of
 * the packet which were unchanged from the previous ACK may be
 * omitted. In the most trivial case, if nothing at all has changed,
 * we'll let the hardware send back an empty ACK.
 *
 * The full ACK packet may be requested at any time by sending a flash
 * decoder state machine reset code. (See above).
 */

// Lengths of each type of ACK packet, in bytes
#define RF_ACK_LEN_EMPTY        0
#define RF_ACK_LEN_FRAME        1
#define RF_ACK_LEN_ACCEL        4
#define RF_ACK_LEN_NEIGHBOR     8
#define RF_ACK_LEN_FLASH_FIFO   9
#define RF_ACK_LEN_BATTERY_V    11
#define RF_ACK_LEN_HWID         19
#define RF_ACK_LEN_MAX          RF_ACK_LEN_HWID

// Struct offsets. Matches the C code before, but intended for inline assembly use
#define RF_ACK_FRAME            0
#define RF_ACK_ACCEL            1
#define RF_ACK_NEIGHBOR         4
#define RF_ACK_FLASH_FIFO       8
#define RF_ACK_BATTERY_V        9
#define RF_ACK_HWID             11

#define HWID_LEN                8

#define NB_ID_MASK              0x1F    // ID portion of neighbor bytes
#define NB_FLAG_SIDE_ACTIVE     0x80    // There's a cube neighbored on this side
#define NB0_FLAG_TOUCH          0x40    // In neighbors[0], toggles when touch is detected

#define FRAME_ACK_CONTINUOUS    0x40
#define FRAME_ACK_COUNT         0x3F
#define FRAME_ACK_TOGGLE        0x01

typedef union {
    uint8_t bytes[RF_ACK_LEN_MAX];
    struct {
        /*
         * For synchronizing LCD refreshes, the master can keep track
         * of how many repaints the cube has performed. Ideally these
         * repaints would be in turn sync'ed with the LCDC's hardware
         * refresh timer.
         *
         * The various bits in here are multi-purpose...
         *
         *   [7]    Query response bit. Normally zero. If set, this
         *          is NOT a normal ACK packet, it's a response to
         *          a particular kind of query command. Bits [6:0] are
         *          then used for an ID which corresponds to the
         *          query this response is paired with. The rest of the
         *          packet's contents are interpreted in a query-specific
         *          way.
         *
         *   [6]    Continuous rendering bit, captured synchronously
         *          with the firmware's read of this bit at the top of
         *          the render loop.
         *
         *   [5:0]  Count of total frames rendered, modulo 64.
         *
         *   [0]    Doubles as a captured copy of the last read of the
         *          toggle bit.
         */
        uint8_t frame_count;

        // Signed 8-bit analog accelerometer data (x, y, z)
        int8_t accel[3];

        // Neighbor cube IDs in low bits, flags in upper bits
        uint8_t neighbors[4];

        /*
         * Number of bytes processed by the flash decoder so
         * far. Increments and wraps around, never decrements or
         * resets. Also increments once on a flash reset completion.
         */
        uint8_t flash_fifo_bytes;

        // Current raw battery voltage level (16-bit little endian)
        uint8_t battery_v[2];

        // Unique hardware ID
        uint8_t hwid[HWID_LEN];
    };
} RF_ACKType;

/*
 * Subset of the ACK packet that we keep in memory on the cubes
 */

#define RF_MEM_ACK_LEN      RF_ACK_LEN_BATTERY_V

typedef struct {
    // In sync with RF_ACKType
    uint8_t frame_count;
    int8_t accel[3];
    uint8_t neighbors[4];
    uint8_t flash_fifo_bytes;
    uint16_t battery_v[2];
} RF_MemACKType;


/**************************************************************************
 *
 * Flash Loadstream codec.
 *
 * The flash loader is a state machine that runs in the main "thread"
 * of the cube firmware, alternately with our graphics rendering
 * engine. It receives commands in a different format, via a FIFO that
 * can be written to via the RF protocol above.
 *
 * The flash loader operates on 8-bit opcodes, most of which support
 * RLE encoding. These opcodes can decode a number of tiles using a
 * particular codec, or they can program the color LUT. In combination
 * with the optimizers in STIR, this gives many of the benefits of
 * indexed color plus an LZ78-style dictionary. So it's a little bit
 * like GIF, but variable-bit-depth and very low-memory.
 *
 * This format also includes a few special codes, for changing write
 * address and doing block erases. There is also quite a lot of
 * reserved code-space, so this would be a plentiful area to add
 * expansion to the protocol if necessary.
 *
 * The decoder will automatically erase a flash block any time the first
 * tile in a block is about to be programmed.
 */

#define FLS_BLOCK_SIZE          (64*1024)   // Size of flash erase blocks

#define FLS_FIFO_SIZE           73      // Size of buffer between radio and flash decoder
#define FLS_FIFO_USABLE         72      // Usable space in FIFO (SIZE-1)

#define FLS_LUT_SIZE            16      // Size of persistent color LUT used by RLE encodings

#define FLS_OP_MASK             0xe0    // Upper 3 bits are an opcode
#define FLS_ARG_MASK            0x1f    // Lower 5 bits are an argument (usually a repeat count)

#define FLS_OP_LUT1             0x00    // Single color 16-bit LUT entry (argument is index)
#define FLS_OP_LUT16            0x20    // Up to 16 LUT entries follow a 16-bit vector of indices
#define FLS_OP_TILE_P0          0x40    // One trivial solid-color tile (arg = color index)
#define FLS_OP_TILE_P1_R4       0x60    // Tiles with 1-bit pixels and 4-bit RLE encoding (arg = count-1)
#define FLS_OP_TILE_P2_R4       0x80    // Tiles with 2-bit pixels and 4-bit RLE encoding (arg = count-1)
#define FLS_OP_TILE_P4_R4       0xa0    // Tiles with 4-bit pixels and 4-bit RLE encoding (arg = count-1)
#define FLS_OP_TILE_P16         0xc0    // Tile with 16-bit pixels and 8-bit repetition mask (arg = count-1)
#define FLS_OP_SPECIAL          0xe0    // Special symbols (below)

#define FLS_OP_NOP              0xe0    // Permanently reserved as a no-op
#define FLS_OP_ADDRESS          0xe1    // Followed by a 2-byte (lat1:lat2) tile address. A21 in LSB of lat2.
#define FLS_OP_QUERY_CHECKSUM   0xe2    // Args: (queryID, numSamples-1, stride-1)

// From 0xe3 to 0xff are all reserved codes currently

/*
 * Minimum operand sizes for various opcodes:
 *
 * In order to support buffered flash programming, we need a quick way
 * to check whether enough data is buffered in order to send an entire
 * decompressed block of 16 pixels to the flash chip at once. So, the
 * nontrivial tile opcodes have a minimum number of buffered bytes requred,
 * corresponding to the worst-case data size of 16 pixels.
 */

#define FLS_MIN_TILE_R4        12       // 4-bpp RLE with worst-case run count overhead
#define FLS_MIN_TILE_P16       34       // 16 pixels, plus bitmap overhead

#endif
