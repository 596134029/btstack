/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#include <stdint.h>
#include <string.h>
#include "a2dp.h"
#include "l2cap.h"
#include "classic/sdp_util.h"
#include "classic/avdtp_source.h"
#include "classic/a2dp_source.h"
#include "btstack_event.h"
#include "bluetooth_sdp.h"
#include "bluetooth_psm.h"

#define BTSTACK_FILE__ "a2dp.c"

#include <stddef.h>
#include "bluetooth.h"
#include "classic/a2dp.h"
#include "classic/avdtp_util.h"
#include "btstack_debug.h"

#define AVDTP_MAX_SEP_NUM 10
#define A2DP_SET_CONFIG_DELAY_MS 200

static void a2dp_discover_seps_with_next_waiting_connection(void);

// higher layer callbacks
static btstack_packet_handler_t a2dp_source_callback;

// config process - singletons using sep_discovery_cid is used as mutex
static uint16_t                 a2dp_source_sep_discovery_cid;
static uint16_t                 a2dp_source_sep_discovery_count;
static uint16_t                 a2dp_source_sep_discovery_index;
static avdtp_sep_t              a2dp_source_sep_discovery_seps[AVDTP_MAX_SEP_NUM];
static btstack_timer_source_t   a2dp_source_set_config_timer;
static bool                     a2dp_source_set_config_timer_active;

void a2dp_init(void) {
}

void a2dp_deinit(void){
    a2dp_source_sep_discovery_cid = 0;
}


void a2dp_create_sdp_record(uint8_t * service,  uint32_t service_record_handle, uint16_t service_class_uuid, uint16_t supported_features, const char * service_name, const char * service_provider_name){
    uint8_t* attribute;
    de_create_sequence(service);

    // 0x0000 "Service Record Handle"
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(service, DE_UINT, DE_SIZE_32, service_record_handle);

    // 0x0001 "Service Class ID List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16, service_class_uuid);
    }
    de_pop_sequence(service, attribute);

    // 0x0004 "Protocol Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t* l2cpProtocol = de_push_sequence(attribute);
        {
            de_add_number(l2cpProtocol,  DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_L2CAP);
            de_add_number(l2cpProtocol,  DE_UINT, DE_SIZE_16, BLUETOOTH_PSM_AVDTP);
        }
        de_pop_sequence(attribute, l2cpProtocol);

        uint8_t* avProtocol = de_push_sequence(attribute);
        {
            de_add_number(avProtocol,  DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_AVDTP);  // avProtocol_service
            de_add_number(avProtocol,  DE_UINT, DE_SIZE_16,  0x0103);  // version
        }
        de_pop_sequence(attribute, avProtocol);
    }
    de_pop_sequence(service, attribute);

    // 0x0005 "Public Browse Group"
    de_add_number(service,  DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST); // public browse group
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute,  DE_UUID, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
    }
    de_pop_sequence(service, attribute);

    // 0x0009 "Bluetooth Profile Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t *a2dProfile = de_push_sequence(attribute);
        {
            de_add_number(a2dProfile,  DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_ADVANCED_AUDIO_DISTRIBUTION);
            de_add_number(a2dProfile,  DE_UINT, DE_SIZE_16, 0x0103);
        }
        de_pop_sequence(attribute, a2dProfile);
    }
    de_pop_sequence(service, attribute);


    // 0x0100 "Service Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0100);
    de_add_data(service,  DE_STRING, strlen(service_name), (uint8_t *) service_name);

    // 0x0100 "Provider Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0102);
    de_add_data(service,  DE_STRING, strlen(service_provider_name), (uint8_t *) service_provider_name);

    // 0x0311 "Supported Features"
    de_add_number(service, DE_UINT, DE_SIZE_16, 0x0311);
    de_add_number(service, DE_UINT, DE_SIZE_16, supported_features);
}

void a2dp_register_source_packet_handler(btstack_packet_handler_t callback){
    btstack_assert(callback != NULL);
    a2dp_source_callback = callback;
}

void a2dp_emit_source(uint8_t * packet, uint16_t size){
    (*a2dp_source_callback)(HCI_EVENT_PACKET, 0, packet, size);
}

void a2dp_replace_subevent_id_and_emit_source(uint8_t * packet, uint16_t size, uint8_t subevent_id) {
    a2dp_replace_subevent_id_and_emit_cmd(a2dp_source_callback, packet, size, subevent_id);
}

void a2dp_emit_source_stream_event(uint16_t cid, uint8_t local_seid, uint8_t subevent_id) {
    a2dp_emit_stream_event(a2dp_source_callback, cid, local_seid, subevent_id);
}

void a2dp_emit_source_streaming_connection_failed(avdtp_connection_t *connection, uint8_t status) {
    uint8_t event[14];
    int pos = 0;
    event[pos++] = HCI_EVENT_A2DP_META;
    event[pos++] = sizeof(event) - 2;
    event[pos++] = A2DP_SUBEVENT_STREAM_ESTABLISHED;
    little_endian_store_16(event, pos, connection->avdtp_cid);
    pos += 2;
    reverse_bd_addr(connection->remote_addr, &event[pos]);
    pos += 6;
    event[pos++] = 0;
    event[pos++] = 0;
    event[pos++] = status;
    a2dp_emit_source(event, sizeof(event));
}

void a2dp_emit_source_stream_reconfigured(uint16_t cid, uint8_t local_seid, uint8_t status){
    uint8_t event[7];
    int pos = 0;
    event[pos++] = HCI_EVENT_A2DP_META;
    event[pos++] = sizeof(event) - 2;
    event[pos++] = A2DP_SUBEVENT_STREAM_RECONFIGURED;
    little_endian_store_16(event, pos, cid);
    pos += 2;
    event[pos++] = local_seid;
    event[pos++] = status;
    a2dp_emit_source(event, sizeof(event));
}

static void a2dp_source_set_config_timer_handler(btstack_timer_source_t * timer){
    uint16_t avdtp_cid = (uint16_t)(uintptr_t) btstack_run_loop_get_timer_context(timer);
    avdtp_connection_t * connection = avdtp_get_connection_for_avdtp_cid(avdtp_cid);
    btstack_run_loop_set_timer_context(&a2dp_source_set_config_timer, NULL);
    a2dp_source_set_config_timer_active = false;

    log_info("Set Config timer fired, avdtp_cid 0x%02x", avdtp_cid);

    if (connection == NULL) {
        a2dp_discover_seps_with_next_waiting_connection();
        return;
    }

    if (connection->a2dp_source_config_process.stream_endpoint_configured) return;
    avdtp_source_discover_stream_endpoints(avdtp_cid);
}

static void a2dp_source_set_config_timer_start(uint16_t avdtp_cid){
    log_info("Set Config timer start for cid 0%02x", avdtp_cid);
    a2dp_source_set_config_timer_active = true;
    btstack_run_loop_remove_timer(&a2dp_source_set_config_timer);
    btstack_run_loop_set_timer_handler(&a2dp_source_set_config_timer,a2dp_source_set_config_timer_handler);
    btstack_run_loop_set_timer(&a2dp_source_set_config_timer, A2DP_SET_CONFIG_DELAY_MS);
    btstack_run_loop_set_timer_context(&a2dp_source_set_config_timer, (void *)(uintptr_t)avdtp_cid);
    btstack_run_loop_add_timer(&a2dp_source_set_config_timer);
}

static void a2dp_source_set_config_timer_restart(void){
    log_info("Set Config timer restart");
    btstack_run_loop_remove_timer(&a2dp_source_set_config_timer);
    btstack_run_loop_set_timer(&a2dp_source_set_config_timer, A2DP_SET_CONFIG_DELAY_MS);
    btstack_run_loop_add_timer(&a2dp_source_set_config_timer);
}

static void a2dp_source_set_config_timer_stop(void){
    if (a2dp_source_set_config_timer_active == false) return;
    log_info("Set Config timer stop");
    btstack_run_loop_remove_timer(&a2dp_source_set_config_timer);
    btstack_run_loop_set_timer_context(&a2dp_source_set_config_timer, NULL);
    a2dp_source_set_config_timer_active = false;
}

// Discover seps, both incoming and outgoing
static void a2dp_start_discovering_seps(avdtp_connection_t * connection){
    connection->a2dp_source_config_process.state = A2DP_DISCOVER_SEPS;
    connection->a2dp_source_config_process.discover_seps = false;

    a2dp_source_sep_discovery_index = 0;
    a2dp_source_sep_discovery_count = 0;
    memset(a2dp_source_sep_discovery_seps, 0, sizeof(avdtp_sep_t) * AVDTP_MAX_SEP_NUM);
    a2dp_source_sep_discovery_cid = connection->avdtp_cid;

    // if we initiated the connection, start config right away, else wait a bit to give remote a chance to do it first
    if (connection->a2dp_source_config_process.outgoing_active){
        log_info("discover seps");
        avdtp_source_discover_stream_endpoints(connection->avdtp_cid);
    } else {
        log_info("wait a bit, then discover seps");
        a2dp_source_set_config_timer_start(connection->avdtp_cid);
    }
}

static void a2dp_discover_seps_with_next_waiting_connection(void){
    btstack_assert(a2dp_source_sep_discovery_cid == 0);
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, avdtp_get_connections());
    while (btstack_linked_list_iterator_has_next(&it)){
        avdtp_connection_t * next_connection = (avdtp_connection_t *)btstack_linked_list_iterator_next(&it);
        if (!next_connection->a2dp_source_config_process.discover_seps) continue;
        a2dp_start_discovering_seps(next_connection);
    }
}

void a2dp_source_ready_for_sep_discovery(avdtp_connection_t * connection){
    // start discover seps now if:
    // - outgoing active: signaling for outgoing connection
    // - outgoing not active: incoming connection and no sep discover ongoing

    // sep discovery active?
    if (a2dp_source_sep_discovery_cid == 0){
        a2dp_start_discovering_seps(connection);
    } else {
        // post-pone sep discovery
        connection->a2dp_source_config_process.discover_seps = true;
    }
}

static void a2dp_handle_received_configuration(const uint8_t *packet, uint8_t local_seid) {
    uint16_t cid = avdtp_subevent_signaling_media_codec_sbc_configuration_get_avdtp_cid(packet);
    avdtp_connection_t *avdtp_connection = avdtp_get_connection_for_avdtp_cid(cid);
    btstack_assert(avdtp_connection != NULL);
    avdtp_connection->a2dp_source_config_process.local_stream_endpoint = avdtp_get_stream_endpoint_for_seid(local_seid);
    // bail out if local seid invalid
    if (!avdtp_connection->a2dp_source_config_process.local_stream_endpoint) return;

    // stop timer
    if (a2dp_source_sep_discovery_cid == cid) {
        a2dp_source_set_config_timer_stop();
        a2dp_source_sep_discovery_cid = 0;
    }

    avdtp_connection->a2dp_source_config_process.stream_endpoint_configured = true;

    switch (avdtp_connection->a2dp_source_config_process.state) {
        case A2DP_W4_SET_CONFIGURATION:
            // outgoing: discovery and config of remote sink sep successful, trigger stream open
            avdtp_connection->a2dp_source_config_process.state = A2DP_W2_OPEN_STREAM_WITH_SEID;
            break;
        case A2DP_DISCOVER_SEPS:
        case A2DP_GET_CAPABILITIES:
        case A2DP_W2_GET_ALL_CAPABILITIES:
        case A2DP_DISCOVERY_DONE:
        case A2DP_W4_GET_CONFIGURATION:
            // incoming: wait for stream open
            avdtp_connection->a2dp_source_config_process.state = A2DP_W4_OPEN_STREAM_WITH_SEID;
            break;
        default:
            // wait for configuration after sending reconfigure - keep state
            break;
    }
}

static void a2dp_source_set_config(avdtp_connection_t * connection){
    uint8_t remote_seid = connection->a2dp_source_config_process.local_stream_endpoint->set_config_remote_seid;
    log_info("A2DP initiate set configuration locally and wait for response ... local seid 0x%02x, remote seid 0x%02x",
             avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint), remote_seid);
    connection->a2dp_source_config_process.state = A2DP_W4_SET_CONFIGURATION;
    avdtp_source_set_configuration(connection->avdtp_cid,
                                   avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                                   remote_seid,
                                   connection->a2dp_source_config_process.local_stream_endpoint->remote_configuration_bitmap,
                                   connection->a2dp_source_config_process.local_stream_endpoint->remote_configuration);
}

static void a2dp_source_handle_media_capability(uint16_t cid, uint8_t a2dp_subevent_id, uint8_t *packet, uint16_t size){
    avdtp_connection_t * connection = avdtp_get_connection_for_avdtp_cid(cid);
    btstack_assert(connection != NULL);
    if (connection->a2dp_source_config_process.state != A2DP_GET_CAPABILITIES) return;
    a2dp_replace_subevent_id_and_emit_source(packet, size, a2dp_subevent_id);
}

uint8_t a2dp_source_config_init(avdtp_connection_t *connection, uint8_t local_seid, uint8_t remote_seid,
                                       avdtp_media_codec_type_t codec_type) {

    // check state
    switch (connection->a2dp_source_config_process.state){
        case A2DP_DISCOVERY_DONE:
        case A2DP_GET_CAPABILITIES:
            break;
        default:
            return ERROR_CODE_COMMAND_DISALLOWED;
    }

    // lookup local stream endpoint
    avdtp_stream_endpoint_t * stream_endpoint = avdtp_get_stream_endpoint_for_seid(local_seid);
    if (stream_endpoint == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    // lookup remote stream endpoint
    avdtp_sep_t * remote_sep = NULL;
    uint8_t i;
    for (i=0; i < a2dp_source_sep_discovery_count; i++){
        if (a2dp_source_sep_discovery_seps[i].seid == remote_seid){
            remote_sep = &a2dp_source_sep_discovery_seps[i];
        }
    }
    if (remote_sep == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    // set media configuration
    stream_endpoint->remote_configuration_bitmap = store_bit16(stream_endpoint->remote_configuration_bitmap, AVDTP_MEDIA_CODEC, 1);
    stream_endpoint->remote_configuration.media_codec.media_type = AVDTP_AUDIO;
    stream_endpoint->remote_configuration.media_codec.media_codec_type = codec_type;
    // remote seid to use
    stream_endpoint->set_config_remote_seid = remote_seid;
    // enable delay reporting if supported
    if (remote_sep->registered_service_categories & (1<<AVDTP_DELAY_REPORTING)){
        stream_endpoint->remote_configuration_bitmap = store_bit16(stream_endpoint->remote_configuration_bitmap, AVDTP_DELAY_REPORTING, 1);
    }

    // suitable Sink stream endpoint found, configure it
    connection->a2dp_source_config_process.local_stream_endpoint = stream_endpoint;
    connection->a2dp_source_config_process.have_config = true;

#ifdef ENABLE_A2DP_SOURCE_EXPLICIT_CONFIG
    // continue outgoing configuration
    if (connection->a2dp_source_state == A2DP_DISCOVERY_DONE){
        connection->a2dp_source_state = A2DP_SET_CONFIGURATION;
    }
#endif

    return ERROR_CODE_SUCCESS;
}
void a2dp_source_config_process_avdtp_event_handler(uint8_t *packet, uint16_t size) {
    uint16_t cid;
    avdtp_connection_t * connection;
    uint8_t signal_identifier;
    uint8_t status;
    uint8_t local_seid;
    uint8_t remote_seid;

    switch (hci_event_avdtp_meta_get_subevent_code(packet)){
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            status = avdtp_subevent_signaling_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                // notify about connection error only if we're initiator
                if (connection->a2dp_source_config_process.outgoing_active){
                    log_info("A2DP source signaling connection failed status 0x%02x", status);
                    connection->a2dp_source_config_process.outgoing_active = false;
                    a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED);
                }
                break;
            }
            log_info("A2DP source signaling connection established avdtp_cid 0x%02x", cid);
            connection->a2dp_source_config_process.state = A2DP_CONNECTED;

            // notify app
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED);

            // Windows 10 as Source starts SEP discovery after 1500 ms, but only if it did not get a Discover command
            // If BTstack is configured for both roles, we need to avoid sending Discover command in Source Role for outgoing Sink connections

            // For this, we trigger SDP query only if:
            // a) this is an outgoing source connection
            // b) this connection wasn't caused by an outgoing sink request
            if (connection->a2dp_source_config_process.outgoing_active || !connection->a2dp_sink_config_process.outgoing_active){
                a2dp_source_ready_for_sep_discovery(connection);
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_FOUND:
            cid = avdtp_subevent_signaling_sep_found_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (connection->a2dp_source_config_process.state == A2DP_DISCOVER_SEPS) {
                avdtp_sep_t sep;
                memset(&sep, 0, sizeof(avdtp_sep_t));
                sep.seid       = avdtp_subevent_signaling_sep_found_get_remote_seid(packet);;
                sep.in_use     = avdtp_subevent_signaling_sep_found_get_in_use(packet);
                sep.media_type = (avdtp_media_type_t) avdtp_subevent_signaling_sep_found_get_media_type(packet);
                sep.type       = (avdtp_sep_type_t) avdtp_subevent_signaling_sep_found_get_sep_type(packet);
                log_info("A2DP Found sep: remote seid 0x%02x, in_use %d, media type %d, sep type %s, index %d",
                         sep.seid, sep.in_use, sep.media_type, sep.type == AVDTP_SOURCE ? "source" : "sink",
                         a2dp_source_sep_discovery_count);
                if ((sep.type == AVDTP_SINK) && (sep.in_use == false)) {
                    a2dp_source_sep_discovery_seps[a2dp_source_sep_discovery_count++] = sep;
                }
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_DICOVERY_DONE:
            cid = avdtp_subevent_signaling_sep_dicovery_done_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (connection->a2dp_source_config_process.state != A2DP_DISCOVER_SEPS) break;

            if (a2dp_source_sep_discovery_count > 0){
                connection->a2dp_source_config_process.state = A2DP_GET_CAPABILITIES;
                a2dp_source_sep_discovery_index = 0;
                connection->a2dp_source_config_process.have_config = false;
            } else {
                if (connection->a2dp_source_config_process.outgoing_active){
                    connection->a2dp_source_config_process.outgoing_active = false;
                    connection = avdtp_get_connection_for_avdtp_cid(cid);
                    btstack_assert(connection != NULL);
                    a2dp_emit_source_streaming_connection_failed(connection,
                                                                 ERROR_CODE_CONNECTION_REJECTED_DUE_TO_NO_SUITABLE_CHANNEL_FOUND);
                }

                // continue
                connection->a2dp_source_config_process.state = A2DP_CONNECTED;
                a2dp_source_sep_discovery_cid = 0;
                a2dp_discover_seps_with_next_waiting_connection();
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            cid = avdtp_subevent_signaling_media_codec_sbc_capability_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (connection->a2dp_source_config_process.state != A2DP_GET_CAPABILITIES) break;

            // forward codec capability
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY);

#ifndef ENABLE_A2DP_SOURCE_EXPLICIT_CONFIG
            // select SEP if none configured yet
            if (connection->a2dp_source_config_process.have_config == false){
                // find SBC stream endpoint
                avdtp_stream_endpoint_t * stream_endpoint = avdtp_get_source_stream_endpoint_for_media_codec(AVDTP_CODEC_SBC);
                if (stream_endpoint != NULL){
                    // choose SBC config params
                    avdtp_configuration_sbc_t configuration;
                    configuration.sampling_frequency = avdtp_choose_sbc_sampling_frequency(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet));
                    configuration.channel_mode       = avdtp_choose_sbc_channel_mode(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet));
                    configuration.block_length       = avdtp_choose_sbc_block_length(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet));
                    configuration.subbands           = avdtp_choose_sbc_subbands(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet));
                    configuration.allocation_method  = avdtp_choose_sbc_allocation_method(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet));
                    configuration.max_bitpool_value  = avdtp_choose_sbc_max_bitpool_value(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet));
                    configuration.min_bitpool_value  = avdtp_choose_sbc_min_bitpool_value(stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet));

                    // and pre-select this endpoint
                    local_seid = avdtp_stream_endpoint_seid(stream_endpoint);
                    remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
                    a2dp_source_set_config_sbc(cid, local_seid, remote_seid, &configuration);
                }
            }
#endif
            break;
            // forward codec capability
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CAPABILITY:
            cid = avdtp_subevent_signaling_media_codec_mpeg_audio_capability_get_avdtp_cid(packet);
            a2dp_source_handle_media_capability(cid, A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CAPABILITY, packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY:
            cid = avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_avdtp_cid(packet);
            a2dp_source_handle_media_capability(cid, A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY, packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CAPABILITY:
            cid = avdtp_subevent_signaling_media_codec_atrac_capability_get_avdtp_cid(packet);
            a2dp_source_handle_media_capability(cid, A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CAPABILITY, packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            cid = avdtp_subevent_signaling_media_codec_other_capability_get_avdtp_cid(packet);
            a2dp_source_handle_media_capability(cid, A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY, packet, size);
            break;

            // not forwarded
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_TRANSPORT_CAPABILITY:
        case AVDTP_SUBEVENT_SIGNALING_REPORTING_CAPABILITY:
        case AVDTP_SUBEVENT_SIGNALING_RECOVERY_CAPABILITY:
        case AVDTP_SUBEVENT_SIGNALING_CONTENT_PROTECTION_CAPABILITY:
        case AVDTP_SUBEVENT_SIGNALING_HEADER_COMPRESSION_CAPABILITY:
        case AVDTP_SUBEVENT_SIGNALING_MULTIPLEXING_CAPABILITY:
            break;

        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY:
            cid = avdtp_subevent_signaling_delay_reporting_capability_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);
            log_info("received AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY, cid 0x%02x, state %d", cid, connection->a2dp_source_config_process.state);

            if (connection->a2dp_source_config_process.state != A2DP_GET_CAPABILITIES) break;

            // store delay reporting capability
            a2dp_source_sep_discovery_seps[a2dp_source_sep_discovery_index].registered_service_categories |= 1 << AVDTP_DELAY_REPORTING;

            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY);
            break;

        case AVDTP_SUBEVENT_SIGNALING_CAPABILITIES_DONE:
            cid = avdtp_subevent_signaling_capabilities_done_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (connection->a2dp_source_config_process.state != A2DP_GET_CAPABILITIES) break;

            // forward capabilities done for endpoint
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_CAPABILITIES_DONE);

            // endpoint was not suitable, check next one
            a2dp_source_sep_discovery_index++;
            if (a2dp_source_sep_discovery_index >= a2dp_source_sep_discovery_count){

                // emit 'all capabilities for all seps reported'
                uint8_t event[6];
                uint8_t pos = 0;
                event[pos++] = HCI_EVENT_A2DP_META;
                event[pos++] = sizeof(event) - 2;
                event[pos++] = A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE;
                little_endian_store_16(event, pos, cid);
                a2dp_emit_source(event, sizeof(event));

                // do we have a valid config?
                if (connection->a2dp_source_config_process.have_config){
                    connection->a2dp_source_config_process.state = A2DP_SET_CONFIGURATION;
                    connection->a2dp_source_config_process.have_config = false;
                    break;
                }

#ifdef ENABLE_A2DP_SOURCE_EXPLICIT_CONFIG
                connection->a2dp_source_state = A2DP_DISCOVERY_DONE;
                // TODO call a2dp_discover_seps_with_next_waiting_connection?
                break;
#else
                // we didn't find a suitable SBC stream endpoint, sorry.
                if (connection->a2dp_source_config_process.outgoing_active){
                    connection->a2dp_source_config_process.outgoing_active = false;
                    connection = avdtp_get_connection_for_avdtp_cid(cid);
                    btstack_assert(connection != NULL);
                    a2dp_emit_source_streaming_connection_failed(connection,
                                                                 ERROR_CODE_CONNECTION_REJECTED_DUE_TO_NO_SUITABLE_CHANNEL_FOUND);
                }
                connection->a2dp_source_config_process.state = A2DP_CONNECTED;
                a2dp_source_sep_discovery_cid = 0;
                a2dp_discover_seps_with_next_waiting_connection();
#endif
            }
            break;

            // forward codec configuration
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            local_seid = avdtp_subevent_signaling_media_codec_sbc_configuration_get_local_seid(packet);
            a2dp_handle_received_configuration(packet, local_seid);
            a2dp_replace_subevent_id_and_emit_source(packet, size,
                                                     A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CONFIGURATION:
            local_seid = avdtp_subevent_signaling_media_codec_mpeg_audio_configuration_get_local_seid(packet);
            a2dp_handle_received_configuration(packet, local_seid);
            a2dp_replace_subevent_id_and_emit_source(packet, size,
                                                     A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CONFIGURATION);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION:
            local_seid = avdtp_subevent_signaling_media_codec_mpeg_aac_configuration_get_local_seid(packet);
            a2dp_handle_received_configuration(packet, local_seid);
            a2dp_replace_subevent_id_and_emit_source(packet, size,
                                                     A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CONFIGURATION:
            local_seid = avdtp_subevent_signaling_media_codec_atrac_configuration_get_local_seid(packet);
            a2dp_handle_received_configuration(packet, local_seid);
            a2dp_replace_subevent_id_and_emit_source(packet, size,
                                                     A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CONFIGURATION);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            local_seid = avdtp_subevent_signaling_media_codec_sbc_configuration_get_local_seid(packet);
            a2dp_handle_received_configuration(packet, local_seid);
            a2dp_replace_subevent_id_and_emit_source(packet, size,
                                                     A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION);
            break;
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED:
            cid = avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (connection->a2dp_source_config_process.state != A2DP_W4_OPEN_STREAM_WITH_SEID) break;

            connection->a2dp_source_config_process.outgoing_active = false;
            status = avdtp_subevent_streaming_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                log_info("A2DP source streaming connection could not be established, avdtp_cid 0x%02x, status 0x%02x ---", cid, status);
                a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_STREAM_ESTABLISHED);
                break;
            }

            log_info("A2DP source streaming connection established --- avdtp_cid 0x%02x, local seid 0x%02x, remote seid 0x%02x", cid,
                     avdtp_subevent_streaming_connection_established_get_local_seid(packet),
                     avdtp_subevent_streaming_connection_established_get_remote_seid(packet));
            connection->a2dp_source_config_process.state = A2DP_STREAMING_OPENED;
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_STREAM_ESTABLISHED);
            break;

        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
            cid = avdtp_subevent_signaling_accept_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            // restart set config timer while remote is active for current cid
            if (a2dp_source_set_config_timer_active &&
                (avdtp_subevent_signaling_accept_get_is_initiator(packet) == 0) &&
                (cid == a2dp_source_sep_discovery_cid)){

                a2dp_source_set_config_timer_restart();
                break;
            }

            signal_identifier = avdtp_subevent_signaling_accept_get_signal_identifier(packet);

            log_info("A2DP cmd %s accepted, global state %d, cid 0x%02x", avdtp_si2str(signal_identifier), connection->a2dp_source_config_process.state, cid);

            switch (connection->a2dp_source_config_process.state){
                case A2DP_GET_CAPABILITIES:
                    remote_seid = a2dp_source_sep_discovery_seps[a2dp_source_sep_discovery_index].seid;
                    log_info("A2DP get capabilities for remote seid 0x%02x", remote_seid);
                    avdtp_source_get_all_capabilities(cid, remote_seid);
                    return;

                case A2DP_SET_CONFIGURATION:
                    a2dp_source_set_config(connection);
                    return;

                case A2DP_W2_OPEN_STREAM_WITH_SEID:
                    log_info("A2DP open stream ... local seid 0x%02x, active remote seid 0x%02x",
                             avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                             connection->a2dp_source_config_process.local_stream_endpoint->remote_sep.seid);
                    connection->a2dp_source_config_process.state = A2DP_W4_OPEN_STREAM_WITH_SEID;
                    avdtp_source_open_stream(cid,
                                             avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                                             connection->a2dp_source_config_process.local_stream_endpoint->remote_sep.seid);
                    break;

                case A2DP_W2_RECONFIGURE_WITH_SEID:
                    log_info("A2DP reconfigured ... local seid 0x%02x, active remote seid 0x%02x",
                             avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                             connection->a2dp_source_config_process.local_stream_endpoint->remote_sep.seid);
                    a2dp_emit_source_stream_reconfigured(cid, avdtp_stream_endpoint_seid(
                            connection->a2dp_source_config_process.local_stream_endpoint), ERROR_CODE_SUCCESS);
                    connection->a2dp_source_config_process.state = A2DP_STREAMING_OPENED;
                    break;

                case A2DP_STREAMING_OPENED:
                    switch (signal_identifier){
                        case  AVDTP_SI_START:
                            a2dp_emit_source_stream_event(cid, avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                                                          A2DP_SUBEVENT_STREAM_STARTED);
                            break;
                        case AVDTP_SI_SUSPEND:
                            a2dp_emit_source_stream_event(cid, avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                                                          A2DP_SUBEVENT_STREAM_SUSPENDED);
                            break;
                        case AVDTP_SI_ABORT:
                        case AVDTP_SI_CLOSE:
                            a2dp_emit_source_stream_event(cid, avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                                                          A2DP_SUBEVENT_STREAM_STOPPED);
                            break;
                        default:
                            break;
                    }
                    break;

                default:
                    break;
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_REJECT:
            cid = avdtp_subevent_signaling_reject_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (avdtp_subevent_signaling_reject_get_is_initiator(packet) == 0) break;

            switch (connection->a2dp_source_config_process.state) {
                case A2DP_W2_RECONFIGURE_WITH_SEID:
                    log_info("A2DP reconfigure failed ... local seid 0x%02x, active remote seid 0x%02x",
                             avdtp_stream_endpoint_seid(connection->a2dp_source_config_process.local_stream_endpoint),
                             connection->a2dp_source_config_process.local_stream_endpoint->remote_sep.seid);
                    a2dp_emit_source_stream_reconfigured(cid, avdtp_stream_endpoint_seid(
                            connection->a2dp_source_config_process.local_stream_endpoint), ERROR_CODE_UNSPECIFIED_ERROR);
                    connection->a2dp_source_config_process.state = A2DP_STREAMING_OPENED;
                    break;
                default:
                    connection->a2dp_source_config_process.state = A2DP_CONNECTED;
                    break;
            }

            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_COMMAND_REJECTED);
            break;

        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT:
            cid = avdtp_subevent_signaling_general_reject_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            if (avdtp_subevent_signaling_general_reject_get_is_initiator(packet) == 0) break;

            connection->a2dp_source_config_process.state = A2DP_CONNECTED;
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_COMMAND_REJECTED);
            break;

        case AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED:
            cid = avdtp_subevent_streaming_connection_released_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            connection->a2dp_source_config_process.state = A2DP_CONFIGURED;
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_STREAM_RELEASED);
            break;

        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            cid = avdtp_subevent_signaling_connection_released_get_avdtp_cid(packet);
            connection = avdtp_get_connection_for_avdtp_cid(cid);
            btstack_assert(connection != NULL);

            // connect/release are passed on to app
            if (a2dp_source_sep_discovery_cid == cid){
                a2dp_source_set_config_timer_stop();
                connection->a2dp_source_config_process.stream_endpoint_configured = false;
                connection->a2dp_source_config_process.local_stream_endpoint = NULL;

                connection->a2dp_source_config_process.state = A2DP_IDLE;
                a2dp_source_sep_discovery_cid = 0;
            }
            a2dp_replace_subevent_id_and_emit_source(packet, size, A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED);
            break;

        default:
            break;
    }
}
