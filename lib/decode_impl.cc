/* -*- c++ -*- */
/* 
 * Copyright 2016 Bastille Networks.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "decode_impl.h"

#define MAXIMUM_RDD 4

#define HAMMING_T1_BITMASK 0xAA  // 0b10101010
#define HAMMING_T2_BITMASK 0x66  // 0b01100110
#define HAMMING_T4_BITMASK 0x1E  // 0b00011110
#define HAMMING_T8_BITMASK 0xFE  // 0b11111110

#define INTERLEAVER_BLOCK_SIZE 12

namespace gr {
  namespace lora {

    decode::sptr
    decode::make(   short spreading_factor,
                    short code_rate,
                    bool  header)
    {
      return gnuradio::get_initial_sptr
        (new decode_impl(spreading_factor, code_rate, header));
    }

    /*
     * The private constructor
     */
    decode_impl::decode_impl( short spreading_factor,
                              short code_rate,
                              bool  header)
      : gr::block("decode",
              gr::io_signature::make(0, 0, 0),
              gr::io_signature::make(0, 0, 0)),
        d_sf(spreading_factor),
        d_cr(code_rate),
        d_header(header ? true : false)
    {
      assert((d_sf > 5) && (d_sf < 13));
      assert((d_cr > 0) && (d_cr < 5));
      if (d_sf == 6) assert(!header);

      d_in_port = pmt::mp("in");
      d_out_port = pmt::mp("out");

      message_port_register_in(d_in_port);
      message_port_register_out(d_out_port);

      set_msg_handler(d_in_port, boost::bind(&decode_impl::decode, this, _1));

      switch(d_sf)
      {
        case 6:
          d_whitening_sequence = whitening_sequence_sf6_implicit;
        case 7:
          d_whitening_sequence = whitening_sequence_sf7_implicit;
          break;
        case 8:
          if (d_header) d_whitening_sequence = whitening_sequence_sf8_explicit;
          else          d_whitening_sequence = whitening_sequence_sf8_implicit;
          break;
        case 9:
          d_whitening_sequence = whitening_sequence_sf9_implicit;
          break;
        case 10:
          d_whitening_sequence = whitening_sequence_sf10_implicit;
          break;
        case 11:
          d_whitening_sequence = whitening_sequence_sf11_implicit;
          break;
        case 12:
          d_whitening_sequence = whitening_sequence_sf12_implicit;
          break;
        default:
          // TODO error condition
          d_whitening_sequence = whitening_sequence_sf8_implicit;
          break;
      }

      d_interleaver_size = d_sf;

      d_fft_size = (1 << spreading_factor);
    }

    /*
     * Our virtual destructor.
     */
    decode_impl::~decode_impl()
    {
    }

    void
    decode_impl::to_gray(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; i < symbols.size(); i++)
      {
        symbols[i] = (symbols[i] >> 1) ^ symbols[i];
      }
    }

    void
    decode_impl::from_gray(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; i < symbols.size(); i++)
      {
        symbols[i] = symbols[i] ^ (symbols[i] >> 16);
        symbols[i] = symbols[i] ^ (symbols[i] >>  8);
        symbols[i] = symbols[i] ^ (symbols[i] >>  4);
        symbols[i] = symbols[i] ^ (symbols[i] >>  2);
        symbols[i] = symbols[i] ^ (symbols[i] >>  1);
      }
    }

    void
    decode_impl::whiten(std::vector<unsigned short> &symbols)
    {
      for (int i = 0; (i < symbols.size()) && (i < whitening_sequence_length); i++)
      {
        symbols[i] = (symbols[i] ^ d_whitening_sequence[i]);
      }
    }

    // Forward interleaver dimensions:
    //  PPM   == number of bits per symbol OUT of interleaver        AND number of codewords IN to interleaver
    //  RDD+4 == number of bits per codeword IN to interleaver       AND number of interleaved codewords OUT of interleaver
    //
    // bit width in:  (4+rdd)   block length: ppm
    // bit width out: ppm       block length: (4+rdd)

    // Reverse interleaver (de-interleaver) dimensions:
    //  PPM   == number of bits per symbol IN to deinterleaver       AND number of codewords OUT of deinterleaver
    //  RDD+4 == number of bits per codeword OUT of deinterleaver    AND number of interleaved codewords IN to deinterleaver
    //
    // bit width in:  ppm       block length: (4+rdd)
    // bit width out: (4+rdd)   block length: ppm
    void
    decode_impl::deinterleave(std::vector <unsigned short> &symbols,
                              std::vector <unsigned char> &codewords,
                              unsigned char ppm,
                              unsigned char rdd)
    {
      int symbol_offset = 0;
      int bit_offset    = 0;
      int bit_idx       = 0;
      unsigned char block[INTERLEAVER_BLOCK_SIZE];    // maximum bit-width is 8, should RDD==4

      // Swap MSBs of each symbol within buffer (one of LoRa's quirks)
      for (int symbol_idx = 0; symbol_idx < symbols.size(); symbol_idx++)
      {
        symbols[symbol_idx] = ( (symbols[symbol_idx] &  (0x1 << (ppm-1))) >> 1 |
                                (symbols[symbol_idx] &  (0x1 << (ppm-2))) << 1 |
                                (symbols[symbol_idx] & ((0x1 << (ppm-2)) - 1))
                              );
      }

      // Block interleaver: de-interleave RDD+4 symbols at a time into PPM codewords
      for (int block_count = 0; block_count < symbols.size()/(4+rdd); block_count++)
      {
        memset(block, 0, INTERLEAVER_BLOCK_SIZE*sizeof(unsigned char));
        bit_idx = 0;
        bit_offset = 0;

        // Iterate through each bit in the interleaver block
        for (int bitcount = 0; bitcount < ppm*(4+rdd); bitcount++)
        {
              // Symbol indexing                                     // Diagonal pattern mask
          if (symbols[(bitcount % (4+rdd)) + (4+rdd)*block_count] & ((0x1 << (ppm-1)) >> ((bit_idx + bit_offset) % ppm)))
          {
            block[bitcount / (4+rdd)] |= 0x1 << (bitcount % (4+rdd));   // integer divison in C++ is defined to floor
          }

          // bit_idx walks through diagonal interleaving pattern, bit_offset adjusts offset starting point for each codeword
          if (bitcount % (4+rdd) == (4+rdd-1))
          {
            bit_idx = 0;
            bit_offset++;
          }
          else
          {
            bit_idx++;
          }
        }

        // Post-process de-interleaved codewords
        for (int cw_idx = 0; cw_idx < ppm; cw_idx++)
        { 
          // Reverse bit order
          // Shift bits down based on bit depth, mask accordingly
          // block[cw_idx] = (block[cw_idx] >> (MAXIMUM_RDD - rdd)) & ((1 << (4+rdd)) - 1);

          // Put bits into traditional Hamming order
          switch (rdd)
          {
            case 4:
              block[cw_idx] = (block[cw_idx] & 128) | (block[cw_idx] & 64) | (block[cw_idx] & 32) >> 1 | (block[cw_idx] & 16) >> 4 | (block[cw_idx] & 8) << 2 | (block[cw_idx] & 4) << 1 | (block[cw_idx] & 2) << 1 | (block[cw_idx] & 1) << 1;
              break;

            case 3:
              block[cw_idx] = (block[cw_idx] & 64) | (block[cw_idx] & 32) | (block[cw_idx] & 16) >> 1 | (block[cw_idx] & 8) << 1 | (block[cw_idx] & 4) | (block[cw_idx] & 2) | (block[cw_idx] & 1);
              break;

            default:
              break;
          }

          // Mask
          block[cw_idx] = block[cw_idx] & ((1 << (4+rdd)) - 1);

          // Append deinterleaved codewords to codeword buffer
          codewords.push_back(block[cw_idx]);
        }
      }

#if 0
      int inner = 0;
      int outer = 0;
      unsigned char block[INTERLEAVER_BLOCK_SIZE] = {0};
      unsigned char word = 0;

      for (int block_idx = 0; block_idx < symbols.size()/(4+rdd); block_idx++)
      {
        memset(block, 0, INTERLEAVER_BLOCK_SIZE*sizeof(unsigned char));
        int block_offset = block_idx*(4+rdd);

        for (int k = 0; k < (4+rdd); k++)
        {
          for (int m = 0; m < ppm; m++)
          {
            const int i = (m + k) % ppm;
            unsigned char bit = (symbols[block_offset + k] >> m) & 0x1;

            if      (m == ppm-1) block[(i - 1 + ppm) % ppm] |= (bit << k);
            else if (m == ppm-2) block[(i + 1 + ppm) % ppm] |= (bit << k);
            else                 block[i]                   |= (bit << k);
          }
        }

        for (int reorder = 0; reorder < INTERLEAVER_BLOCK_SIZE; reorder++)    // Rather than PPM to account for non-sequential codeword order
        {
          if (rdd == 4)
          {
            block[reorder] = ((block[reorder] & 128)) | ((block[reorder] & 64)) | ((block[reorder] & 32) >> 5) | ((block[reorder] & 16)) | ((block[reorder] & 8) << 2) | ((block[reorder] & 4) << 1) | ((block[reorder] & 2) << 1) | ((block[reorder] & 1) << 1);
          }
          // else if (rdd == 3)
          // {
          //   block[reorder] = (block[reorder] & 64) | (block[reorder] & 32) | ((block[reorder] & 16) >> 1) | ((block[reorder] & 8) << 1) | (block[reorder] & 4) | (block[reorder] & 2) | (block[reorder] & 1);
          // }
        }

        // std::cout << "INTERLEAVER BLOCK FOLLOWS" << std::endl;
        // for (int i = 0; i < INTERLEAVER_BLOCK_SIZE; i++)
        // {
        //   std::cout << std::bitset<8>(block[i]) << std::endl;
        // }
        // std::cout << "INTERLEAVER BLOCK END" << std::endl;

        // codewords.push_back(block[1]);
        // codewords.push_back(block[0]);
        // codewords.push_back(block[3]);
        // codewords.push_back(block[2]);
        // if (ppm == 5)
        // {
        //   codewords.push_back(block[4]);
        // }
        // else if (ppm == 6)
        // {
        //   codewords.push_back(block[5]);
        //   codewords.push_back(block[4]);
        // }
        // else if (ppm == 7)
        // {
        //   codewords.push_back(block[5]);
        //   codewords.push_back(block[4]);
        //   codewords.push_back(block[6]);
        // }
        // else if (ppm == 8)
        // {
        //   codewords.push_back(block[5]);
        //   codewords.push_back(block[4]);
        //   codewords.push_back(block[7]);
        //   codewords.push_back(block[6]);
        // }
        // else if (ppm == 9)
        // {
        //   codewords.push_back(block[5]);
        //   codewords.push_back(block[4]);
        //   codewords.push_back(block[7]);
        //   codewords.push_back(block[6]);
        //   codewords.push_back(block[8]);
        // }
        // else if (ppm == 10)
        // {
        //   codewords.push_back(block[5]);
        //   codewords.push_back(block[4]);
        //   codewords.push_back(block[7]);
        //   codewords.push_back(block[6]);
        //   codewords.push_back(block[9]);
        //   codewords.push_back(block[8]);
        // }

/*
        for (int out_idx = 0; out_idx < ppm/2; out_idx++)
        {
          if (2*out_idx == ppm-1)
          {
            codewords.push_back(block[2*out_idx+1]);
          }
          else
          {
            codewords.push_back(block[2*out_idx+1]);
            codewords.push_back(block[2*out_idx]);
          }
        }
*/

        for (int out_idx = 0; out_idx < ppm; out_idx++)
        {
          codewords.push_back(block[out_idx]);
        }
      }
#endif
    }

/*
    //   for (int x = 0; x < symbols.size()/(4+rdd); x++)
    //   {
    //     memset(block, 0, ppm*sizeof(unsigned char));
    //     const int symOff = x*(4+rdd);
    //     for (int k = 0; k < (4+rdd); k++)
    //     {
    //       for (int m = 0; m < ppm; m++)
    //       {
    //         const int i = (m + k) % ppm;
    //         unsigned char bit = (symbols[symOff + k] >> m) & 0x1;
    //
    //         if      (m == ppm-1) block[(i - 1 + ppm) % ppm] |= (bit << k);
    //         else if (m == ppm-2) block[(i + 1 + ppm) % ppm] |= (bit << k);
    //         else                 block[i]                   |= (bit << k);
    //       }
    //     }
    //     for (int j = 0; j < ppm; j++) codewords.push_back(block[j]);
    //   }
    // }
  */


    void
    decode_impl::hamming_decode(std::vector<unsigned char> &codewords,
                                std::vector<unsigned char> &bytes,
                                unsigned char rdd)
    {
      unsigned char t1, t2, t4, t8;
      unsigned char mask;
      unsigned int num_set_bits;
      unsigned int num_set_flags;
      int error_pos = 0;

      for (int i = 0; i < codewords.size(); i++)
      {
        // Reorder into hamming format. Hamming parity bits p3 and p4 are flipped OTA, so format to be corrected is p1,p2,p4,p3,d1,d2,d3,d4
        // Final bit order is p1,p2,d1,p3,d2,d3,d4,p4
        // codewords[i] =  ((codewords[i] & 128))    | ((codewords[i] & 64))     | ((codewords[i] & 32) >> 5) | ((codewords[i] & 16)) | 
        //                 ((codewords[i] & 8) << 2) | ((codewords[i] & 4) << 1) | ((codewords[i] & 2) << 1)  | ((codewords[i] & 1) << 1);

        // std::cout << "Interleave Debug Raw Codeword " << std::bitset<8>(codewords[i]) << std::endl;

        switch (rdd) {
          case 4:
            t8 = parity(codewords[i], mask = (unsigned char)HAMMING_T8_BITMASK);
          case 3:
            t4 = parity(codewords[i], mask = (unsigned char)HAMMING_T4_BITMASK >> (4 - rdd));
          case 2:
            t2 = parity(codewords[i], mask = (unsigned char)HAMMING_T2_BITMASK >> (4 - rdd));
          case 1:
            t1 = parity(codewords[i], mask = (unsigned char)HAMMING_T1_BITMASK >> (4 - rdd));
            break;
        }
        // t1 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T1_BITMASK >> (4 - rdd));
        // t2 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T2_BITMASK >> (4 - rdd));
        // t4 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T4_BITMASK >> (4 - rdd));
        // if (rdd >= 4)
        //   t8 = parity((unsigned char)codewords[i], mask = (unsigned char)HAMMING_T8_BITMASK);

        error_pos = -1;
        if (t1 != 0) error_pos += 1;
        if (t2 != 0) error_pos += 2;
        if (t4 != 0) error_pos += 4;

        num_set_flags = t1 + t2 + t4;

        // std::cout << "Interleave Debug error position: " << std::dec << error_pos << std::endl;

        // Hamming(4+rdd,4) is only corrective if rdd >= 3
        if (rdd > 2)
        {
          if (error_pos >= 0 && num_set_flags < 3)
          {
            codewords[i] ^= (0x80 >> (4-rdd)) >> error_pos;
          } 

          // std::cout << "Interleave Debug Bits Corrected " << std::bitset<8>(codewords[i]) << std::endl;

          num_set_bits = 0;
          for (int bit_idx = 0; bit_idx < 8; bit_idx++)
          {
            if (codewords[i] & (0x01 << bit_idx))
            {
              num_set_bits++;
            }
          }

          // std::cout << "Interleave Debug # Corrected Set Bits: " << std::dec << num_set_bits << std::endl;

          if (rdd == 4)
          {
            if (num_set_bits < 3)
            {
              codewords[i] = 0;
            }
            else if (num_set_bits > 5)
            {
              codewords[i] = 0xFF;
            }
          }
        }

        // std::cout << "Interleave Debug Bit Threshold " << std::bitset<8>(codewords[i]) << std::endl;

        switch (rdd)
        {
          case 1:
          case 2:
            codewords[i] = codewords[i] & 0x0F;
            break;
          case 3:
            codewords[i] = (((codewords[i] & 0x10) >> 1) | \
                            ((codewords[i] & 0x04))      | \
                            ((codewords[i] & 0x02))      | \
                            ((codewords[i] & 0x01))) & 0x0F;
            break;
          case 4:
            codewords[i] = (((codewords[i] & 0x20) >> 2) | \
                            ((codewords[i] & 0x08) >> 1) | \
                            ((codewords[i] & 0x04) >> 1) | \
                            ((codewords[i] & 0x02) >> 1)) & 0x0F;
            break;
        }

        // std::cout << "Interleave Debug Corrected Data " << std::bitset<4>(codewords[i] & 0x0F) << std::endl;
        bytes.push_back(codewords[i] & 0x0F);
/*
        if (i%2 == 1)
        {
          bytes.push_back(((codewords[i-1] & 0x0F) << 4) | (codewords[i] & 0x0F));
        }
*/
        // else if ((i%2 == 0) && (i == codewords.size())) // last codeword of odd # of codewords
        // {
        //   bytes.push_back(codewords[i] & 0x0F);
        // }
      }
    }

    unsigned char
    decode_impl::parity(unsigned char c, unsigned char bitmask)
    {
      unsigned char parity = 0;
      unsigned char shiftme = c & bitmask;

      for (int i = 0; i < 8; i++)
      {
        if (shiftme & 0x1) parity++;
        shiftme = shiftme >> 1;
      }

      return parity % 2;
    }

    void
    decode_impl::print_payload(std::vector<unsigned char> &payload)
    {
        std::cout << "Received LoRa packet (hex): ";
        for (int i = 0; i < payload.size(); i++)
        {
          std::cout << std::hex << (unsigned int)payload[i] << " ";
        }
        std::cout << std::endl;
    }

    void
    decode_impl::print_bitwise_u8(std::vector<unsigned char> &buffer)
    {
      for (int i = 0; i < buffer.size(); i++)
      {
        std::cout << i << "\t" << std::bitset<8>(buffer[i] & 0xFF) << "\t";
        std::cout << std::hex << (buffer[i] & 0xFF) << std::endl;
      }
    }

    void
    decode_impl::print_bitwise_u16(std::vector<unsigned short> &buffer)
    {
      for (int i = 0; i < buffer.size(); i++)
      {
        std::cout << i << "\t" << std::bitset<16>(buffer[i] & 0xFFFF) << "\t";
        std::cout << std::hex << (buffer[i] & 0xFFFF) << std::endl;
      }
    }

    void
    decode_impl::decode(pmt::pmt_t msg)
    {
      pmt::pmt_t symbols(pmt::cdr(msg));

      size_t pkt_len(0);
      const uint16_t* symbols_v = pmt::u16vector_elements(symbols, pkt_len);

      std::vector<unsigned short> symbols_in;
      std::vector<unsigned short> header_symbols_in;
      std::vector<unsigned short> payload_symbols_in;
      std::vector<unsigned char> header_codewords;
      std::vector<unsigned char> payload_codewords;
      std::vector<unsigned char> header_bytes;
      std::vector<unsigned char> payload_bytes;

      symbols_in.clear();
      for (int i = 0; i < pkt_len; i++) symbols_in.push_back(symbols_v[i]);

      // std::cout << "Received Symbols: " << std::endl;
      // print_bitwise_u16(symbols_in);

      to_gray(symbols_in);
#if 1
      whiten(symbols_in);

      for (int i = 0; (i < symbols_in.size()) && (i < 8); i++) header_symbols_in.push_back(symbols_in[i]);
      for (int i = 8;  i < symbols_in.size()            ; i++) payload_symbols_in.push_back(symbols_in[i]);

      std::cout << "header syms len " << header_symbols_in.size() << std::endl;
      std::cout << "payload syms len " << payload_symbols_in.size() << std::endl;

      print_bitwise_u16(header_symbols_in);
      print_bitwise_u16(payload_symbols_in);

      // Decode header
      // First 8 symbols are always sent at ppm=d_sf-2, rdd=4 (code rate 4/8), regardless of header mode
      deinterleave(header_symbols_in, header_codewords, d_sf - 2, 4);
      std::cout << "deinterleaved header" << std::endl;
      print_bitwise_u8(header_codewords);
      hamming_decode(header_codewords, header_bytes, 4);

      // Decode payload
      deinterleave(payload_symbols_in, payload_codewords, d_sf, d_cr);
      std::cout << "deinterleaved payload" << std::endl;
      print_bitwise_u8(payload_codewords);
      hamming_decode(payload_codewords, payload_bytes, d_cr);

      std::cout << "payload bytes len " << payload_bytes.size() << std::endl;

      // print_payload(bytes);

      header_bytes.insert(header_bytes.end(), payload_bytes.begin(), payload_bytes.end());
      std::cout << "combined bytes len " << header_bytes.size() << std::endl;
      pmt::pmt_t output = pmt::init_u8vector(header_bytes.size(), header_bytes);
#else
      // std::cout << "Whitening Sequence: " << std::endl;
      // print_bitwise_u16(symbols_in);
      for (int i = 0; i < symbols_in.size(); i++)
      {
        // std::cout << ", 0x" << std::hex << symbols_in[i];
        // std::cout << ", " << symbols_in[i];
        std::cout << ", " << std::bitset<16>(symbols_in[i]);
      }
      std::cout << std::endl;
      std::cout << "Length of above: " << symbols_in.size() << std::endl;

      pmt::pmt_t output = pmt::init_u16vector(symbols_in.size(), symbols_in);
      // for (int i = 0; i < symbols_in.size(); i++)
      // {
      //   bytes.push_back(symbols_in[i]);
      // }
      // pmt::pmt_t output = pmt::init_u8vector(bytes.size(), bytes);    
#endif
      pmt::pmt_t msg_pair = pmt::cons(pmt::make_dict(), output);
      message_port_pub(d_out_port, msg_pair);
    }

  } /* namespace lora */
} /* namespace gr */

