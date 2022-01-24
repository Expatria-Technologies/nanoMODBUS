#include "modbusino.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef MBSN_DEBUG
#include <stdio.h>
#define DEBUG(...) printf(__VA_ARGS__)
#else
#define DEBUG(...) (void) (0)
#endif

#if !defined(MBSN_BIG_ENDIAN) && !defined(MBSN_LITTLE_ENDIAN)
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN || defined(__BIG_ENDIAN__) || defined(__ARMEB__) ||          \
        defined(__THUMBEB__) || defined(__AARCH64EB__) || defined(_MIBSEB) || defined(__MIBSEB) || defined(__MIBSEB__)
#define MBSN_BIG_ENDIAN
#elif defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN || defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) ||  \
        defined(__THUMBEL__) || defined(__AARCH64EL__) || defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__)
#define MBSN_LITTLE_ENDIAN
#else
#error "Failed to automatically detect platform endianness. Please define either MBSN_BIG_ENDIAN or MBSN_LITTLE_ENDIAN."
#endif
#endif

#define get_1(m)                                                                                                       \
    (m)->msg.buf[(m)->msg.buf_idx];                                                                                    \
    (m)->msg.buf_idx++
#define put_1(m, b)                                                                                                    \
    (m)->msg.buf[(m)->msg.buf_idx] = (b);                                                                              \
    (m)->msg.buf_idx++

#ifdef MBSN_BIG_ENDIAN
#define get_2(m)                                                                                                       \
    (*(uint16_t*) ((m)->msg.buf + (m)->msg.buf_idx));                                                                  \
    (m)->msg.buf_idx += 2
#define put_2(m, w)                                                                                                    \
    (*(uint16_t*) ((m)->msg.buf + (m)->msg.buf_idx)) = (w);                                                            \
    (m)->msg.buf_idx += 2
#else
#define get_2(m)                                                                                                       \
    ((uint16_t) ((m)->msg.buf[(m)->msg.buf_idx + 1])) | (((uint16_t) (m)->msg.buf[(m)->msg.buf_idx] << 8));            \
    (m)->msg.buf_idx += 2
#define put_2(m, w)                                                                                                    \
    (m)->msg.buf[(m)->msg.buf_idx] = ((uint8_t) ((((uint16_t) (w)) & 0xFF00) >> 8));                                   \
    (m)->msg.buf[(m)->msg.buf_idx + 1] = ((uint8_t) (((uint16_t) (w)) & 0x00FF));                                      \
    (m)->msg.buf_idx += 2
#endif


static void msg_buf_reset(mbsn_t* mbsn) {
    mbsn->msg.buf_idx = 0;
}


static void msg_state_reset(mbsn_t* mbsn) {
    msg_buf_reset(mbsn);
    mbsn->msg.unit_id = 0;
    mbsn->msg.fc = 0;
    mbsn->msg.transaction_id = 0;
    mbsn->msg.broadcast = false;
    mbsn->msg.ignored = 0;
}


static void msg_state_req(mbsn_t* mbsn, uint8_t fc) {
    if (mbsn->current_tid == UINT16_MAX)
        mbsn->current_tid = 1;
    else
        mbsn->current_tid++;

    msg_state_reset(mbsn);
    mbsn->msg.unit_id = mbsn->dest_address_rtu;
    mbsn->msg.fc = fc;
    mbsn->msg.transaction_id = mbsn->current_tid;
    if (mbsn->msg.unit_id == 0 && mbsn->platform.transport == MBSN_TRANSPORT_RTU)
        mbsn->msg.broadcast = true;
}


int mbsn_create(mbsn_t* mbsn, const mbsn_platform_conf* platform_conf) {
    if (!mbsn)
        return MBSN_ERROR_INVALID_ARGUMENT;

    memset(mbsn, 0, sizeof(mbsn_t));

    mbsn->byte_timeout_ms = -1;
    mbsn->read_timeout_ms = -1;
    mbsn->byte_spacing_ms = 0;

    if (!platform_conf)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if (platform_conf->transport != MBSN_TRANSPORT_RTU && platform_conf->transport != MBSN_TRANSPORT_TCP)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if (!platform_conf->read_byte || !platform_conf->write_byte || !platform_conf->sleep)
        return MBSN_ERROR_INVALID_ARGUMENT;

    mbsn->platform = *platform_conf;

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_client_create(mbsn_t* mbsn, const mbsn_platform_conf* platform_conf) {
    return mbsn_create(mbsn, platform_conf);
}


mbsn_error mbsn_server_create(mbsn_t* mbsn, uint8_t address_rtu, const mbsn_platform_conf* platform_conf,
                              const mbsn_callbacks* callbacks) {
    if (platform_conf->transport == MBSN_TRANSPORT_RTU && address_rtu == 0)
        return MBSN_ERROR_INVALID_ARGUMENT;

    mbsn_error ret = mbsn_create(mbsn, platform_conf);
    if (ret != MBSN_ERROR_NONE)
        return ret;

    mbsn->address_rtu = address_rtu;
    mbsn->callbacks = *callbacks;

    return MBSN_ERROR_NONE;
}


void mbsn_set_read_timeout(mbsn_t* mbsn, int32_t timeout_ms) {
    mbsn->read_timeout_ms = timeout_ms;
}


void mbsn_set_byte_timeout(mbsn_t* mbsn, int32_t timeout_ms) {
    mbsn->byte_timeout_ms = timeout_ms;
}


void mbsn_set_byte_spacing(mbsn_t* mbsn, uint32_t spacing_ms) {
    mbsn->byte_spacing_ms = spacing_ms;
}


void mbsn_set_destination_rtu_address(mbsn_t* mbsn, uint8_t address) {
    mbsn->dest_address_rtu = address;
}


void mbsn_set_platform_arg(mbsn_t* mbsn, void* arg) {
    mbsn->platform.arg = arg;
}


static uint16_t crc_calc(const uint8_t* data, uint32_t length) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= (uint16_t) data[i];
        for (int j = 8; j != 0; j--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
                crc >>= 1;
        }
    }

    return crc;
}


static mbsn_error recv(mbsn_t* mbsn, uint32_t count) {
    uint32_t r = 0;
    while (r != count) {
        int ret = mbsn->platform.read_byte(mbsn->msg.buf + mbsn->msg.buf_idx + r, mbsn->byte_timeout_ms,
                                           mbsn->platform.arg);
        if (ret == 0) {
            return MBSN_ERROR_TIMEOUT;
        }
        else if (ret != 1) {
            return MBSN_ERROR_TRANSPORT;
        }

        r++;
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error send(mbsn_t* mbsn) {
    uint32_t spacing_ms = 0;
    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU)
        spacing_ms = mbsn->byte_spacing_ms;

    for (int i = 0; i < mbsn->msg.buf_idx; i++) {
        if (spacing_ms != 0)
            mbsn->platform.sleep(spacing_ms, mbsn->platform.arg);

        int ret = mbsn->platform.write_byte(mbsn->msg.buf[i], mbsn->read_timeout_ms, mbsn->platform.arg);
        if (ret == 0) {
            return MBSN_ERROR_TIMEOUT;
        }
        else if (ret != 1) {
            return MBSN_ERROR_TRANSPORT;
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error recv_msg_footer(mbsn_t* mbsn) {
    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        uint16_t crc = crc_calc(mbsn->msg.buf, mbsn->msg.buf_idx);

        mbsn_error err = recv(mbsn, 2);
        if (err != MBSN_ERROR_NONE)
            return err;

        uint16_t recv_crc = get_2(mbsn);

        if (recv_crc != crc)
            return MBSN_ERROR_TRANSPORT;
    }

    DEBUG("\n");

    return MBSN_ERROR_NONE;
}


static mbsn_error recv_msg_header(mbsn_t* mbsn, bool* first_byte_received) {
    // We wait for the read timeout here, just for the first message byte
    int32_t old_byte_timeout = mbsn->byte_timeout_ms;
    mbsn->byte_timeout_ms = mbsn->read_timeout_ms;

    msg_state_reset(mbsn);

    if (first_byte_received)
        *first_byte_received = false;

    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        mbsn_error err = recv(mbsn, 1);

        mbsn->byte_timeout_ms = old_byte_timeout;

        if (err != MBSN_ERROR_NONE)
            return err;

        if (first_byte_received)
            *first_byte_received = true;

        mbsn->msg.unit_id = get_1(mbsn);

        err = recv(mbsn, 1);
        if (err != MBSN_ERROR_NONE)
            return err;

        mbsn->msg.fc = get_1(mbsn);
    }
    else if (mbsn->platform.transport == MBSN_TRANSPORT_TCP) {
        mbsn_error err = recv(mbsn, 1);

        mbsn->byte_timeout_ms = old_byte_timeout;

        if (err != MBSN_ERROR_NONE)
            return err;

        if (first_byte_received)
            *first_byte_received = true;

        // Advance buf_idx
        get_1(mbsn);

        err = recv(mbsn, 7);
        if (err != MBSN_ERROR_NONE)
            return err;

        // Starting over
        msg_buf_reset(mbsn);

        mbsn->msg.transaction_id = get_2(mbsn);
        uint16_t protocol_id = get_2(mbsn);
        uint16_t length = get_2(mbsn);    // We should actually check the length of the request against this value
        mbsn->msg.unit_id = get_1(mbsn);
        mbsn->msg.fc = get_1(mbsn);

        if (protocol_id != 0)
            return MBSN_ERROR_TRANSPORT;

        if (length > 255)
            return MBSN_ERROR_TRANSPORT;
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error recv_req_header(mbsn_t* mbsn, bool* first_byte_received) {
    mbsn_error err = recv_msg_header(mbsn, first_byte_received);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        // Check if request is for us
        if (mbsn->msg.unit_id == MBSN_BROADCAST_ADDRESS)
            mbsn->msg.broadcast = true;
        else if (mbsn->msg.unit_id != mbsn->address_rtu)
            mbsn->msg.ignored = true;
        else
            mbsn->msg.ignored = false;
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error recv_res_header(mbsn_t* mbsn) {
    uint16_t req_transaction_id = mbsn->msg.transaction_id;
    uint8_t req_fc = mbsn->msg.fc;

    mbsn_error err = recv_msg_header(mbsn, NULL);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (mbsn->platform.transport == MBSN_TRANSPORT_TCP) {
        if (mbsn->msg.transaction_id != req_transaction_id)
            return MBSN_ERROR_INVALID_RESPONSE;
    }

    if (mbsn->msg.ignored)
        return MBSN_ERROR_INVALID_RESPONSE;

    if (mbsn->msg.fc != req_fc) {
        if (mbsn->msg.fc - 0x80 == req_fc) {
            err = recv(mbsn, 1);
            if (err != MBSN_ERROR_NONE)
                return err;

            uint8_t exception = get_1(mbsn);
            err = recv_msg_footer(mbsn);
            if (err != MBSN_ERROR_NONE)
                return err;

            if (exception < 1 || exception > 4)
                return MBSN_ERROR_INVALID_RESPONSE;
            else {
                DEBUG("exception %d\n", exception);
                return exception;
            }
        }
        else {
            return MBSN_ERROR_INVALID_RESPONSE;
        }
    }

    DEBUG("MBSN res <- fc %d\t", mbsn->msg.fc);

    return MBSN_ERROR_NONE;
}


static void send_msg_header(mbsn_t* mbsn, uint16_t data_length) {
    msg_buf_reset(mbsn);

    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        put_1(mbsn, mbsn->msg.unit_id);
    }
    else if (mbsn->platform.transport == MBSN_TRANSPORT_TCP) {
        put_2(mbsn, mbsn->msg.transaction_id);
        put_2(mbsn, 0);
        put_2(mbsn, (uint16_t) (1 + 1 + data_length));
        put_1(mbsn, mbsn->msg.unit_id);
    }

    put_1(mbsn, mbsn->msg.fc);
}


static mbsn_error send_msg_footer(mbsn_t* mbsn) {
    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        uint16_t crc = crc_calc(mbsn->msg.buf, mbsn->msg.buf_idx);
        put_2(mbsn, crc);
    }

    mbsn_error err = send(mbsn);

    DEBUG("\n");

    return err;
}


static void send_req_header(mbsn_t* mbsn, uint16_t data_length) {
    send_msg_header(mbsn, data_length);
    DEBUG("MBSN req -> fc %d\t", mbsn->msg.fc);
}


static void send_res_header(mbsn_t* mbsn, uint16_t data_length) {
    send_msg_header(mbsn, data_length);
    DEBUG("MBSN res -> fc %d\t", mbsn->msg.fc);
}


static mbsn_error handle_exception(mbsn_t* mbsn, uint8_t exception) {
    mbsn->msg.fc += 0x80;
    send_msg_header(mbsn, 1);
    put_1(mbsn, exception);

    DEBUG("MBSN res -> exception %d\n", exception);

    return send_msg_footer(mbsn);
}


static mbsn_error handle_read_discrete(mbsn_t* mbsn, mbsn_error (*callback)(uint16_t, uint16_t, mbsn_bitfield)) {
    mbsn_error err = recv(mbsn, 4);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t quantity = get_2(mbsn);

    DEBUG("a %d\tq %d", address, quantity);

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (quantity < 1 || quantity > 2000)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (callback) {
            mbsn_bitfield bf = {0};
            err = callback(address, quantity, bf);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                uint8_t discrete_bytes = (quantity / 8) + 1;
                send_res_header(mbsn, discrete_bytes);

                put_1(mbsn, discrete_bytes);

                DEBUG("b %d\t", discrete_bytes);

                DEBUG("coils ");
                for (int i = 0; i < discrete_bytes; i++) {
                    put_1(mbsn, bf[i]);
                    DEBUG("%d", bf[i]);
                }

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_read_registers(mbsn_t* mbsn, mbsn_error (*callback)(uint16_t, uint16_t, uint16_t*)) {
    mbsn_error err = recv(mbsn, 4);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t quantity = get_2(mbsn);

    DEBUG("a %d\tq %d", address, quantity);

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (quantity < 1 || quantity > 125)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (callback) {
            uint16_t regs[125] = {0};
            err = callback(address, quantity, regs);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                uint8_t regs_bytes = quantity * 2;
                send_res_header(mbsn, regs_bytes);

                put_1(mbsn, regs_bytes);

                DEBUG("b %d\t", regs_bytes);

                DEBUG("regs ");
                for (int i = 0; i < quantity; i++) {
                    put_2(mbsn, regs[i]);
                    DEBUG("%d", regs[i]);
                }

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_read_coils(mbsn_t* mbsn) {
    return handle_read_discrete(mbsn, mbsn->callbacks.read_coils);
}


static mbsn_error handle_read_discrete_inputs(mbsn_t* mbsn) {
    return handle_read_discrete(mbsn, mbsn->callbacks.read_discrete_inputs);
}


static mbsn_error handle_read_holding_registers(mbsn_t* mbsn) {
    return handle_read_registers(mbsn, mbsn->callbacks.read_holding_registers);
}


static mbsn_error handle_read_input_registers(mbsn_t* mbsn) {
    return handle_read_registers(mbsn, mbsn->callbacks.read_input_registers);
}


static mbsn_error handle_write_single_coil(mbsn_t* mbsn) {
    mbsn_error err = recv(mbsn, 4);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t value = get_2(mbsn);

    DEBUG("a %d\tvalue %d", address, value);

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (mbsn->callbacks.write_single_coil) {
            if (value != 0 && value != 0xFF00)
                return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

            err = mbsn->callbacks.write_single_coil(address, value == 0 ? false : true);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                send_res_header(mbsn, 4);

                put_2(mbsn, address);
                put_2(mbsn, value);
                DEBUG("a %d\tvalue %d", address, value);

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_write_single_register(mbsn_t* mbsn) {
    mbsn_error err = recv(mbsn, 4);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t value = get_2(mbsn);

    DEBUG("a %d\tvalue %d", address, value);

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (mbsn->callbacks.write_single_register) {
            err = mbsn->callbacks.write_single_register(address, value);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                send_res_header(mbsn, 4);

                put_2(mbsn, address);
                put_2(mbsn, value);
                DEBUG("a %d\tvalue %d", address, value);

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_write_multiple_coils(mbsn_t* mbsn) {
    mbsn_error err = recv(mbsn, 5);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t quantity = get_2(mbsn);
    uint8_t coils_bytes = get_1(mbsn);

    DEBUG("a %d\tq %d\tb %d\tcoils ", address, quantity, coils_bytes);

    err = recv(mbsn, coils_bytes);
    if (err != MBSN_ERROR_NONE)
        return err;

    mbsn_bitfield coils;
    for (int i = 0; i < coils_bytes; i++) {
        coils[i] = get_1(mbsn);
        DEBUG("%d ", coils[i]);
    }

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (quantity < 1 || quantity > 0x07B0)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (coils_bytes == 0)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((quantity / 8) + 1 != coils_bytes)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (mbsn->callbacks.write_multiple_coils) {
            err = mbsn->callbacks.write_multiple_coils(address, quantity, coils);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                send_res_header(mbsn, 4);

                put_2(mbsn, address);
                put_2(mbsn, quantity);
                DEBUG("a %d\tq %d", address, quantity);

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_write_multiple_registers(mbsn_t* mbsn) {
    mbsn_error err = recv(mbsn, 5);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t address = get_2(mbsn);
    uint16_t quantity = get_2(mbsn);
    uint8_t registers_bytes = get_1(mbsn);

    DEBUG("a %d\tq %d\tb %d\tregs ", address, quantity, registers_bytes);

    err = recv(mbsn, registers_bytes);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint16_t registers[0x007B];
    for (int i = 0; i < registers_bytes / 2; i++) {
        registers[i] = get_2(mbsn);
        DEBUG("%d ", registers[i]);
    }

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.ignored) {
        if (quantity < 1 || quantity > 0x007B)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_ADDRESS);

        if (registers_bytes == 0)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (registers_bytes != quantity * 2)
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_DATA_VALUE);

        if (mbsn->callbacks.write_multiple_registers) {
            err = mbsn->callbacks.write_multiple_registers(address, quantity, registers);
            if (err != MBSN_ERROR_NONE) {
                if (mbsn_error_is_exception(err))
                    return handle_exception(mbsn, err);
                else
                    return handle_exception(mbsn, MBSN_EXCEPTION_SERVER_DEVICE_FAILURE);
            }

            if (!mbsn->msg.broadcast) {
                send_res_header(mbsn, 4);

                put_2(mbsn, address);
                put_2(mbsn, quantity);
                DEBUG("a %d\tq %d", address, quantity);

                err = send_msg_footer(mbsn);
                if (err != MBSN_ERROR_NONE)
                    return err;
            }
        }
        else {
            return handle_exception(mbsn, MBSN_EXCEPTION_ILLEGAL_FUNCTION);
        }
    }

    return MBSN_ERROR_NONE;
}


static mbsn_error handle_req_fc(mbsn_t* mbsn) {
    DEBUG("fc %d\t", mbsn->msg.fc);

    mbsn_error err;
    switch (mbsn->msg.fc) {
        case 1:
            err = handle_read_coils(mbsn);
            break;

        case 2:
            err = handle_read_discrete_inputs(mbsn);
            break;

        case 3:
            err = handle_read_holding_registers(mbsn);
            break;

        case 4:
            err = handle_read_input_registers(mbsn);
            break;

        case 5:
            err = handle_write_single_coil(mbsn);
            break;

        case 6:
            err = handle_write_single_register(mbsn);
            break;

        case 15:
            err = handle_write_multiple_coils(mbsn);
            break;

        case 16:
            err = handle_write_multiple_registers(mbsn);
            break;

        default:
            err = MBSN_EXCEPTION_ILLEGAL_FUNCTION;
    }

    return err;
}


mbsn_error mbsn_server_poll(mbsn_t* mbsn) {
    msg_state_reset(mbsn);

    bool first_byte_received = false;
    mbsn_error err = recv_req_header(mbsn, &first_byte_received);
    if (err != MBSN_ERROR_NONE) {
        if (!first_byte_received && err == MBSN_ERROR_TIMEOUT)
            return MBSN_ERROR_NONE;
        else
            return err;
    }

#ifdef MBSN_DEBUG
    printf("MBSN req <- ");
    if (mbsn->platform.transport == MBSN_TRANSPORT_RTU) {
        if (mbsn->msg.broadcast)
            printf("broadcast\t");

        printf("client_id %d\t", mbsn->msg.unit_id);
    }
#endif

    err = handle_req_fc(mbsn);
    if (err != MBSN_ERROR_NONE) {
        if (!mbsn_error_is_exception(err))
            return err;
    }

    return err;
}


static mbsn_error read_discrete(mbsn_t* mbsn, uint8_t fc, uint16_t address, uint16_t quantity, mbsn_bitfield values) {
    if (quantity < 1 || quantity > 2000)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
        return MBSN_ERROR_INVALID_ARGUMENT;

    msg_state_req(mbsn, fc);
    send_req_header(mbsn, 4);

    put_2(mbsn, address);
    put_2(mbsn, quantity);

    DEBUG("a %d\tq %d", address, quantity);

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    err = recv_res_header(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    err = recv(mbsn, 1);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint8_t coils_bytes = get_1(mbsn);
    DEBUG("b %d\t", coils_bytes);

    err = recv(mbsn, coils_bytes);
    if (err != MBSN_ERROR_NONE)
        return err;

    DEBUG("coils ");
    for (int i = 0; i < coils_bytes; i++) {
        values[i] = get_1(mbsn);
        DEBUG("%d", values[i]);
    }

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_read_coils(mbsn_t* mbsn, uint16_t address, uint16_t quantity, mbsn_bitfield coils_out) {
    return read_discrete(mbsn, 1, address, quantity, coils_out);
}


mbsn_error mbsn_read_discrete_inputs(mbsn_t* mbsn, uint16_t address, uint16_t quantity, mbsn_bitfield inputs_out) {
    return read_discrete(mbsn, 2, address, quantity, inputs_out);
}


static mbsn_error read_registers(mbsn_t* mbsn, uint8_t fc, uint16_t address, uint16_t quantity, uint16_t* registers) {
    if (quantity < 1 || quantity > 125)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
        return MBSN_ERROR_INVALID_ARGUMENT;

    msg_state_req(mbsn, fc);
    send_req_header(mbsn, 4);

    put_2(mbsn, address);
    put_2(mbsn, quantity);

    DEBUG("a %d\tq %d ", address, quantity);

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    err = recv_res_header(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    err = recv(mbsn, 1);
    if (err != MBSN_ERROR_NONE)
        return err;

    uint8_t registers_bytes = get_1(mbsn);
    DEBUG("b %d\t", registers_bytes);

    err = recv(mbsn, registers_bytes);
    if (err != MBSN_ERROR_NONE)
        return err;

    DEBUG("regs ");
    for (int i = 0; i < registers_bytes / 2; i++) {
        registers[i] = get_2(mbsn);
        DEBUG("%d", registers[i]);
    }

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (registers_bytes != quantity * 2)
        return MBSN_ERROR_INVALID_RESPONSE;

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_read_holding_registers(mbsn_t* mbsn, uint16_t address, uint16_t quantity, uint16_t* registers_out) {
    return read_registers(mbsn, 3, address, quantity, registers_out);
}


mbsn_error mbsn_read_input_registers(mbsn_t* mbsn, uint16_t address, uint16_t quantity, uint16_t* registers_out) {
    return read_registers(mbsn, 4, address, quantity, registers_out);
}


mbsn_error mbsn_write_single_coil(mbsn_t* mbsn, uint16_t address, bool value) {
    msg_state_req(mbsn, 5);
    send_req_header(mbsn, 4);

    uint16_t value_req = value ? 0xFF00 : 0;

    put_2(mbsn, address);
    put_2(mbsn, value_req);

    DEBUG("a %d\tvalue %d ", address, value_req);

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.broadcast) {
        err = recv_res_header(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        err = recv(mbsn, 4);
        if (err != MBSN_ERROR_NONE)
            return err;

        uint16_t address_res = get_2(mbsn);
        uint16_t value_res = get_2(mbsn);

        DEBUG("a %d\tvalue %d", address, value_res);

        err = recv_msg_footer(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        if (address_res != address)
            return MBSN_ERROR_INVALID_RESPONSE;

        if (value_res != value_req)
            return MBSN_ERROR_INVALID_RESPONSE;
    }

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_write_single_register(mbsn_t* mbsn, uint16_t address, uint16_t value) {
    msg_state_req(mbsn, 6);
    send_req_header(mbsn, 4);

    put_2(mbsn, address);
    put_2(mbsn, value);

    DEBUG("a %d\tvalue %d", address, value);

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.broadcast) {
        err = recv_res_header(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        err = recv(mbsn, 4);
        if (err != MBSN_ERROR_NONE)
            return err;

        uint16_t address_res = get_2(mbsn);
        uint16_t value_res = get_2(mbsn);
        DEBUG("a %d\tvalue %d ", address, value_res);

        err = recv_msg_footer(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        if (address_res != address)
            return MBSN_ERROR_INVALID_RESPONSE;

        if (value_res != value)
            return MBSN_ERROR_INVALID_RESPONSE;
    }

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_write_multiple_coils(mbsn_t* mbsn, uint16_t address, uint16_t quantity, const mbsn_bitfield coils) {
    if (quantity < 1 || quantity > 0x07B0)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
        return MBSN_ERROR_INVALID_ARGUMENT;

    uint8_t coils_bytes = (quantity / 8) + 1;

    msg_state_req(mbsn, 15);
    send_req_header(mbsn, 5 + coils_bytes);

    put_2(mbsn, address);
    put_2(mbsn, quantity);
    put_1(mbsn, coils_bytes);
    DEBUG("a %d\tq %d\tb %d\t", address, quantity, coils_bytes);

    DEBUG("coils ");
    for (int i = 0; i < coils_bytes; i++) {
        put_1(mbsn, coils[i]);
        DEBUG("%d ", coils[i]);
    }

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.broadcast) {
        err = recv_res_header(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        err = recv(mbsn, 4);
        if (err != MBSN_ERROR_NONE)
            return err;

        uint16_t address_res = get_2(mbsn);
        uint16_t quantity_res = get_2(mbsn);
        DEBUG("a %d\tq %d", address_res, quantity_res);

        err = recv_msg_footer(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        if (address_res != address)
            return MBSN_ERROR_INVALID_RESPONSE;

        if (quantity_res != quantity)
            return MBSN_ERROR_INVALID_RESPONSE;
    }

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_write_multiple_registers(mbsn_t* mbsn, uint16_t address, uint16_t quantity, const uint16_t* registers) {
    if (quantity < 1 || quantity > 0x007B)
        return MBSN_ERROR_INVALID_ARGUMENT;

    if ((uint32_t) address + (uint32_t) quantity > 0xFFFF + 1)
        return MBSN_ERROR_INVALID_ARGUMENT;

    uint8_t registers_bytes = quantity * 2;

    msg_state_req(mbsn, 16);
    send_req_header(mbsn, 5 + registers_bytes);

    put_2(mbsn, address);
    put_2(mbsn, quantity);
    put_1(mbsn, registers_bytes);
    DEBUG("a %d\tq %d\tb %d\t", address, quantity, registers_bytes);

    DEBUG("regs ");
    for (int i = 0; i < quantity; i++) {
        put_2(mbsn, registers[i]);
        DEBUG("%d ", registers[i]);
    }

    mbsn_error err = send_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    if (!mbsn->msg.broadcast) {
        err = recv_res_header(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        err = recv(mbsn, 4);
        if (err != MBSN_ERROR_NONE)
            return err;

        uint16_t address_res = get_2(mbsn);
        uint16_t quantity_res = get_2(mbsn);
        DEBUG("a %d\tq %d", address_res, quantity_res);

        err = recv_msg_footer(mbsn);
        if (err != MBSN_ERROR_NONE)
            return err;

        if (address_res != address)
            return MBSN_ERROR_INVALID_RESPONSE;

        if (quantity_res != quantity)
            return MBSN_ERROR_INVALID_RESPONSE;
    }

    return MBSN_ERROR_NONE;
}


mbsn_error mbsn_send_raw_pdu(mbsn_t* mbsn, uint8_t fc, const void* data, uint32_t data_len) {
    msg_state_req(mbsn, fc);
    send_msg_header(mbsn, data_len);

    DEBUG("raw ");
    for (uint32_t i = 0; i < data_len; i++) {
        put_1(mbsn, ((uint8_t*) (data))[i]);
        DEBUG("%d ", ((uint8_t*) (data))[i]);
    }

    return send_msg_footer(mbsn);
}


mbsn_error mbsn_receive_raw_pdu_response(mbsn_t* mbsn, void* data_out, uint32_t data_out_len) {
    mbsn_error err = recv_res_header(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    err = recv(mbsn, data_out_len);
    if (err != MBSN_ERROR_NONE)
        return err;

    for (uint32_t i = 0; i < data_out_len; i++) {
        ((uint8_t*) (data_out))[i] = get_1(mbsn);
    }

    err = recv_msg_footer(mbsn);
    if (err != MBSN_ERROR_NONE)
        return err;

    return MBSN_ERROR_NONE;
}


#ifndef MBSN_STRERROR_DISABLED
const char* mbsn_strerror(mbsn_error error) {
    switch (error) {
        case MBSN_ERROR_TRANSPORT:
            return "transport error";

        case MBSN_ERROR_TIMEOUT:
            return "timeout";

        case MBSN_ERROR_INVALID_RESPONSE:
            return "invalid response received";

        case MBSN_ERROR_INVALID_ARGUMENT:
            return "invalid argument provided";

        case MBSN_ERROR_NONE:
            return "no error";

        case MBSN_EXCEPTION_ILLEGAL_FUNCTION:
            return "modbus exception 1: illegal function";

        case MBSN_EXCEPTION_ILLEGAL_DATA_ADDRESS:
            return "modbus exception 2: illegal data address";

        case MBSN_EXCEPTION_ILLEGAL_DATA_VALUE:
            return "modbus exception 3: data value";

        case MBSN_EXCEPTION_SERVER_DEVICE_FAILURE:
            return "modbus exception 4: server device failure";

        default:
            return "unknown error";
    }
}
#endif
