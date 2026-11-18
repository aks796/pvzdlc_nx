/* rt_vorbis.h: our stb_vorbis (v1.22, PvZ-Portable's copy, 64-bit positions), with every public symbol
 * renamed rtv_*: libopenmpt links its own, older stb_vorbis for MO3 samples and the two aren't ABI
 * compatible. rt_vorbis.c defines RT_VORBIS_IMPL to compile the decoder itself. */
#ifndef RT_VORBIS_H
#define RT_VORBIS_H
#define stb_vorbis_close rtv_close
#define stb_vorbis_decode_frame_pushdata rtv_decode_frame_pushdata
#define stb_vorbis_decode_memory rtv_decode_memory
#define stb_vorbis_flush_pushdata rtv_flush_pushdata
#define stb_vorbis_get_comment rtv_get_comment
#define stb_vorbis_get_error rtv_get_error
#define stb_vorbis_get_file_offset rtv_get_file_offset
#define stb_vorbis_get_frame_float rtv_get_frame_float
#define stb_vorbis_get_frame_short rtv_get_frame_short
#define stb_vorbis_get_frame_short_interleaved rtv_get_frame_short_interleaved
#define stb_vorbis_get_info rtv_get_info
#define stb_vorbis_get_playback_sample_offset rtv_get_playback_sample_offset
#define stb_vorbis_get_sample_offset rtv_get_sample_offset
#define stb_vorbis_get_samples_float rtv_get_samples_float
#define stb_vorbis_get_samples_float_interleaved rtv_get_samples_float_interleaved
#define stb_vorbis_get_samples_short rtv_get_samples_short
#define stb_vorbis_get_samples_short_interleaved rtv_get_samples_short_interleaved
#define stb_vorbis_open_memory rtv_open_memory
#define stb_vorbis_open_pushdata rtv_open_pushdata
#define stb_vorbis_seek rtv_seek
#define stb_vorbis_seek_frame rtv_seek_frame
#define stb_vorbis_seek_start rtv_seek_start
#define stb_vorbis_stream_length_in_samples rtv_stream_length_in_samples
#define stb_vorbis_stream_length_in_seconds rtv_stream_length_in_seconds
#define STB_VORBIS_NO_STDIO
#ifndef RT_VORBIS_IMPL
#define STB_VORBIS_HEADER_ONLY
#endif
#include "../third_party/stb_vorbis/stb_vorbis.h"
#endif
