#include <stdint.h>

/*
 * sampling_adc_encode_request
 *
 * Encode the SPI message for the ADC requesting sampling of the supplied
 * channel number.
 *
 * **NOTE: Bytes are sent least-significant first**
 */
inline uint32_t sampling_adc_encode_request(uint32_t channel) {

    #if SAMPLING_ADC == MCP3202

        // MSP3202 ADC single-ended on channel c (binary):
        //
        //               .-------- Start bit
        //               | .------ Single ended
        //               | |.----- Channel select
        //               | ||.---- MSBF (set to 1 to disable MSB mode)
        //               | |||.--- Null bit
        //               | ||||
        //   TX: 0000 0001 1c1x xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd
        //      | byte 0  | byte 1  | byte 2  |
        //
        
        // Byte 0
        return 0x01
        // Byte 1
        | ((0xa0 | ((channel & 0x01) << 6)) << 8);

    #elif SAMPLING_ADC == MCP3204

        // MSP3204 ADC single-ended on channel cc (binary):
        //
        //             .---------- Start bit
        //             |.--------- Single ended
        //             ||
        //             ||  .------ Channel select 1
        //             ||  |.----- Channel select 0
        //             ||  || .--- Null bit
        //             ||  || |
        //   TX: 0000 011x ccxx xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd
        //      | byte 0  | byte 1  | byte 2  |
        //

        // Byte 0
        return 0x06
        // Byte 1
        | ((channel & 0x03) << 14);

    #elif SAMPLING_ADC == MCP3208

        // MSP3208 ADC single-ended on channel ccc (binary):
        //
        //             .---------- Start bit
        //             |.--------- Single ended
        //             ||.-------- Channel select 2
        //             ||| .------ Channel select 1
        //             ||| |.----- Channel select 0
        //             ||| || .--- Null bit
        //             ||| || |
        //   TX: 0000 011c ccxx xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd
        //      | byte 0  | byte 1  | byte 2  |
        //

        // Byte 0
        return 0x06 | ((channel & 0x07) >> 2)
        // Byte 1
        | ((channel & 0x03) << 14);

    #else
        #error SAMPLING_ADC must be one of (MCP3202, MCP3204, MCP3208)
    #endif
}

/*
 * sampling_adc_decode_response
 *
 * Decode the SPI message into the ADC sampled value.
 *
 */
inline uint32_t sampling_adc_decode_response(uint32_t response) {
    #if SAMPLING_ADC == MCP3202

        // MSP3202 ADC single-ended on channel c (binary):
        //
        //               .-------- Start bit
        //               | .------ Single ended
        //               | |.----- Channel select
        //               | ||.---- MSBF (set to 1 to disable MSB mode)
        //               | |||.--- Null bit
        //               | ||||
        //   TX: 0000 0001 1c1x xxxx xxxx xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd 0000 0000
        //      | byte 0  | byte 1  | byte 2  | byte 3  |
        //
        // NOTE: Bytes are sent least-significant first

        return (response & 0x000f00) | ((response & 0xff0000) >> 16);

    #elif SAMPLING_ADC == MCP3204

        // MSP3204 ADC single-ended on channel cc (binary):
        //
        //             .---------- Start bit
        //             |.--------- Single ended
        //             ||
        //             ||  .------ Channel select 1
        //             ||  |.----- Channel select 0
        //             ||  || .--- Null bit
        //             ||  || |
        //   TX: 0000 011x ccxx xxxx xxxx xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd 0000 0000
        //      | byte 0  | byte 1  | byte 2  | byte 3  |
        //
        // NOTE: Bytes are sent least-significant first

        return (response & 0x000f00) | ((response & 0xff0000) >> 16);

    #elif SAMPLING_ADC == MCP3208

        // MSP3208 ADC single-ended on channel ccc (binary):
        //
        //             .---------- Start bit
        //             |.--------- Single ended
        //             ||.-------- Channel select 2
        //             ||| .------ Channel select 1
        //             ||| |.----- Channel select 0
        //             ||| || .--- Null bit
        //             ||| || |
        //   TX: 0000 011c ccxx xxxx xxxx xxxx xxxx xxxx
        //   RX: xxxx xxxx xxx0 dddd dddd dddd 0000 0000
        //      | byte 0  | byte 1  | byte 2  | byte 3  |
        //
        // NOTE: Bytes are sent least-significant first

        return (response & 0x000f00) | ((response & 0xff0000) >> 16);

    #else
        #error SAMPLING_ADC must be one of (MCP3202, MCP3204, MCP3208)
    #endif
}
