/* * Copyright (c) 2016-2017 Haggai Eran, Gabi Malka, Lior Zeno, Maroun Tork
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <ntl/axi_data.hpp>
#include <ntl/constexpr.hpp>
#include <hls_stream.h>

namespace ntl {
    /** Adds a suffix onto an existing stream */
    template <unsigned suffix_length_bytes, unsigned log_suffix_alignment = 2>
    class push_suffix
    {
    public:
        typedef ap_uint<suffix_length_bytes * 8> suffix_t;
        static const int data_bits = axi_data::data_bits;
        static const int data_bytes = axi_data::data_bytes;
        typedef ap_uint<ntl::log2(data_bytes) + 1> tkeep_count_t;

        push_suffix() : state(IDLE) {}

        template <typename DataStream>
        void reorder(DataStream& data_in,
                     hls::stream<bool>& empty_packet,
                     hls::stream<bool>& enable_stream,
                     hls::stream<suffix_t>& suffix_in, DataStream& out)
        {
    #pragma HLS pipeline enable_flush
            static_assert(suffix_length_bytes < data_bytes, "suffix size too large - not implemented");

            switch (state)
            {
            case IDLE:
                if (empty_packet.empty() || enable_stream.empty())
                    break;

                empty = empty_packet.read();
                enable = enable_stream.read();

                if (!enable)
                    state = empty ? IDLE : DATA;
                else
                    state = READ_SUFFIX;
                break;

            case READ_SUFFIX:
                if (suffix_in.empty())
                    break;

                suffix = suffix_in.read();

                if (empty) {
                    last_flit_bytes = suffix_length_bytes;
                    state = LAST;
                } else {
                    // We have data
                    state = DATA;
                }
                break;

            case DATA: {
                if (data_in.empty() || out.full())
                    break;

                ntl::axi_data flit = data_in.read();
                if (!flit.last || !enable) {
                    out.write(flit);
                    state = flit.last ? IDLE : DATA;
                    break;
                }

                if (!enable) {
                    state = IDLE;
                    break;
                }

                /* Check if the amount of remaining space in the last flit of the
                 * data stream has enough room for the suffix */
                tkeep_count_t b;
                const auto num_chunks = data_bytes >> log_suffix_alignment;
                for (b = 0; b < num_chunks; ++b) {
                    if (!flit.keep(((num_chunks - b) << log_suffix_alignment) - 1,
                                   ((num_chunks - b - 1) << log_suffix_alignment)))
                        break;
                }

                /* Number of bits that fit in the current flit */
                tkeep_count_t bit = data_bytes - (b << log_suffix_alignment);
                const tkeep_count_t cur_flit_suffix = std::min(bit, tkeep_count_t(suffix_length_bytes));
                const int shift = (bit - cur_flit_suffix) * 8;
                const ap_uint<data_bits> mask = ((ap_uint<data_bits>(1) << (cur_flit_suffix * 8)) - 1) << shift;
                const ap_uint<data_bytes> keep_mask = 0xffffffffu << (shift / 8);

                ap_uint<data_bits> suffix_shifted;

                if (cur_flit_suffix >= suffix_length_bytes) {
                    suffix_shifted = ap_uint<data_bits>(suffix) << shift;
                    state = IDLE;
                } else {
                    suffix_shifted = ap_uint<data_bits>(suffix) >> ((suffix_length_bytes - cur_flit_suffix) * 8);
                    last_flit_bytes = suffix_length_bytes - cur_flit_suffix;
                    flit.last = false;
                    state = LAST;
                }
                /* Use exact type to hold not_mask because the ~ operator in HLS adds
                 * an extra bit for some reason, causing a warning. */
                ap_uint<data_bits> not_mask = ~mask;
                flit.data &= not_mask;
                flit.data |= suffix_shifted;
                flit.keep |= keep_mask;
                out.write(flit);
                break;
            }
            case LAST: {
    last:
                ap_uint<data_bits> data = ap_uint<data_bits>(suffix) << (data_bits - last_flit_bytes * 8);
                ntl::axi_data out_buf(data,
                    ntl::axi_data::keep_bytes(last_flit_bytes), true);
                out.write(out_buf);
                state = IDLE;
                break;
            }
            }
        }

    protected:
        /** Reordering state:
         *  IDLE   waiting for header stream entry.
         *  READ_SUFFIX   read the suffix to push (if applies)
         *  DATA   reading and transmitting the data stream.
         *  LAST   output the suffix if it did not fit in the last data flit.
         */
        enum { IDLE, READ_SUFFIX, DATA, LAST } state;
        bool enable, empty;
        suffix_t suffix;
        tkeep_count_t last_flit_bytes;
    };
}
