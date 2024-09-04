#include <cstring> // memcpy
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>

#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <sidekiq_types.h>

// Attempted to put this in SoapySidekiq.hpp and it would not link
// could not understand why
bool   start_signal = false;

std::vector<std::string> SoapySidekiq::getStreamFormats(
    const int direction, const size_t channel) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamFormats");

    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CS16);

    if (direction == SOAPY_SDR_RX)
    { 
        formats.push_back(SOAPY_SDR_CF32);
    }

    return formats;
}

std::string SoapySidekiq::getNativeStreamFormat(const int    direction,
                                                const size_t channel,
                                                double &     fullScale) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    fullScale = this->max_value;

    return "CS16";
}

SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(
            const int direction, const size_t channel) const
{
    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo bufflenArg;
    bufflenArg.key = "bufflen";

    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamArgsInfo");

    if (direction == SOAPY_SDR_RX)
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "samples";
        bufflenArg.type        = SoapySDR::ArgInfo::INT;
        bufflenArg.value       = std::to_string(rx_payload_size_in_words);

        streamArgs.push_back(bufflenArg);

    }
    else
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "samples";
        bufflenArg.value       = std::to_string(DEFAULT_TX_BUFFER_LENGTH);
        bufflenArg.type        = SoapySDR::ArgInfo::INT;

        streamArgs.push_back(bufflenArg);
    }

    return streamArgs;
}

/*******************************************************************
 * Sidekiq receive thread
 ******************************************************************/

void SoapySidekiq::rx_receive_operation(void)
{
    int status = 0;


    //  skiq receive params
    skiq_rx_block_t *tmp_p_rx_block;
    uint32_t         len;
    bool             first = true;
    uint64_t         expected_timestamp = 0;


    // metadata
    uint64_t      overload = 0;
    skiq_rx_hdl_t rcvd_hdl;

    std::unique_lock<std::mutex> lock(rx_mutex);

    SoapySDR_log(SOAPY_SDR_TRACE, "entering rx_receive_thread");

    // wait till called to start running
    _cv.wait(lock, [this] { return start_signal; });

    SoapySDR_log(SOAPY_SDR_INFO, "Starting RX Sidekiq Thread loop");

    //  loop until stream is deactivated
    while (rx_running)
    {
        //  check for overflow
        if (rxReadIndex == ((rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS) ||
            overload)
        {
            SoapySDR_log(SOAPY_SDR_WARNING,
                         "Detected overflow Event in RX Sidekiq Thread");
            SoapySDR_logf(SOAPY_SDR_DEBUG,
                          "rxReadIndex %d, rxWriteIndex %d, overload %d\n",
                          rxReadIndex, rxWriteIndex, overload);
            rxWriteIndex     = rxReadIndex;
        }
        else
        {
            /*
             *  put the block into a ring buffer so readStream can read it out
             * at a different pace */
            status = skiq_receive(card, &rcvd_hdl, &tmp_p_rx_block, &len);
            if (status == skiq_rx_status_success)
            {
                if (rcvd_hdl == rx_hdl)
                {
                    if (len != rx_block_size_in_bytes)
                    {
                        SoapySDR_logf(SOAPY_SDR_ERROR, "received length %d is not the correct "
                                      "block size %d\n",
                                      len, rx_block_size_in_bytes);
                        throw std::runtime_error("");
                    }

                    // get overload out of metadata
                    overload = tmp_p_rx_block->overload;

                    int num_words_read = (len / 4);

                    // check for timestamp error
                    if (first == false)
                    {
                        if(expected_timestamp != tmp_p_rx_block->rf_timestamp)
                        {
                            SoapySDR_log(SOAPY_SDR_WARNING,
                                        "Detected timestamp overflow in RX Sidekiq Thread");
                            SoapySDR_logf(SOAPY_SDR_DEBUG, "expected timestamp %lu, actual %lu",
                                            expected_timestamp, tmp_p_rx_block->rf_timestamp);

                            // restart the timestamp checking
                            first = true;
                        }
                        else
                        {
                            expected_timestamp += rx_payload_size_in_words;
                        }
                    }
                    else 
                    {
                        first = false;
                        expected_timestamp = tmp_p_rx_block->rf_timestamp + rx_payload_size_in_words;
                    }

                    // copy the data out of the rx_buffer and into the RAM
                    // buffers copy the header in also
                    memcpy(p_rx_block[rxWriteIndex], (void *)tmp_p_rx_block,
                           (num_words_read * sizeof(uint32_t)));
#ifdef DEBUG
                    uint8_t *temp_ptr = (uint8_t *)p_rx_block[rxWriteIndex];

                    for (int i = 0; i < num_words_read; i++)
                    {
                        if ((rxWriteIndex == 0) && (i < 10 || i > 1000))
                        {
                            SoapySDR_logf(SOAPY_SDR_DEBUG,
                                          "R %d, 0x%02X%02X 0x%02X%02X ", i,
                                          *temp_ptr++, *temp_ptr++, *temp_ptr++,
                                          *temp_ptr++);
                        }
                    }

                    printf(" done \n");
#endif

                    rxWriteIndex = (rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS;
                }
            }
            else
            {
                if (status != -1)
                {
                    SoapySDR_logf(SOAPY_SDR_FATAL,
                                  "Failure: skiq_receive (card %d) status %d",
                                  card, status);
                    throw std::runtime_error("");
                }
            }
        }
    }

    SoapySDR_log(SOAPY_SDR_INFO, "Exiting RX Sidekiq Thread");
}

/*******************************************************************
 * Stream API
 ******************************************************************/

SoapySDR::Stream *SoapySidekiq::setupStream(const int direction,
                                            const std::string &format,
                                            const std::vector<size_t> &channels,
                                            const SoapySDR::Kwargs &args)
{

    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setupStream");

    if (direction == SOAPY_SDR_RX)
    {
        //  check the channel configuration
        if (channels.size() > 1) 
        {
            throw std::runtime_error("only one RX channel is supported simultaneously"); 
        }

        rx_hdl = (skiq_rx_hdl_t)channels.at(0);

        SoapySDR_logf(SOAPY_SDR_INFO, "The RX handle is: %u", rx_hdl);
                
        status = skiq_read_rx_block_size(card, skiq_rx_stream_mode_balanced);
        if (status < 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_block_size failed: status: %d\n", 
                          status);
            throw std::runtime_error("");
        }
        rx_block_size_in_bytes   = status;
        rx_block_size_in_words   = status / 4;

        rx_payload_size_in_bytes = status - SKIQ_RX_HEADER_SIZE_IN_BYTES;
        rx_payload_size_in_words = rx_payload_size_in_bytes / 4;

        SoapySDR_logf(SOAPY_SDR_INFO, "Rx payload size in words: %u", rx_payload_size_in_words);

        // allocate the RAM buffers
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            p_rx_block[i] = (skiq_rx_block_t *)malloc(rx_block_size_in_bytes);
            if (p_rx_block[i] == NULL)
            {
                SoapySDR_log(SOAPY_SDR_ERROR, "malloc failed to allocate memory ");
                throw std::runtime_error("");
            }

            memset(p_rx_block[i], 0, rx_block_size_in_bytes);
        }
        rxWriteIndex = 0;
        rxReadIndex  = 0;

        if (format == "CS16")
        {
            rxUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16\n");
        }
        else if (format == "CF32")
        {
            rxUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32\n");
        }
        else
        {
            throw std::runtime_error("setupStream invalid format '" + format +
                "' -- Only CS16 or CF32 is supported by SoapySidekiq module.");
        }

        return RX_STREAM;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("only one TX channel is supported simultaneously"); 
        }

        tx_hdl = (skiq_tx_hdl_t)channels.at(0);

        SoapySDR_logf(SOAPY_SDR_INFO, "The TX handle is: %u", tx_hdl);

        if (format == "CS16")
        {
            txUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16\n");
        }
        else if (format == "CF32")
        {

            txUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32\n");
        }
        else 
        {
            throw std::runtime_error(
                "setupStream invalid format '" + format +
                "' -- Only CS16 is supported by SoapySidekiq TX module.");
        }

        // Allocate buffers
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            p_tx_block[i] = skiq_tx_block_allocate(DEFAULT_TX_BUFFER_LENGTH);
        }
        currTXBuffIndex = 0;

        tx_hdl = (skiq_tx_hdl_t)channels.at(0);
        
        return TX_STREAM;
    }
    else
    {
        throw std::runtime_error("Invalid direction");
    }
}

void SoapySidekiq::closeStream(SoapySDR::Stream *stream)
{

    SoapySDR_logf(SOAPY_SDR_TRACE, "closeStream");
//    this->deactivateStream(stream, 0, 0);

    if (stream == RX_STREAM)
    {
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            free(p_rx_block[i]);
        }
    }
    else if (stream == TX_STREAM)
    {
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            skiq_tx_block_free(p_tx_block[i]);
        }
    }
}

size_t SoapySidekiq::getStreamMTU(SoapySDR::Stream *stream) const
{

    SoapySDR_logf(SOAPY_SDR_TRACE, "getStremMTU");

    if (stream == RX_STREAM)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "block size %u", rx_payload_size_in_words);

        return (rx_payload_size_in_words);
    }
    else if (stream == TX_STREAM)
    {
        return DEFAULT_TX_BUFFER_LENGTH;
    }
    else
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    return 0;
}

int SoapySidekiq::activateStream(SoapySDR::Stream *stream, 
                                 const int flags,
                                 const long long timeNs, 
                                 const size_t numElems)
{

    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "activateStream");

    if (stream == RX_STREAM)
    {
        /* set rx source as iq data */
        if (iq_swap == true)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_qi);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                             "skiq_write_rx_data_src failed (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "RX is set to I then Q order"); 
        }

        skiq_iq_order_t iq_order;
        status = skiq_read_iq_order_mode(card, &iq_order);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                         "skiq_read_iq_order_mode failed, (card %u) status %d",
                         card, status);
            throw std::runtime_error("");
        }
        
        /* set a modest rx timeout to make skiq_receive blocking*/
        status = skiq_set_rx_transfer_timeout(card, 100000);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_set_rx_transfer_timeout failed, (card %d) status %d", 
                          card, status);
            throw std::runtime_error("");
        }

        //  start the receive thread
        if (!_rx_receive_thread.joinable())
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Start RX thread");

            rx_running = true;
            start_signal = false;

            _rx_receive_thread =
                std::thread(&SoapySidekiq::rx_receive_operation, this);
        }

        // signal it to start running
        std::lock_guard<std::mutex> lock(rx_mutex);

        start_signal = true;
        _cv.notify_one();  // Notify the thread to run

        /* start rx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            status = skiq_start_rx_streaming_on_1pps(card, rx_hdl, 0);
            if (skiq_start_rx_streaming(card, rx_hdl) != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming_on_1pps failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }
        else
        {
            status = skiq_start_rx_streaming(card, rx_hdl);
            if (skiq_start_rx_streaming(card, rx_hdl) != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }

    }
    else if (stream == TX_STREAM)
    {
        SoapySDR_logf(SOAPY_SDR_DEBUG, "Start TX, hdl: %d", tx_hdl);
        p_tx_block_index = 0;
        tx_underruns     = 0;

        //  tx block size
        status =
            skiq_write_tx_block_size(card, tx_hdl, DEFAULT_TX_BUFFER_LENGTH);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_write_tx_block_size (card %d) status %d", card,
                status);
            throw std::runtime_error("skiq_write_tx_block_size error");
        }

        //  tx data flow mode
        status = skiq_write_tx_data_flow_mode(card, tx_hdl,
                                              skiq_tx_immediate_data_flow_mode);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_write_tx_data_flow_mode (card %d) status %d",
                card, status);
            throw std::runtime_error("skiq_write_tx_data_flow_mode error");
        }

        // transfer mode (sync for now)
        status = skiq_write_tx_transfer_mode(card, tx_hdl,
                                             skiq_tx_transfer_mode_sync);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_tx_transfer_mode (card %d) status %d",
                          card, status);
            throw std::runtime_error("skiq_tx_transfer_mode error");
        }

        /* start tx streaming */
        status = skiq_start_tx_streaming(card, tx_hdl);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_start_tx_streaming (card %d) status %d", card,
                status);
            throw std::runtime_error("skiq_start_tx_streaming error");
        }
    }

    return 0;
}

int SoapySidekiq::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                   const long long timeNs)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "deactivateStream");

    if (flags != 0)
        return SOAPY_SDR_NOT_SUPPORTED;

    if (stream == RX_STREAM && rx_running == true)
    {
        // stop receive thread
        rx_running = false;

        /* stop rx streaming */
        status = skiq_stop_rx_streaming(card, rx_hdl);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_stop_rx_streaming (card %d) handle "
                          "%d, status %d",
                          card, rx_hdl, status);
        }

        /* wait till the rx thread is done */
        if (_rx_receive_thread.joinable())
        {
            _rx_receive_thread.join();
        }
    }
    else if (stream == TX_STREAM)
    {
        /* stop tx streaming */
        status = skiq_stop_tx_streaming(card, tx_hdl);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_stop_tx_streaming (card %d), status %d", card,
                status);
        }
    }

    return 0;
}

int SoapySidekiq::readStream(SoapySDR::Stream *stream, void *const *buffs,
                             const size_t numElems, int &flags,
                             long long &timeNs, const long timeoutUs)
{
    long waitTime = timeoutUs;

    if (stream != RX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    if (numElems % rx_payload_size_in_words != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the rx block size "
                     " numElems %d, block size %u", numElems, rx_payload_size_in_words);
        throw std::runtime_error("");
    }

    // if the user didn't give a waittime then wait a LONG time
    if (waitTime == 0)
    {
        waitTime = SLEEP_1SEC;
    }

    // see if we have any receive buffers to give
    while ((rxReadIndex == rxWriteIndex) && (waitTime > 0))
    {
        // wait
        usleep(DEFAULT_SLEEP_US);
        waitTime -= DEFAULT_SLEEP_US;
    }

    if (waitTime <= 0)
    {
        SoapySDR_log(SOAPY_SDR_DEBUG, "readStream timed out");
        return SOAPY_SDR_TIMEOUT;
    }

    char *buff_ptr = (char *)buffs[0];
    skiq_rx_block_t *block_ptr = p_rx_block[rxReadIndex];
    char *ringbuffer_ptr = (char *)((char *)block_ptr->data);
    
    if (this->rf_time_source == true)
    {
        timeNs = block_ptr->rf_timestamp;
    }
    else
    {
        timeNs = block_ptr->sys_timestamp;
    }


    // move to the next buffer in the ring
    rxReadIndex = (rxReadIndex + 1) % DEFAULT_NUM_BUFFERS;

    uint32_t block_num = 0;
    uint32_t num_blocks = numElems / rx_payload_size_in_words;

    while (block_num < num_blocks)
    {
        // copy in the amount of data we have in the ring block
        if (rxUseShort == true)
        {
//            SoapySDR_logf(SOAPY_SDR_DEBUG, "block num %u, first value %d", block_num, (int16_t )buff_ptr[0]);

            // CS16
            memcpy(buff_ptr, ringbuffer_ptr, rx_payload_size_in_bytes);
        }
        else
        { 
            // CF32 float
            float *  dbuff_ptr = (float *)buff_ptr;
            int16_t *source = (int16_t *)ringbuffer_ptr;
            int short_ctr = 0;

            for (uint32_t i = 0; i < rx_payload_size_in_bytes; i++)
            {
                *dbuff_ptr++ = (float)source[short_ctr + 1] / this->max_value;
                *dbuff_ptr++ = (float)source[short_ctr] / this->max_value;
                short_ctr += 2;
            }
        }
    
        // a block is done, so move counter.
        block_num++;

        // if we need more blocks get the next one
        if (block_num < num_blocks)
        {
            // get next block if available otherwise wait
            while ((rxReadIndex == rxWriteIndex) && (waitTime > 0))
            {
                // wait
                usleep(DEFAULT_SLEEP_US);
                waitTime -= DEFAULT_SLEEP_US;
            }

            if (waitTime <= 0)
            {
                SoapySDR_log(SOAPY_SDR_DEBUG, "readStream timed out");
                return SOAPY_SDR_TIMEOUT;
            }

            buff_ptr +=  rx_payload_size_in_bytes;
            block_ptr = p_rx_block[rxReadIndex];
            ringbuffer_ptr = (char *)((char *)block_ptr->data);

            // move to the next buffer in the ring
            rxReadIndex = (rxReadIndex + 1) % DEFAULT_NUM_BUFFERS;
        }
    }

    // if we are here then we have put NumElems into the buffer
    return numElems;
}

int SoapySidekiq::writeStream(SoapySDR::Stream * stream,
                              const void *const *buffs, const size_t numElems,
                              int &flags, const long long timeNs,
                              const long timeoutUs)
{

    int      status = 0;
    uint32_t errors = 0;

    if (stream != TX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    if (numElems % DEFAULT_TX_BUFFER_LENGTH != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the tx MTU size "
                     " numElems %d, block size %u", numElems, DEFAULT_TX_BUFFER_LENGTH);
        throw std::runtime_error("");
    }

    // Pointer to the location in the input buffer to transmit from
    char *inbuff_ptr = (char *)(buffs[0]);
#ifdef debug2
    int16_t *tmp_ptr = (int16_t *)inbuff_ptr;

    uint32_t tmp_ctr = 0;

    for (int i = 0; i < 5; i++)
    {
        printf(" 0x%04X 0x%04X ", (uint16_t)tmp_ptr[tmp_ctr],
               (uint16_t)tmp_ptr[tmp_ctr + 1]);
        tmp_ctr += 2;
        fflush(stdout);
    }
    printf("\n");
#endif

    // Pointer to the location in the output buffer to copy to.
    char *outbuff_ptr =
        (char *)p_tx_block[currTXBuffIndex]->data + p_tx_block_index;

    // How many bytes are left in the block we are working on.
    uint32_t block_bytes_left =
        (DEFAULT_TX_BUFFER_LENGTH * 4) - p_tx_block_index;

    // total number of bytes that need to be transmitted in this call
    uint32_t data_bytes = numElems * 4;

    /* loop until the number of elements to send is less than the size left in
     * the block */
    while (data_bytes >= block_bytes_left)
    {
#ifdef debug
        SoapySDR_logf(SOAPY_SDR_DEBUG,
                      "1 numElems %u, data_bytes %d, block_bytes_left %d, "
                      "p_tx_block_index %u\n",
                      numElems, data_bytes, block_bytes_left, p_tx_block_index);
        SoapySDR_logf(SOAPY_SDR_DEBUG, "inbuff_ptr %p, outbuff_ptr %p\n",
                      inbuff_ptr, outbuff_ptr);
#endif

        // determine if we received short or float
        if (txUseShort == true)
        { // CS16
            memcpy(outbuff_ptr, inbuff_ptr, block_bytes_left);
            inbuff_ptr += block_bytes_left;
        }
        else
        { // float
            float *  float_inbuff = (float *)inbuff_ptr;
            uint32_t words_left = block_bytes_left / 4;
            uint16_t * new_outbuff = (uint16_t *)outbuff_ptr;

            int short_ctr = 0;
            for (uint32_t i = 0; i < words_left; i++)
            {
                new_outbuff[short_ctr + 1] = (uint16_t)(float_inbuff[short_ctr + 1] * this->max_value);
                new_outbuff[short_ctr ] = (uint16_t)(float_inbuff[short_ctr] * this->max_value);
#ifdef debug3
                if (i < 1)
                {

                    printf("1 I write float %f, short %d, calc_int %f\n",
                           float_inbuff[short_ctr], new_outbuff[short_ctr],
                           (*(dbuff_ptr - 2) * this->max_value));
                    printf("1 Q write %d, real %f, calc_int %f\n\n",
                           float_inbuff[short_ctr + 1], *(dbuff_ptr - 1),
                           (*(dbuff_ptr - 1) * this->max_value));
                }
#endif
                short_ctr += 2;
            }

        }



        status = skiq_transmit(card, tx_hdl, p_tx_block[currTXBuffIndex], NULL);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_transmit (card %d) status %d", card,
                          status);
        }

        data_bytes -= block_bytes_left;
        block_bytes_left = DEFAULT_TX_BUFFER_LENGTH * 4;

        p_tx_block_index = 0;
        currTXBuffIndex  = (currTXBuffIndex + 1) % DEFAULT_NUM_BUFFERS;
        outbuff_ptr      = (char *)p_tx_block[currTXBuffIndex]->data;
    }

    // This means that the number of bytes left to send is smaller than what is
    // left in the block So just copy the data into the block and be done.  The
    // rest will be sent on the next call
    if (data_bytes < block_bytes_left && data_bytes != 0)
    {
#ifdef debug
        SoapySDR_logf(
            SOAPY_SDR_DEBUG,
            "2 data_bytes %d, block_bytes_left %d, p_tx_block_index %u\n",
            data_bytes, block_bytes_left, p_tx_block_index);
        SoapySDR_logf(SOAPY_SDR_DEBUG, "inbuff_ptr %p, outbuff_ptr %p\n",
                      inbuff_ptr, outbuff_ptr);
#endif

        memcpy(outbuff_ptr, inbuff_ptr, block_bytes_left);
        p_tx_block_index += data_bytes;
    }

    /* This call will return a cumulative number of underruns since start
     * streaming */
    status = skiq_read_tx_num_underruns(card, tx_hdl, &errors);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "Failure: skiq_read_tx_num_underruns (card %d) status %d",
                      card, status);
    }

    if (errors >= tx_underruns + 500)
    {
        printf("cumulative underruns %d\n", errors);
        tx_underruns = errors;
    }

    return numElems;
}

