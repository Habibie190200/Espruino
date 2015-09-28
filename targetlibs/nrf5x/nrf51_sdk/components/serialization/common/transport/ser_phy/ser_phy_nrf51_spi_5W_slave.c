/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

/**@file
 *
 * @defgroup ser_phy_spi_5W_phy_driver_slave ser_phy_nrf51_spi_5W_slave.c
 * @{
 * @ingroup ser_phy_spi_5W_phy_driver_slave
 *
 * @brief SPI_5W_RAW PHY slave driver.
 */


#include <stddef.h>
#include <string.h>


#include "boards.h"
#include "spi_slave.h"
#include "ser_phy.h"
#include "ser_config.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_gpiote.h"
#include "nrf_soc.h"
#include "app_error.h"
#include "app_util.h"
#include "ser_phy_config_conn_nrf51.h"
#include "ser_phy_debug_conn.h"
#include "app_error.h"

#define _static static

#define SER_PHY_SPI_5W_MTU_SIZE SER_PHY_SPI_MTU_SIZE

#define SER_PHY_SPI_DEF_CHARACTER 0xFF //SPI default character. Character clocked out in case of an ignored transaction
#define SER_PHY_SPI_ORC_CHARACTER 0xFF //SPI over-read character. Character clocked out after an over-read of the transmit buffer

#define _SPI_5W_

//SPI raw peripheral device configuration data
typedef struct
{
    int32_t pin_req;       //SPI /REQ pin. -1 for not using
    int32_t pin_rdy;       //SPI /RDY pin. -1 for not using
    int32_t ppi_rdy_ch;    //SPI /RDY ppi ready channel
    int32_t gpiote_rdy_ch; //SPI /RDY pin ready channel
} spi_slave_raw_trasp_cfg_t;

/**@brief States of the SPI transaction state machine. */
typedef enum
{
    SPI_RAW_STATE_UNKNOWN,
    SPI_RAW_STATE_SETUP_HEADER,
    SPI_RAW_STATE_RX_HEADER,
    SPI_RAW_STATE_MEM_REQUESTED,
    SPI_RAW_STATE_RX_PAYLOAD,
    SPI_RAW_STATE_TX_HEADER,
    SPI_RAW_STATE_TX_PAYLOAD,
} trans_state_t;

_static spi_slave_raw_trasp_cfg_t m_spi_slave_raw_config;

_static uint16_t m_accumulated_rx_packet_length;
_static uint16_t m_rx_packet_length;
_static uint16_t m_current_rx_frame_length;

_static uint16_t m_accumulated_tx_packet_length;
_static uint16_t m_tx_packet_length;
_static uint16_t m_current_tx_frame_length;

_static uint8_t m_header_rx_buffer[SER_PHY_HEADER_SIZE + 1]; //+1 for '0' guard in SPI_5W
_static uint8_t m_header_tx_buffer[SER_PHY_HEADER_SIZE + 1]; //+1 for '0' guard in SPI_5W

_static uint8_t       m_tx_frame_buffer[SER_PHY_SPI_5W_MTU_SIZE];
_static uint8_t       m_rx_frame_buffer[SER_PHY_SPI_5W_MTU_SIZE];
_static uint8_t const m_zero_buff[SER_PHY_SPI_5W_MTU_SIZE] = {  0  }; //ROM'able declaration - all guard bytes

_static uint8_t * volatile       m_p_rx_buffer = NULL;
_static const uint8_t * volatile m_p_tx_buffer = NULL;

_static bool m_trash_payload_flag;
_static bool m_buffer_reqested_flag;

_static trans_state_t            m_trans_state      = SPI_RAW_STATE_UNKNOWN;
_static ser_phy_events_handler_t m_ser_phy_callback = NULL;

static void spi_slave_raw_assert(bool cond)
{
    APP_ERROR_CHECK_BOOL(cond);
}

static void callback_ser_phy_event(ser_phy_evt_t event)
{
    if (m_ser_phy_callback)
    {
        m_ser_phy_callback(event);
    }
    return;
}

static void callback_memory_request(uint16_t size)
{
    ser_phy_evt_t event;

    event.evt_type                               = SER_PHY_EVT_RX_BUF_REQUEST;
    event.evt_params.rx_buf_request.num_of_bytes = size;
    callback_ser_phy_event(event);
    return;

}

static void callback_packet_received(uint8_t * pBuffer, uint16_t size)
{
    ser_phy_evt_t event;

    event.evt_type = SER_PHY_EVT_RX_PKT_RECEIVED;
    event.evt_params.rx_pkt_received.num_of_bytes = size;
    event.evt_params.rx_pkt_received.p_buffer     = pBuffer;
    callback_ser_phy_event(event);
    return;
}

static void callback_packet_dropped()
{
    ser_phy_evt_t event;

    event.evt_type = SER_PHY_EVT_RX_PKT_DROPPED;
    callback_ser_phy_event(event);
    return;
}

static void callback_packet_transmitted(void)
{
    ser_phy_evt_t event;

    event.evt_type = SER_PHY_EVT_TX_PKT_SENT;
    callback_ser_phy_event(event);
    return;
}

static void copy_buff(uint8_t * const p_dest, uint8_t const * const p_src, uint16_t len)
{
    uint16_t index;

    for (index = 0; index < len; index++)
    {
        p_dest[index] = p_src[index];
    }
    return;
}

/* Function computes current packet length */
static uint16_t compute_current_frame_length(const uint16_t packet_length,
                                             const uint16_t accumulated_packet_length)
{
    uint16_t current_packet_length = packet_length - accumulated_packet_length;

    if (current_packet_length > SER_PHY_SPI_5W_MTU_SIZE)
    {
        current_packet_length = SER_PHY_SPI_5W_MTU_SIZE;
    }

    return current_packet_length;
}

static uint32_t header_get()
{
    uint32_t err_code;

    err_code = spi_slave_buffers_set((uint8_t *) m_zero_buff,
                                     m_header_rx_buffer,
                                     SER_PHY_HEADER_SIZE,
                                     SER_PHY_HEADER_SIZE);
    return err_code;
}

static uint32_t frame_get()
{
    uint32_t err_code;

    m_current_rx_frame_length = compute_current_frame_length(m_rx_packet_length,
                                                             m_accumulated_rx_packet_length);

    if (!m_trash_payload_flag)
    {
        err_code =
            spi_slave_buffers_set((uint8_t *) m_zero_buff,
                                  &(m_p_rx_buffer[m_accumulated_rx_packet_length]),
                                  m_current_rx_frame_length,
                                  m_current_rx_frame_length);
    }
    else
    {
        err_code = spi_slave_buffers_set((uint8_t *) m_zero_buff,
                                         m_rx_frame_buffer,
                                         m_current_rx_frame_length,
                                         m_current_rx_frame_length);
    }
    return err_code;
}

static uint32_t header_send(uint16_t len)
{
    uint32_t err_code;

    m_header_tx_buffer[0] = (uint8_t) 0; //this is guard byte
    (void)uint16_encode(len, &(m_header_tx_buffer[1]));
    err_code = spi_slave_buffers_set(m_header_tx_buffer,
                                     m_header_rx_buffer,
                                     SER_PHY_HEADER_SIZE + 1,
                                     SER_PHY_HEADER_SIZE + 1);
    return err_code;
}

static uint32_t frame_send()
{
    uint32_t err_code;

    m_current_tx_frame_length = compute_current_frame_length(m_tx_packet_length,
                                                             m_accumulated_tx_packet_length);

    if (m_current_tx_frame_length == SER_PHY_SPI_5W_MTU_SIZE)
    {
        m_current_tx_frame_length -= 1; //extra space for guard byte must be taken into account for MTU
    }
    m_tx_frame_buffer[0] = 0; //guard byte
    copy_buff(&(m_tx_frame_buffer[1]),
              &(m_p_tx_buffer[m_accumulated_tx_packet_length]),
              m_current_tx_frame_length);
    err_code = spi_slave_buffers_set(m_tx_frame_buffer,
                                     m_rx_frame_buffer,
                                     m_current_tx_frame_length + 1,
                                     m_current_tx_frame_length + 1);

    return err_code;
}

static void set_ready_line(void)
{
#ifndef _SPI_5W_
    //toggle - this should go high - but toggle is unsafe
    uint32_t rdy_task = nrf_drv_gpiote_out_task_addr_get(m_spi_slave_raw_config.gpiote_rdy_ch);
    *(uint32_t *)rdy_task = 1;
#endif
    return;
}

static void set_request_line(void)
{
    //active low logic - set is 0
    nrf_gpio_pin_clear(m_spi_slave_raw_config.pin_req);
    DEBUG_EVT_SPI_SLAVE_RAW_REQ_SET(0);
    return;
}

static void clear_request_line(void)
{
    //active low logic - clear is 1
    nrf_gpio_pin_set(m_spi_slave_raw_config.pin_req);
    DEBUG_EVT_SPI_SLAVE_RAW_REQ_CLEARED(0);
    return;
}

/**
 * \brief Slave driver main state machine
 * For UML graph, please refer to SDK documentation
*/
static void spi_slave_event_handle(spi_slave_evt_t event)
{
    static uint32_t err_code = NRF_SUCCESS;
    static uint16_t packetLength;

    switch (m_trans_state)
    {
        case SPI_RAW_STATE_SETUP_HEADER:
            m_trans_state = SPI_RAW_STATE_RX_HEADER;
            err_code      = header_get();
            break;

        case SPI_RAW_STATE_RX_HEADER:

            if (event.evt_type == SPI_SLAVE_BUFFERS_SET_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_BUFFERS_SET(0);
                set_ready_line();
            }

            if (event.evt_type == SPI_SLAVE_XFER_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_RX_XFER_DONE(event.rx_amount);
                spi_slave_raw_assert(event.rx_amount == SER_PHY_HEADER_SIZE);
                packetLength = uint16_decode(m_header_rx_buffer);

                if (packetLength != 0 )
                {
                    m_trans_state          = SPI_RAW_STATE_MEM_REQUESTED;
                    m_buffer_reqested_flag = true;
                    m_rx_packet_length     = packetLength;
                    callback_memory_request(packetLength);
                }
                else
                {
                    if (m_p_tx_buffer)
                    {
                        clear_request_line();
                        m_trans_state = SPI_RAW_STATE_TX_HEADER;
                        err_code      = header_send(m_tx_packet_length);
                    }
                    else
                    {
                        //there is nothing to send - zero response facilitates pooling - but perhaps, it should be assert
                        err_code = header_send(0);
                    }
                }
            }

            break;

        case SPI_RAW_STATE_MEM_REQUESTED:

            if (event.evt_type == SPI_SLAVE_EVT_TYPE_MAX) //This is API dummy event
            {
                m_buffer_reqested_flag         = false;
                m_trans_state                  = SPI_RAW_STATE_RX_PAYLOAD;
                m_accumulated_rx_packet_length = 0;
                err_code                       = frame_get();
            }
            break;


        case SPI_RAW_STATE_RX_PAYLOAD:

            if (event.evt_type == SPI_SLAVE_BUFFERS_SET_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_BUFFERS_SET(0);
                set_ready_line();
            }

            if (event.evt_type == SPI_SLAVE_XFER_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_RX_XFER_DONE(event.rx_amount);
                spi_slave_raw_assert(event.rx_amount == m_current_rx_frame_length);
                m_accumulated_rx_packet_length += m_current_rx_frame_length;

                if (m_accumulated_rx_packet_length < m_rx_packet_length )
                {
                    err_code = frame_get();
                }
                else
                {
                    spi_slave_raw_assert(m_accumulated_rx_packet_length == m_rx_packet_length);
                    m_trans_state = SPI_RAW_STATE_RX_HEADER;
                    err_code      = header_get();

                    if (!m_trash_payload_flag)
                    {
                        callback_packet_received(m_p_rx_buffer, m_accumulated_rx_packet_length);
                    }
                    else
                    {
                        callback_packet_dropped();
                    }
                }
            }
            break;

        case SPI_RAW_STATE_TX_HEADER:

            if (event.evt_type == SPI_SLAVE_BUFFERS_SET_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_BUFFERS_SET(0);
                set_ready_line();
            }

            if (event.evt_type == SPI_SLAVE_XFER_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_TX_XFER_DONE(event.tx_amount);
                spi_slave_raw_assert(event.tx_amount == SER_PHY_HEADER_SIZE + 1);
                m_trans_state                  = SPI_RAW_STATE_TX_PAYLOAD;
                m_accumulated_tx_packet_length = 0;
                err_code                       = frame_send();
            }

            break;

        case SPI_RAW_STATE_TX_PAYLOAD:

            if (event.evt_type == SPI_SLAVE_BUFFERS_SET_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_BUFFERS_SET(0);
                set_ready_line();
            }

            if (event.evt_type == SPI_SLAVE_XFER_DONE)
            {
                DEBUG_EVT_SPI_SLAVE_RAW_TX_XFER_DONE(event.tx_amount);
                spi_slave_raw_assert(event.tx_amount == m_current_tx_frame_length + 1);
                m_accumulated_tx_packet_length += m_current_tx_frame_length;

                if ( m_accumulated_tx_packet_length < m_tx_packet_length )
                {
                    err_code = frame_send();
                }
                else
                {
                    spi_slave_raw_assert(m_accumulated_tx_packet_length == m_tx_packet_length);
                    //clear pointer before callback
                    m_p_tx_buffer = NULL;
                    callback_packet_transmitted();
                    //spi slave TX transfer is possible only when RX is ready, so return to waiting for a header
                    m_trans_state = SPI_RAW_STATE_RX_HEADER;
                    err_code      = header_get();
                }
            }
            break;

        default:
            err_code = NRF_ERROR_INVALID_STATE;
            break;
    }
    APP_ERROR_CHECK(err_code);
}

#ifndef _SPI_5W_
static void spi_slave_gpiote_init(void)
{
    if (!nrf_drv_gpiote_is_init())
    {
        (void)nrf_drv_gpiote_init();
    }
    nrf_drv_gpiote_out_config_t config = GPIOTE_CONFIG_OUT_TASK_TOGGLE(true);
    (void)nrf_drv_gpiote_out_init(m_spi_slave_raw_config.gpiote_rdy_ch, &config);
    return;
}

static void spi_slave_ppi_init(void)
{
    uint32_t rdy_task = nrf_drv_gpiote_out_task_addr_get(m_spi_slave_raw_config.gpiote_rdy_ch);
    //Configure PPI channel  to clear /RDY line
    NRF_PPI->CH[m_spi_slave_raw_config.ppi_rdy_ch].EEP = (uint32_t)(&NRF_SPIS1->EVENTS_END);
    NRF_PPI->CH[m_spi_slave_raw_config.ppi_rdy_ch].TEP = rdy_task;

    //this works only for channels 0..15 - but soft device is using 8-15 anyway
    NRF_PPI->CHEN |= (1 << m_spi_slave_raw_config.ppi_rdy_ch);
    return;
}
#endif

static void spi_slave_gpio_init(void)
{
    nrf_gpio_cfg_output(m_spi_slave_raw_config.pin_req);
    nrf_gpio_pin_set(m_spi_slave_raw_config.pin_req);
#ifndef _SPI_5W_
    nrf_gpio_cfg_output(m_spi_slave_raw_config.pin_rdy);
    nrf_gpio_pin_set(m_spi_slave_raw_config.pin_rdy);
#endif
    return;
}

/* ser_phy API function */
void ser_phy_interrupts_enable(void)
{
    NVIC_EnableIRQ(SPI1_TWI1_IRQn);
}

/* ser_phy API function */
void ser_phy_interrupts_disable(void)
{
    NVIC_DisableIRQ(SPI1_TWI1_IRQn);
}

/* ser_phy API function */
uint32_t ser_phy_rx_buf_set(uint8_t * p_buffer)
{
    uint32_t        status = NRF_SUCCESS;
    spi_slave_evt_t event;

    ser_phy_interrupts_disable();

    if (m_buffer_reqested_flag && (m_trans_state == SPI_RAW_STATE_MEM_REQUESTED))
    {
        m_p_rx_buffer = p_buffer;

        if (m_p_rx_buffer)
        {
            m_trash_payload_flag = false;
        }
        else
        {
            m_trash_payload_flag = true;
        }

        event.evt_type  = SPI_SLAVE_EVT_TYPE_MAX; //force transition with dummy event
        event.rx_amount = 0;
        event.tx_amount = 0;
        spi_slave_event_handle(event);
    }
    else
    {
        status = NRF_ERROR_BUSY;
    }
    ser_phy_interrupts_enable();

    return status;
}

/* ser_phy API function */
uint32_t ser_phy_tx_pkt_send(const uint8_t * p_buffer, uint16_t num_of_bytes)
{
    uint32_t status = NRF_SUCCESS;

    if ( p_buffer == NULL || num_of_bytes == 0)
    {
        return NRF_ERROR_NULL;
    }

    ser_phy_interrupts_disable();

    if ( m_p_tx_buffer == NULL)
    {
        m_tx_packet_length = num_of_bytes;
        m_p_tx_buffer      = p_buffer;
        set_request_line();
    }
    else
    {
        status = NRF_ERROR_BUSY;
    }
    ser_phy_interrupts_enable();

    return status;
}

/* ser_phy API function */
uint32_t ser_phy_open(ser_phy_events_handler_t events_handler)
{
    uint32_t           err_code;
    spi_slave_config_t spi_slave_config;
    spi_slave_evt_t    event;

    if (m_trans_state != SPI_RAW_STATE_UNKNOWN)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (events_handler == NULL)
    {
        return NRF_ERROR_NULL;
    }

    //one ppi channel and one gpiote channel are used to drive RDY line
    m_spi_slave_raw_config.pin_req       = SER_PHY_SPI_SLAVE_REQ_PIN;
    m_spi_slave_raw_config.pin_rdy       = SER_PHY_SPI_SLAVE_RDY_PIN;
    m_spi_slave_raw_config.ppi_rdy_ch    = SER_PHY_SPI_PPI_RDY_CH;
    m_spi_slave_raw_config.gpiote_rdy_ch = SER_PHY_SPI_GPIOTE_RDY_CH;

    spi_slave_gpio_init();
#ifndef _SPI_5W_
    spi_slave_gpiote_init();
    spi_slave_ppi_init();
#endif

    err_code = spi_slave_evt_handler_register(spi_slave_event_handle);

    if (err_code == NRF_SUCCESS)
    {

        spi_slave_config.pin_miso         = SPIS_MISO_PIN;
        spi_slave_config.pin_mosi         = SPIS_MOSI_PIN;
        spi_slave_config.pin_sck          = SPIS_SCK_PIN;
        spi_slave_config.pin_csn          = SPIS_CSN_PIN;
        spi_slave_config.mode             = SPI_MODE_0;
        spi_slave_config.bit_order        = SPIM_LSB_FIRST;
        spi_slave_config.def_tx_character = SER_PHY_SPI_DEF_CHARACTER;
        spi_slave_config.orc_tx_character = SER_PHY_SPI_ORC_CHARACTER;

        //keep /CS high when init
        nrf_gpio_cfg_input(spi_slave_config.pin_csn, NRF_GPIO_PIN_PULLUP);
        err_code = spi_slave_set_cs_pull_up_config(GPIO_PIN_CNF_PULL_Pullup);
        APP_ERROR_CHECK(err_code);

        err_code = spi_slave_init(&spi_slave_config);

        if (err_code == NRF_SUCCESS)
        {
            m_ser_phy_callback = events_handler;

            m_trans_state   = SPI_RAW_STATE_SETUP_HEADER;
            event.evt_type  = SPI_SLAVE_EVT_TYPE_MAX; //force transition for dummy event
            event.rx_amount = 0;
            event.tx_amount = 0;
            spi_slave_event_handle(event);

        }
    }
    return err_code;
}

/* ser_phy API function */
void ser_phy_close(void)
{
    (void) spi_slave_evt_handler_register(NULL);
    m_ser_phy_callback = NULL;
    m_trans_state      = SPI_RAW_STATE_UNKNOWN;
}