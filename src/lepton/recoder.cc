#include <time.h>
#include <stdio.h>
#include "uncompressed_components.hh"
#include "recoder.hh"
#include "bitops.hh"
int encode_block_seq( abitwriter* huffw, huffCodes* dctbl, huffCodes* actbl, short* block);
int next_mcupos( int* mcu, int* cmp, int* csc, int* sub, int* dpos, int* rstw );
const bool fast_exit = true;
extern int    jpgfilesize;            // size of JPEG file
extern std::atomic<int> errorlevel;
extern UncompressedComponents colldata; // baseline sorted DCT coefficients
extern componentInfo cmpnfo[ 4 ];
extern int jpegtype;
extern int cmpc; // component count
extern char padbit;
extern int scnc;   // count of scans
extern int rstc;
extern int grbs;   // size of garbage
extern int            hufs;   // size of huffman data
extern int            hdrs;   // size of header
extern unsigned short qtables[4][64];                // quantization tables
extern huffCodes      hcodes[2][4];                // huffman codes
extern huffTree       htrees[2][4];                // huffman decoding trees
extern unsigned char  htset[2][4];                    // 1 if huffman table is set
extern unsigned char* grbgdata;    // garbage data
extern unsigned char* hdrdata;   // header data
extern unsigned char* huffdata;   // huffman coded data
extern int            rsti;
extern int mcuv; // mcus per line
extern unsigned int mcuh; // mcus per collumn
extern int mcuc; // count of mcus
extern std::vector<unsigned char> rst_err;   // number of wrong-set RST markers per scan
extern std::vector<unsigned int> rst_cnt;
void check_decompression_memory_bound_ok();


extern int cs_cmpc; // component count in current scan
extern int cs_cmp[ 4 ]; // component numbers  in current scan

bool parse_jfif_jpg( unsigned char type, unsigned int len, unsigned char* segment );
#define B_SHORT(v1,v2)    ( ( ((int) v1) << 8 ) + ((int) v2) )

static bool aligned_memchr16ff(const unsigned char *local_huff_data) {
#if 1
    __m128i buf = _mm_load_si128((__m128i const*)local_huff_data);
    __m128i ff = _mm_set1_epi8(-1);
    __m128i res = _mm_cmpeq_epi8(buf, ff);
    uint32_t movmask = _mm_movemask_epi8(res);
    bool retval = movmask != 0x0;
    assert (retval == (memchr(local_huff_data, 0xff, 16) != NULL));
    return retval;
#endif
    return memchr(local_huff_data, 0xff, 16) != NULL;
}

void sync_jpeg_huffman(MergeJpegProgress *stored_progress,
                       bounded_iostream* str_out,
                       const unsigned char * local_huff_data,
                       unsigned int max_byte_coded,
                       bool flush) {
    MergeJpegProgress progress(stored_progress);

    //write a single scan
    {
        // write & expand huffman coded image data
        unsigned int progress_ipos = progress.ipos;
        const unsigned char mrk = 0xFF; // marker start
        const unsigned char stv = 0x00; // 0xFF stuff value
        for ( ; progress_ipos & 0xf; progress_ipos++ ) {
            if (__builtin_expect(!(progress_ipos < max_byte_coded), 0)) {
                break;
            }
            uint8_t byte_to_write = local_huff_data[progress_ipos];
            str_out->write_byte(byte_to_write);
            // check current byte, stuff if needed
            if (__builtin_expect(byte_to_write == 0xFF, 0))
                str_out->write_byte(stv);
        }

        while(true) {
            if (__builtin_expect(!(progress_ipos + 15 < max_byte_coded), 0)) {
                break;
            }
            if ( __builtin_expect(aligned_memchr16ff(local_huff_data + progress_ipos), 0)){
                // insert restart markers if needed
                for (int veci = 0 ; veci < 16; ++veci, ++progress_ipos ) {
                    uint8_t byte_to_write = local_huff_data[progress_ipos];
                    str_out->write_byte(byte_to_write);
                    // check current byte, stuff if needed
                    if (__builtin_expect(byte_to_write == 0xFF, 0)) {
                        str_out->write_byte(stv);
                    }
                }
            } else {
                str_out->write(local_huff_data + progress_ipos, 16);
                progress_ipos+=16;
            }
        }
        for ( ; ; progress_ipos++ ) {
            if (__builtin_expect(!(progress_ipos < max_byte_coded), 0)) {
                break;
            }
            uint8_t byte_to_write = local_huff_data[progress_ipos];
            str_out->write_byte(byte_to_write);
            // check current byte, stuff if needed
            if (__builtin_expect(byte_to_write == 0xFF, 0))
                str_out->write_byte(stv);
        }
        progress.ipos = progress_ipos;
    }
}



/* -----------------------------------------------
    JPEG encoding routine
    ----------------------------------------------- */
bool recode_baseline_jpeg(bounded_iostream*str_out,
                          int max_file_size)
{
    abitwriter*  huffw; // bitwise writer for image data
    abytewriter* storw; // bytewise writer for storage of correction bits

    unsigned char  type = 0x00; // type of current marker segment
    unsigned int   len  = 0; // length of current marker segment
    const unsigned char mrk = 0xFF;

    int lastdc[ 4 ]; // last dc for each component
    Sirikata::Aligned256Array1d<int16_t, 64> block; // store block for coeffs
    unsigned int eobrun; // run of eobs
    int rstw; // restart wait counter

    int cmp, bpos, dpos;
    int mcu, sub, csc;
    int eob, sta;
    int tmp;
    int ABIT_WRITER_PRELOAD = 4096 * 1024 + 1024;
    // open huffman coded image data in abitwriter
    huffw = new abitwriter( ABIT_WRITER_PRELOAD, max_file_size);
    huffw->fillbit = padbit;

    // init storage writer
    storw = new abytewriter( ABIT_WRITER_PRELOAD);

    // preset count of scans and restarts
    scnc = 0;
    rstc = 0;
    MergeJpegProgress streaming_progress;
    assert (streaming_progress.ipos == 0
            && streaming_progress.hpos == 0
            && streaming_progress.scan == 1
            && streaming_progress.within_scan == false);
    str_out->set_bound(max_file_size - grbs);
    {
        unsigned char SOI[ 2 ] = { 0xFF, 0xD8 }; // SOI segment
        // write SOI
        str_out->write( SOI, 2 );
    }

    // JPEG decompression loop
    while ( true )
    {
        uint32_t hpos_start = streaming_progress.hpos;
        // seek till start-of-scan, parse only DHT, DRI and SOS
        for ( type = 0x00; type != 0xDA; ) {
            if ( ( int ) streaming_progress.hpos >= hdrs ) break;
            type = hdrdata[ streaming_progress.hpos + 1 ];
            len = 2 + B_SHORT( hdrdata[ streaming_progress.hpos + 2 ], hdrdata[ streaming_progress.hpos + 3 ] );
            if ( ( type == 0xC4 ) || ( type == 0xDA ) || ( type == 0xDD ) ) {
                if ( !parse_jfif_jpg( type, len, &( hdrdata[ streaming_progress.hpos ] ) ) ) {
                    return false;
                }
                int max_scan = 0;
                for (int i = 0; i < cmpc; ++i) {
                    max_scan = std::max(max_scan, cmpnfo[i].bcv);
                }
                streaming_progress.hpos += len;
            }
            else {
                streaming_progress.hpos += len;
                continue;
            }
        }
        str_out->write(hdrdata + hpos_start, (streaming_progress.hpos - hpos_start));
        // get out if last marker segment type was not SOS
        if ( type != 0xDA ) break;


        // (re)alloc restart marker positons array if needed
        if ( rsti > 0 ) {
            tmp = rstc + ( ( cs_cmpc > 1 ) ?
                ( mcuc / rsti ) : ( cmpnfo[ cs_cmp[ 0 ] ].bc / rsti ) );
        }

        // intial variables set for encoding
        cmp  = cs_cmp[ 0 ];
        csc  = 0;
        mcu  = 0;
        sub  = 0;
        dpos = 0;

        // store scan position
        if (jpegtype != 1) {
            // unreachable: we let this image through the encoder with baseline markers
            custom_exit(ExitCode::PROGRESSIVE_UNSUPPORTED);
        }
        if (cs_cmpc != colldata.get_num_components()) {
            // unreachable: we let this image through the encoder with baseline markers
            custom_exit(ExitCode::PROGRESSIVE_UNSUPPORTED);
        }
        // JPEG imagedata encoding routines
        while ( true )
        {
            // (re)set last DCs for diff coding
            lastdc[ 0 ] = 0;
            lastdc[ 1 ] = 0;
            lastdc[ 2 ] = 0;
            lastdc[ 3 ] = 0;

            // (re)set status
            sta = 0;

            // (re)set eobrun
            eobrun = 0;

            // (re)set rst wait counter
            rstw = rsti;
            // ---> sequential interleaved encoding <---
            while ( sta == 0 ) {
                // copy from colldata
                const AlignedBlock &aligned_block = colldata.block((BlockType)cmp, dpos);
                //fprintf(stderr, "Reading from cmp(%d) dpos %d\n", cmp, dpos);
                for ( bpos = 0; bpos < 64; bpos++ ) {
                    block[bpos] = aligned_block.coefficients_zigzag(bpos);
                }
                int16_t dc = block[0];
                // diff coding for dc
                block[ 0 ] -= lastdc[ cmp ];
                lastdc[ cmp ] = dc;
                
                // encode block
                eob = encode_block_seq( huffw,
                                        &(hcodes[ 0 ][ cmpnfo[cmp].huffdc ]),
                                        &(hcodes[ 1 ][ cmpnfo[cmp].huffac ]),
                                        block.begin() );

                // check for errors, proceed if no error encountered
                if ( eob < 0 ) sta = -1;
                else {
                    int test_cmp = cmp;
                    int test_dpos = dpos;
                    int test_rstw = rstw;
                    sta = next_mcupos( &mcu, &cmp, &csc, &sub, &dpos, &rstw );
/*
                    if (cmpc == 1) { // lets make sure we can use the original in the noninterleaved case
                        int test_sta = next_mcuposn( &test_cmp, &test_dpos, &test_rstw );
                        assert(test_sta == sta);
                        assert(test_cmp == cmp);
                        assert(test_dpos == dpos);
                        assert(test_rstw == rstw);
                    }
*/
                }
                if (sta == 0 && huffw->no_remainder()) {
                    sync_jpeg_huffman(&streaming_progress, str_out, huffw->peekptr(), huffw->getpos(), false);
                }
                if (str_out->has_reached_bound()) {
                    sta = 2;
                }
            }

            // pad huffman writer
            huffw->pad( padbit );
            if (huffw->no_remainder()) {
                sync_jpeg_huffman(&streaming_progress, str_out, huffw->peekptr(), huffw->getpos(), false);
            }
            // evaluate status
            if ( sta == -1 ) { // status -1 means error
                fprintf( stderr, "encode error in scan%i / mcu%i",
                    scnc, ( cs_cmpc > 1 ) ? mcu : dpos );
                delete huffw;
                errorlevel.store(2);
                return false;
            }
            else if ( sta == 2 ) { // status 2 means done
                scnc++; // increment scan counter
                break; // leave decoding loop, everything is done here
            }
            else if ( sta == 1 ) { // status 1 means restart
                if ( rsti > 0 ) {
                    const unsigned char rst = 0xD0 + ( streaming_progress.cpos & 7);
                    str_out->write_byte(mrk);
                    str_out->write_byte(rst);
                    streaming_progress.rpos++;
                    streaming_progress.cpos++;
                    ++streaming_progress.num_rst_markers_this_scan;
                }
            }
            assert(huffw->no_remainder() && "this should have been padded");
        }
        // insert false rst markers at end if needed
        if (streaming_progress.scan - 1 < rst_err.size()) {
            while ( rst_err[streaming_progress.scan - 1 ] > 0 ) {
                const unsigned char rst = 0xD0 + (streaming_progress.cpos & 7 );
                str_out->write_byte(mrk);
                str_out->write_byte(rst);
                streaming_progress.cpos++;    rst_err[streaming_progress.scan - 1 ]--;
            }
        }
        streaming_progress.num_rst_markers_this_scan = 0;
        streaming_progress.within_scan = false;
        // proceed with next scan
        streaming_progress.scan++;
        if(str_out->has_reached_bound()) {
            check_decompression_memory_bound_ok();
            break;
        } 
    }

    // safety check for error in huffwriter
    if ( huffw->error ) {
        custom_exit(ExitCode::OOM);
    }

    // get data into huffdata
    huffdata = huffw->getptr();
    hufs = huffw->getpos();
    assert(huffw->no_remainder() && "this should have been padded");
    sync_jpeg_huffman(&streaming_progress, str_out, huffw->peekptr(), huffw->getpos(), true);

    // write EOI (now EOI is stored in garbage of at least 2 bytes)
    // this guarantees that we can stop the write in time.
    // if it used too much memory
    // str_out->write( EOI, 1, 2 );
    str_out->set_bound(max_file_size);
    check_decompression_memory_bound_ok();
    // write garbage if needed
    if ( grbs > 0 )
        str_out->write( grbgdata, grbs );
    check_decompression_memory_bound_ok();
    str_out->flush();

    // errormessage if write error
    if ( str_out->chkerr() ) {
        fprintf( stderr, "write error, possibly drive is full" );
        errorlevel.store(2);
        return false;
    }
    // get filesize
    if (!fast_exit) {
        delete huffw;

        // remove storage writer
        delete storw;
    }

    jpgfilesize = str_out->getsize();
    return true;
}
