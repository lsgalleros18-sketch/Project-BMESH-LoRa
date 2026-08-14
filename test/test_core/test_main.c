#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <unity.h>

#include "lwip/inet.h"

#include "mesh_protocol.h"
#include "app/app_runtime.h"
#include "bems_crypto.h"
#include "mesh_control.h"
#include "mesh/mesh_sequence.h"
#include "mesh/replay_protection.h"
#include "mesh/tx_scheduler.h"
#include "network/dns_server.h"
#include "messages/message_store.h"
#include "route_table.h"
#include "roster.h"
#include "utils/string_utils.h"

static uint32_t fake_tick;
static uint32_t fake_roster_now_ms;

void setUp(void)
{
    fake_tick = 1000;
    fake_roster_now_ms = 1000;
    replay_protection_reset();
    route_table_debug_reset_for_test();
    deduplication_debug_reset_for_test();
}

void tearDown(void)
{
}

static emergency_message_t make_tx_message(uint32_t id, const char *source, const char *destination, const char *priority)
{
    emergency_message_t message = {0};

    message.id = id;
    copy_field(message.direction, sizeof(message.direction), "TX");
    copy_field(message.source, sizeof(message.source), source);
    copy_field(message.destination, sizeof(message.destination), destination);
    copy_field(message.type, sizeof(message.type), "FLOOD");
    copy_field(message.priority, sizeof(message.priority), priority);
    copy_field(message.payload, sizeof(message.payload), "payload");
    copy_field(message.status, sizeof(message.status), "PENDING");
    copy_field(message.packet, sizeof(message.packet), "BEMS|1|SRC|DST|FLOOD|HIGH|HOPS=1|RELAY=SRC|LOC=A|payload");
    return message;
}

static emergency_message_t make_sync_message(uint32_t id, const char *source, const char *destination, const char *type, const char *priority, int hops, const char *payload)
{
    emergency_message_t message = {0};

    message.id = id;
    copy_field(message.direction, sizeof(message.direction), "RX");
    copy_field(message.source, sizeof(message.source), source);
    copy_field(message.destination, sizeof(message.destination), destination);
    copy_field(message.type, sizeof(message.type), type);
    copy_field(message.priority, sizeof(message.priority), priority);
    copy_field(message.payload, sizeof(message.payload), payload);
    message.hops = hops;
    return message;
}

static void test_parse_mesh_packet_valid_packet(void)
{
    mesh_packet_t parsed;

    TEST_ASSERT_TRUE(parse_mesh_packet("BEMS|42|NODE01|ALL|FLOOD|HIGH|HOPS=5|RELAY=1|LOC=Purok 3~San Isidro~Cabuyao|Water rising", &parsed));
    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL_UINT32(42, parsed.id);
    TEST_ASSERT_EQUAL_INT(5, parsed.hops);
    TEST_ASSERT_EQUAL_STRING("NODE01", parsed.source);
    TEST_ASSERT_EQUAL_STRING("ALL", parsed.destination);
    TEST_ASSERT_EQUAL_STRING("FLOOD", parsed.type);
    TEST_ASSERT_EQUAL_STRING("HIGH", parsed.priority);
    TEST_ASSERT_EQUAL_STRING("RELAY=1", parsed.relay);
    TEST_ASSERT_EQUAL_STRING("LOC=Purok 3~San Isidro~Cabuyao", parsed.location_raw);
    TEST_ASSERT_EQUAL_STRING("Purok 3", parsed.location.sitio);
    TEST_ASSERT_EQUAL_STRING("San Isidro", parsed.location.barangay);
    TEST_ASSERT_EQUAL_STRING("Cabuyao", parsed.location.municipality);
    TEST_ASSERT_EQUAL_STRING("ANNOUNCEMENTS", parsed.thread_key);
    TEST_ASSERT_EQUAL_STRING("Water rising", parsed.payload);
}

static void test_parse_mesh_packet_sets_thread_key_for_direct_messages(void)
{
    mesh_packet_t parsed;

    TEST_ASSERT_TRUE(parse_mesh_packet("BEMS|43|NODE01|NODE99|FLOOD|HIGH|HOPS=5|RELAY=1|LOC=Purok 3~San Isidro~Cabuyao|Water rising", &parsed));
    TEST_ASSERT_EQUAL_STRING("NODE01", parsed.thread_key);
}

static void test_packet_seen_uses_source_and_id(void)
{
    remember_packet("NODE01", 10);

    TEST_ASSERT_TRUE(packet_seen("NODE01", 10));
    TEST_ASSERT_FALSE(packet_seen("NODE02", 10));
    TEST_ASSERT_FALSE(packet_seen("NODE01", 11));
}

static void test_packet_seen_expires_after_ttl(void)
{
    remember_packet("NODE01", 10);
    TEST_ASSERT_TRUE(packet_seen("NODE01", 10));
    deduplication_debug_set_seen_tick_for_test("NODE01", 10, 0);
    TEST_ASSERT_FALSE(packet_seen("NODE01", 10));
    (void)fake_tick;
}

static void test_replay_accepts_first_and_sequential_packets(void)
{
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 100, 1));
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 101, 2));
}

static void test_replay_rejects_duplicate(void)
{
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 100, 1));
    TEST_ASSERT_FALSE(replay_protection_accept("NODE01", 100, 2));
}

static void test_replay_accepts_out_of_order_inside_window(void)
{
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 100, 1));
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 102, 2));
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 101, 3));
}

static void test_replay_wraparound(void)
{
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", UINT32_MAX - 1, 1));
    TEST_ASSERT_TRUE(replay_protection_accept("NODE01", 0, 2));
}

static void test_mesh_sequence_advances_monotonically(void)
{
    mesh_sequence_init();
    mesh_sequence_update(41);
    TEST_ASSERT_EQUAL_UINT32(41, mesh_sequence_peek());
    TEST_ASSERT_EQUAL_UINT32(42, mesh_sequence_next());
    TEST_ASSERT_EQUAL_UINT32(42, mesh_sequence_peek());
}

static void test_tx_scheduler_queue_and_ack(void)
{
    emergency_message_t message = make_tx_message(9001, "NODE01", "NODE02", "HIGH");

    tx_scheduler_debug_reset_for_test();
    TEST_ASSERT_TRUE(tx_scheduler_enqueue(&message, 0));
    TEST_ASSERT_EQUAL_UINT(1, tx_scheduler_queue_depth());
    TEST_ASSERT_TRUE(tx_scheduler_acknowledge(9001, "NODE02"));
    TEST_ASSERT_EQUAL_UINT(0, tx_scheduler_queue_depth());
}

static void test_tx_scheduler_rejects_wrong_and_duplicate_ack(void)
{
    emergency_message_t message = make_tx_message(9002, "NODE01", "NODE02", "HIGH");

    tx_scheduler_debug_reset_for_test();
    TEST_ASSERT_TRUE(tx_scheduler_enqueue(&message, 0));
    TEST_ASSERT_FALSE(tx_scheduler_acknowledge(9002, "NODEXX"));
    TEST_ASSERT_TRUE(tx_scheduler_acknowledge(9002, "NODE02"));
    TEST_ASSERT_FALSE(tx_scheduler_acknowledge(9002, "NODE02"));
}

static void test_tx_scheduler_queue_full(void)
{
    emergency_message_t message;

    tx_scheduler_debug_reset_for_test();
    for (uint32_t i = 0; i < MAX_MESSAGES; i++) {
        message = make_tx_message(9100 + i, "NODE01", "NODE02", "HIGH");
        TEST_ASSERT_TRUE(tx_scheduler_enqueue(&message, 0));
    }
    message = make_tx_message(9999, "NODE01", "NODE02", "HIGH");
    TEST_ASSERT_FALSE(tx_scheduler_enqueue(&message, 0));
}

static void test_dedup_tracks_same_source_and_id(void)
{
    remember_packet("NODE01", 7);
    TEST_ASSERT_TRUE(packet_seen("NODE01", 7));
}

static void test_dedup_distinguishes_different_sources(void)
{
    remember_packet("NODE01", 7);
    TEST_ASSERT_FALSE(packet_seen("NODE02", 7));
}

static void test_dedup_handles_full_cache_and_bursts(void)
{
    char source[FIELD_LEN];

    for (uint32_t i = 0; i < MAX_SEEN_PACKETS; i++) {
        snprintf(source, sizeof(source), "NODE%02lu", (unsigned long)i);
        remember_packet(source, i + 1);
    }

    TEST_ASSERT_TRUE(packet_seen("NODE00", 1));
    TEST_ASSERT_FALSE(packet_seen("NODE99", 9999));
}

static void test_dedup_evicts_old_entries_when_full(void)
{
    char source[FIELD_LEN];

    for (uint32_t i = 0; i < MAX_SEEN_PACKETS; i++) {
        snprintf(source, sizeof(source), "NODE%02lu", (unsigned long)i);
        remember_packet(source, i + 1);
    }
    remember_packet("NODE_NEW", 9999);
    TEST_ASSERT_FALSE(packet_seen("NODE00", 1));
    TEST_ASSERT_TRUE(packet_seen("NODE_NEW", 9999));
}

static void test_dedup_expiration_allows_reuse_after_ttl(void)
{
    remember_packet("NODE01", 100);
    deduplication_debug_set_seen_tick_for_test("NODE01", 100, 0);
    TEST_ASSERT_FALSE(packet_seen("NODE01", 100));
}

static void test_route_table_prefers_direct_route(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 1, -55);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_A", &route));
    TEST_ASSERT_EQUAL_STRING("NODE_A", route.destination);
    TEST_ASSERT_EQUAL_STRING("NODE_B", route.next_hop);
    TEST_ASSERT_EQUAL_INT(1, route.hop_count);
}

static void test_route_table_prefers_better_hop_count_over_rssi(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 3, -20);
    route_table_learn("NODE_A", "NODE_C", 1, -90);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_A", &route));
    TEST_ASSERT_EQUAL_STRING("NODE_C", route.next_hop);
    TEST_ASSERT_EQUAL_INT(1, route.hop_count);
}

static void test_route_table_prefers_better_rssi_when_hops_equal(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 2, -90);
    route_table_learn("NODE_A", "NODE_C", 2, -40);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_A", &route));
    TEST_ASSERT_EQUAL_STRING("NODE_C", route.next_hop);
    TEST_ASSERT_EQUAL_INT(-40, route.best_rssi);
}

static void test_route_table_rejects_self_route(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_A", 1, -50);
    TEST_ASSERT_FALSE(route_table_get_best("NODE_A", &route));
}

static void test_route_table_rejects_missing_route(void)
{
    route_entry_t route = {0};

    TEST_ASSERT_FALSE(route_table_get_best("NODE_A", &route));
}

static void test_route_table_rejects_stale_route(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 1, -55);
    route_table_debug_set_last_seen_for_test("NODE_A", "NODE_B", UINT32_MAX - 1000);
    TEST_ASSERT_FALSE(route_table_get_best("NODE_A", &route));
}

static void test_route_table_chooses_lowest_cost_among_candidates(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 2, -50);
    route_table_learn("NODE_A", "NODE_C", 2, -80);
    route_table_learn("NODE_A", "NODE_D", 1, -95);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_A", &route));
    TEST_ASSERT_EQUAL_STRING("NODE_D", route.next_hop);
    TEST_ASSERT_EQUAL_INT(1, route.hop_count);
}

static void test_route_table_excludes_previous_hop_self_candidate(void)
{
    route_entry_t route = {0};

    route_table_learn("NODE_A", "NODE_B", 1, -50);
    route_table_learn("NODE_A", "NODE_A", 1, -40);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_A", &route));
    TEST_ASSERT_EQUAL_STRING("NODE_B", route.next_hop);
}

static void test_route_table_keeps_fallback_when_no_route(void)
{
    route_entry_t route = {0};

    TEST_ASSERT_FALSE(route_table_get_best("NODE_Z", &route));
}

static void test_route_guided_forwarding_allows_preferred_previous_hop(void)
{
    mesh_packet_t packet = {0};
    route_entry_t route = {0};

    copy_field(packet.source, sizeof(packet.source), "NODE_A");
    copy_field(packet.destination, sizeof(packet.destination), "NODE_Z");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE_B");
    packet.hops = 3;

    route_table_learn("NODE_Z", "NODE_B", 2, -55);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_Z", &route));
    TEST_ASSERT_TRUE(app_runtime_should_forward_packet(&packet, &route, "NODE_C"));
}

static void test_route_guided_forwarding_suppresses_non_preferred_previous_hop(void)
{
    mesh_packet_t packet = {0};
    route_entry_t route = {0};

    copy_field(packet.source, sizeof(packet.source), "NODE_A");
    copy_field(packet.destination, sizeof(packet.destination), "NODE_Z");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE_X");
    packet.hops = 3;

    route_table_learn("NODE_Z", "NODE_B", 2, -55);
    TEST_ASSERT_TRUE(route_table_get_best("NODE_Z", &route));
    TEST_ASSERT_FALSE(app_runtime_should_forward_packet(&packet, &route, "NODE_C"));
}

static void test_route_guided_forwarding_falls_back_without_route(void)
{
    mesh_packet_t packet = {0};

    copy_field(packet.source, sizeof(packet.source), "NODE_A");
    copy_field(packet.destination, sizeof(packet.destination), "NODE_Z");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE_B");
    packet.hops = 3;

    TEST_ASSERT_TRUE(app_runtime_should_forward_packet(&packet, NULL, "NODE_C"));
}

static void test_forwarded_packet_preserves_source_and_id(void)
{
    mesh_packet_t packet = {0};
    uint8_t forwarded[PACKET_LEN] = {0};
    size_t forwarded_len = 0;
    mesh_packet_t parsed = {0};

    packet.id = 77;
    packet.hops = 3;
    copy_field(packet.source, sizeof(packet.source), "NODE_A");
    copy_field(packet.destination, sizeof(packet.destination), "NODE_Z");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE_B");
    copy_field(packet.payload, sizeof(packet.payload), "payload");
    packet.payload_len = strnlen(packet.payload, sizeof(packet.payload));
    location_decode("Purok 3~San Isidro~Cabuyao", &packet.location);

    TEST_ASSERT_TRUE(build_forward_packet_v2(&packet, forwarded, sizeof(forwarded), &forwarded_len));
    TEST_ASSERT_TRUE(parse_mesh_packet_v2(forwarded, forwarded_len, &parsed));
    TEST_ASSERT_EQUAL_UINT32(77, parsed.id);
    TEST_ASSERT_EQUAL_STRING("NODE_A", parsed.source);
}

static void test_parse_mesh_packet_with_next_hop(void)
{
    mesh_packet_t parsed = {0};

    TEST_ASSERT_TRUE(parse_mesh_packet("BEMS|44|NODE01|NODE99|FLOOD|HIGH|HOPS=4|RELAY=NODE02|NODE03|LOC=Purok 3~San Isidro~Cabuyao|Water rising", &parsed));
    TEST_ASSERT_EQUAL_STRING("NODE03", parsed.next_hop);
    TEST_ASSERT_EQUAL_STRING("NODE02", parsed.relay);
}

static void test_parse_mesh_packet_without_next_hop_remains_legacy_compatible(void)
{
    mesh_packet_t parsed = {0};

    TEST_ASSERT_TRUE(parse_mesh_packet("BEMS|45|NODE01|NODE99|FLOOD|HIGH|HOPS=4|RELAY=NODE02|LOC=Purok 3~San Isidro~Cabuyao|Water rising", &parsed));
    TEST_ASSERT_EQUAL_STRING("", parsed.next_hop);
}

static void test_parse_mesh_packet_v2_rejects_truncated_minimum_frame(void)
{
    const uint8_t packet[] = {0xB2, BEMS_PACKET_FORMAT_V2, 0x00, BEMS_PACKET_TYPE_FLOOD, BEMS_PRIORITY_NORMAL, 0x00};
    mesh_packet_t parsed = {0};

    TEST_ASSERT_FALSE(parse_mesh_packet_v2(packet, sizeof(packet), &parsed));
}

static void test_parse_mesh_packet_v2_maps_reserved_broadcast_destination(void)
{
    uint8_t packet[] = {
        0xB2, BEMS_PACKET_FORMAT_V2, 0x00, BEMS_PACKET_TYPE_FLOOD, BEMS_PRIORITY_NORMAL,
        0x05, 'S','R','C','1','2',
        0xFF,
        0x01, 0x00, 0x00, 0x00,
        0x05, 'R','E','L','A','Y',
        0x01,
        0x00
    };
    mesh_packet_t parsed = {0};

    TEST_ASSERT_TRUE(parse_mesh_packet_v2(packet, sizeof(packet), &parsed));
    TEST_ASSERT_EQUAL_STRING("ALL", parsed.destination);
}

static void test_build_forward_packet_v2_roundtrips_minimal_broadcast_packet(void)
{
    mesh_packet_t packet = {0};
    uint8_t encoded[PACKET_LEN] = {0};
    size_t encoded_len = 0;
    mesh_packet_t parsed = {0};

    packet.valid = true;
    packet.id = 1234;
    packet.hops = 1;
    copy_field(packet.source, sizeof(packet.source), "NODE01");
    copy_field(packet.destination, sizeof(packet.destination), "ALL");
    copy_field(packet.type, sizeof(packet.type), "ACK");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE01");
    copy_field(packet.payload, sizeof(packet.payload), "ACK for 7");
    packet.broadcast_destination = true;

    TEST_ASSERT_TRUE(build_forward_packet_v2(&packet, encoded, sizeof(encoded), &encoded_len));
    TEST_ASSERT_TRUE(encoded_len <= BEMS_MAX_PLAINTEXT);
    TEST_ASSERT_TRUE(parse_mesh_packet_v2(encoded, encoded_len, &parsed));
    TEST_ASSERT_EQUAL_UINT32(1234, parsed.id);
    TEST_ASSERT_EQUAL_STRING("NODE01", parsed.source);
    TEST_ASSERT_EQUAL_STRING("ALL", parsed.destination);
    TEST_ASSERT_EQUAL_STRING("ACK", parsed.type);
    TEST_ASSERT_EQUAL_STRING("ACK for 7", parsed.payload);
}

static void test_build_forward_packet_v2_accepts_zero_length_payload(void)
{
    mesh_packet_t packet = {0};
    uint8_t encoded[PACKET_LEN] = {0};
    size_t encoded_len = 0;
    mesh_packet_t parsed = {0};

    packet.valid = true;
    packet.id = 2001;
    packet.hops = 1;
    copy_field(packet.source, sizeof(packet.source), "NODE01");
    copy_field(packet.destination, sizeof(packet.destination), "NODE02");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE01");
    packet.payload[0] = '\0';
    packet.payload_len = 0;

    TEST_ASSERT_TRUE(build_forward_packet_v2(&packet, encoded, sizeof(encoded), &encoded_len));
    TEST_ASSERT_TRUE(parse_mesh_packet_v2(encoded, encoded_len, &parsed));
    TEST_ASSERT_EQUAL_UINT32(2001, parsed.id);
    TEST_ASSERT_EQUAL_STRING("", parsed.payload);
}

static void test_build_forward_packet_v2_rejects_insufficient_output_buffer_with_guards(void)
{
    mesh_packet_t packet = {0};
    uint8_t storage[PACKET_LEN + 2];
    uint8_t *encoded = &storage[1];
    size_t encoded_len = 0;

    memset(storage, 0xA5, sizeof(storage));
    packet.valid = true;
    packet.id = 2002;
    packet.hops = 1;
    copy_field(packet.source, sizeof(packet.source), "NODE01");
    copy_field(packet.destination, sizeof(packet.destination), "NODE02");
    copy_field(packet.type, sizeof(packet.type), "FLOOD");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), "NODE01");
    copy_field(packet.payload, sizeof(packet.payload), "payload");

    TEST_ASSERT_FALSE(build_forward_packet_v2(&packet, encoded, 8, &encoded_len));
    TEST_ASSERT_EQUAL_HEX8(0xA5, storage[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, storage[sizeof(storage) - 1]);
}

static void test_sync_response_batch_encoder_handles_zero_one_and_many_records(void)
{
    emergency_message_t records[3];
    uint8_t payload[PAYLOAD_LEN] = {0};
    size_t payload_len = 0;

    TEST_ASSERT_FALSE(mesh_control_encode_sync_response_batch(NULL, 0, payload, sizeof(payload), &payload_len));

    records[0] = make_sync_message(100, "NODE01", "ALL", "FLOOD", "NORMAL", 1, "hello");
    TEST_ASSERT_TRUE(mesh_control_encode_sync_response_batch(records, 1, payload, sizeof(payload), &payload_len));
    TEST_ASSERT_EQUAL_UINT8(1, payload[0]);
    TEST_ASSERT_TRUE(payload_len > 1);

    records[1] = make_sync_message(101, "NODE02", "ALL", "ACK", "NORMAL", 0, "ACK for 7");
    records[2] = make_sync_message(102, "NODE03", "ALL", "FLOOD", "NORMAL", 2, "payload");
    TEST_ASSERT_TRUE(mesh_control_encode_sync_response_batch(records, 3, payload, sizeof(payload), &payload_len));
    TEST_ASSERT_EQUAL_UINT8(3, payload[0]);
    TEST_ASSERT_TRUE(payload_len > 1);
}

static void test_sync_response_batch_encoder_handles_exact_batch_and_one_byte_over_boundary(void)
{
    emergency_message_t records[2] = {
        make_sync_message(300, "NODE01", "ALL", "FLOOD", "NORMAL", 1, "hello"),
        make_sync_message(301, "NODE02", "ALL", "ACK", "NORMAL", 0, "ACK for 7"),
    };
    uint8_t payload[PAYLOAD_LEN] = {0};
    size_t payload_len = 0;

    TEST_ASSERT_TRUE(mesh_control_encode_sync_response_batch(records, 2, payload, sizeof(payload), &payload_len));
    TEST_ASSERT_TRUE(payload_len <= BEMS_MAX_PLAINTEXT);
    TEST_ASSERT_EQUAL_UINT8(2, payload[0]);

    records[1].payload[sizeof(records[1].payload) - 2] = 'X';
    records[1].payload[sizeof(records[1].payload) - 1] = '\0';
    TEST_ASSERT_FALSE(mesh_control_encode_sync_response_batch(records, 2, payload, 12, &payload_len));
}

static void test_sync_response_batch_encoder_rejects_oversized_record(void)
{
    emergency_message_t record = make_sync_message(200, "NODE01", "ALL", "FLOOD", "NORMAL", 3, "payload");
    uint8_t payload[16] = {0};
    size_t payload_len = 0;

    memset(record.source, 'A', sizeof(record.source) - 1);
    record.source[sizeof(record.source) - 1] = '\0';
    memset(record.destination, 'B', sizeof(record.destination) - 1);
    record.destination[sizeof(record.destination) - 1] = '\0';
    memset(record.type, 'C', sizeof(record.type) - 1);
    record.type[sizeof(record.type) - 1] = '\0';
    memset(record.priority, 'D', sizeof(record.priority) - 1);
    record.priority[sizeof(record.priority) - 1] = '\0';
    memset(record.payload, 'E', sizeof(record.payload) - 1);
    record.payload[sizeof(record.payload) - 1] = '\0';

    TEST_ASSERT_FALSE(mesh_control_encode_sync_response_batch(&record, 1, payload, sizeof(payload), &payload_len));
}

static void test_sync_response_decoder_rejects_too_many_or_truncated_fields(void)
{
    const uint8_t truncated_length[] = {0x01, 0x64, 0x00, 0x00, 0x00, 0x06, 'N', 'O', 'D', 'E', '0', '1', 0x03, 'A', 'L', 'L', 0x05, 'F', 'L', 'O', 'O', 'D', 0x06, 'N', 'O', 'R', 'M', 'A', 'L', 0x01, 0x05, 'h', 'e', 'l', 'l'};
    const uint8_t trailing_bytes[] = {0x01, 0x64, 0x00, 0x00, 0x00, 0x06, 'N', 'O', 'D', 'E', '0', '1', 0x03, 'A', 'L', 'L', 0x05, 'F', 'L', 'O', 'O', 'D', 0x06, 'N', 'O', 'R', 'M', 'A', 'L', 0x01, 0x05, 'h', 'e', 'l', 'l', 'o', 0xFF};
    mesh_packet_t records[2] = {0};

    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(truncated_length, sizeof(truncated_length), records, 2));
    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(trailing_bytes, sizeof(trailing_bytes), records, 2));
}

static void test_sync_response_batch_round_trip_multiple_records_preserves_order_and_fields(void)
{
    emergency_message_t records[3] = {
        make_sync_message(400, "NODE01", "ALL", "FLOOD", "NORMAL", 1, "hello"),
        make_sync_message(401, "NODE02", "NODE03", "ACK", "HIGH", 0, "ACK for 12"),
        make_sync_message(402, "NODE04", "ALL", "TIME_SYNC", "NORMAL", 2, "epoch=123~dist=2"),
    };
    uint8_t payload[PAYLOAD_LEN] = {0};
    mesh_packet_t decoded[4] = {0};
    size_t payload_len = 0;
    size_t count;

    TEST_ASSERT_TRUE(mesh_control_encode_sync_response_batch(records, 3, payload, sizeof(payload), &payload_len));
    count = mesh_control_decode_sync_response_records(payload, payload_len, decoded, 4);
    TEST_ASSERT_EQUAL_UINT(3, count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(records[i].id, decoded[i].id);
        TEST_ASSERT_EQUAL_STRING(records[i].source, decoded[i].source);
        TEST_ASSERT_EQUAL_STRING(records[i].destination, decoded[i].destination);
        TEST_ASSERT_EQUAL_STRING(records[i].type, decoded[i].type);
        TEST_ASSERT_EQUAL_STRING(records[i].priority, decoded[i].priority);
        TEST_ASSERT_EQUAL_INT(records[i].hops, decoded[i].hops);
        TEST_ASSERT_EQUAL_STRING(records[i].payload, decoded[i].payload);
    }
}

static void test_sync_response_record_decoder_handles_binary_records(void)
{
    const uint8_t payload[] = {
        0x02,
        0x64,0x00,0x00,0x00, 0x06, 'N','O','D','E','0','1', 0x03, 'A','L','L', 0x05, 'F','L','O','O','D', 0x06, 'N','O','R','M','A','L', 0x01, 0x05, 'h','e','l','l','o',
        0x65,0x00,0x00,0x00, 0x06, 'N','O','D','E','0','2', 0x03, 'A','L','L', 0x03, 'A','C','K', 0x06, 'N','O','R','M','A','L', 0x00, 0x09, 'A','C','K',' ','f','o','r',' ','7'
    };
    mesh_packet_t records[4] = {0};
    size_t count;

    count = mesh_control_decode_sync_response_records(payload, sizeof(payload), records, 4);
    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_STRING("NODE01", records[0].source);
    TEST_ASSERT_EQUAL_STRING("hello", records[0].payload);
    TEST_ASSERT_EQUAL_STRING("NODE02", records[1].source);
    TEST_ASSERT_EQUAL_STRING("ACK for 7", records[1].payload);
    TEST_ASSERT_EQUAL_UINT32(100, records[0].id);
    TEST_ASSERT_EQUAL_UINT32(101, records[1].id);
}

static void test_sync_response_record_decoder_rejects_truncated_and_invalid_binary_records(void)
{
    const uint8_t truncated_header[] = {0x01, 0x64, 0x00, 0x00};
    const uint8_t truncated_record[] = {0x01, 0x64, 0x00, 0x00, 0x00, 0x06, 'N', 'O'};
    const uint8_t invalid_count_extra[] = {
        0x01,
        0x64,0x00,0x00,0x00, 0x06, 'N','O','D','E','0','1', 0x03, 'A','L','L', 0x05, 'F','L','O','O','D', 0x06, 'N','O','R','M','A','L', 0x01, 0x05, 'h','e','l','l','o',
        0xFF
    };
    mesh_packet_t records[2] = {0};

    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(truncated_header, sizeof(truncated_header), records, 2));
    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(truncated_record, sizeof(truncated_record), records, 2));
    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(invalid_count_extra, sizeof(invalid_count_extra), records, 2));
}

static void test_sync_response_record_decoder_rejects_overlong_length(void)
{
    uint8_t payload[] = {
        0x01,
        0x64,0x00,0x00,0x00,
        0x80
    };
    mesh_packet_t records[1] = {0};

    TEST_ASSERT_EQUAL_UINT(0, mesh_control_decode_sync_response_records(payload, sizeof(payload), records, 1));
}

static void test_dns_parse_accepts_a_query(void)
{
    const uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0x03,'w','w','w',0x06,'g','o','o','g','l','e',0x03,'c','o','m',0x00,
        0x00,0x01,0x00,0x01
    };
    dns_request_info_t info;

    TEST_ASSERT_TRUE(dns_server_parse_request(packet, sizeof(packet), &info));
    TEST_ASSERT_EQUAL_UINT16(1, info.qtype);
    TEST_ASSERT_EQUAL_UINT16(1, info.qclass);
}

static void test_dns_parse_accepts_aaaa_query(void)
{
    const uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0x03,'w','w','w',0x06,'a','p','p','l','e',0x03,'c','o','m',0x00,
        0x00,0x1c,0x00,0x01
    };
    dns_request_info_t info;

    TEST_ASSERT_TRUE(dns_server_parse_request(packet, sizeof(packet), &info));
    TEST_ASSERT_EQUAL_UINT16(28, info.qtype);
}

static void test_dns_builds_a_response_for_a_query(void)
{
    uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0x03,'w','w','w',0x06,'g','o','o','g','l','e',0x03,'c','o','m',0x00,
        0x00,0x01,0x00,0x01
    };
    dns_request_info_t info;
    size_t response_length = 0;

    TEST_ASSERT_TRUE(dns_server_parse_request(packet, sizeof(packet), &info));
    TEST_ASSERT_TRUE(dns_server_build_response(packet, sizeof(packet), &info, inet_addr("192.168.4.1"), &response_length));
    TEST_ASSERT_TRUE(response_length > info.question_end);
    TEST_ASSERT_EQUAL_UINT16(1, (uint16_t)((packet[6] << 8) | packet[7]));
}

static void test_dns_builds_no_answer_for_aaaa_query(void)
{
    uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0x03,'w','w','w',0x06,'g','o','o','g','l','e',0x03,'c','o','m',0x00,
        0x00,0x1c,0x00,0x01
    };
    dns_request_info_t info;
    size_t response_length = 0;

    TEST_ASSERT_TRUE(dns_server_parse_request(packet, sizeof(packet), &info));
    TEST_ASSERT_TRUE(dns_server_build_response(packet, sizeof(packet), &info, inet_addr("192.168.4.1"), &response_length));
    TEST_ASSERT_EQUAL_UINT16(0, (uint16_t)((packet[6] << 8) | packet[7]));
    TEST_ASSERT_EQUAL_UINT16(info.question_end, response_length);
}

static void test_dns_parse_rejects_multiple_questions(void)
{
    const uint8_t packet[] = {0x12,0x34,0x01,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00};
    dns_request_info_t info;

    TEST_ASSERT_FALSE(dns_server_parse_request(packet, sizeof(packet), &info));
}

static void test_dns_parse_rejects_truncated_query(void)
{
    const uint8_t packet[] = {0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x03,'w','w'};
    dns_request_info_t info;

    TEST_ASSERT_FALSE(dns_server_parse_request(packet, sizeof(packet), &info));
}

static void test_dns_parse_rejects_empty_packet(void)
{
    dns_request_info_t info;

    TEST_ASSERT_FALSE(dns_server_parse_request(NULL, 0, &info));
}

static void test_dns_parse_rejects_invalid_compression_pointer(void)
{
    const uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0xC0,0xFF,0x00,0x01,0x00,0x01
    };
    dns_request_info_t info;

    TEST_ASSERT_FALSE(dns_server_parse_request(packet, sizeof(packet), &info));
}

static void test_dns_response_rejects_multiple_questions(void)
{
    uint8_t packet[] = {
        0x12,0x34,0x01,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00
    };
    dns_request_info_t info;
    size_t response_length = 0;

    TEST_ASSERT_FALSE(dns_server_parse_request(packet, sizeof(packet), &info));
    TEST_ASSERT_FALSE(dns_server_build_response(packet, sizeof(packet), &info, inet_addr("192.168.4.1"), &response_length));
}

static void test_string_utils_form_value_uses_production_parser(void)
{
    char output[FIELD_LEN] = {0};

    TEST_ASSERT_TRUE(form_value("node_id=BRGY01&location=Purok+3%2C+Hall", "location", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("Purok 3, Hall", output);
}

static void test_message_store_add_find_remove(void)
{
    emergency_message_t message = {0};
    emergency_message_t found = {0};
    int slot = -1;

    copy_field(message.direction, sizeof(message.direction), "TX");
    copy_field(message.source, sizeof(message.source), "NODE01");
    copy_field(message.destination, sizeof(message.destination), "ALL");
    copy_field(message.type, sizeof(message.type), "FLOOD");
    copy_field(message.priority, sizeof(message.priority), "NORMAL");
    copy_field(message.status, sizeof(message.status), "PENDING");
    message.id = 5001;

    TEST_ASSERT_TRUE(message_store_add(&message, &slot));
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_TRUE(message_store_find(5001, "NODE01", &found));
    TEST_ASSERT_EQUAL_UINT32(5001, found.id);
    TEST_ASSERT_TRUE(message_store_remove(5001, "NODE01"));
    TEST_ASSERT_FALSE(message_store_find(5001, "NODE01", &found));
}

static void test_roster_touch_inserts_and_updates(void)
{
    location_info_t loc = {0};
    roster_entry_t snapshot[MAX_ROSTER_ENTRIES];
    size_t count;

    copy_field(loc.barangay, sizeof(loc.barangay), "San Isidro");
    roster_touch("NODE01", &loc, 100, -60, 8);
    TEST_ASSERT_FALSE(roster_is_stale("NODE01"));

    fake_roster_now_ms += 1000;
    roster_touch("NODE01", &loc, 200, -55, 9);

    count = roster_get_snapshot(snapshot, MAX_ROSTER_ENTRIES);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_STRING("NODE01", snapshot[0].node_id);
    TEST_ASSERT_EQUAL_UINT32(200, snapshot[0].last_seen_epoch);
    TEST_ASSERT_TRUE(snapshot[0].online);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_mesh_packet_valid_packet);
    RUN_TEST(test_parse_mesh_packet_sets_thread_key_for_direct_messages);
    RUN_TEST(test_packet_seen_uses_source_and_id);
    RUN_TEST(test_packet_seen_expires_after_ttl);
    RUN_TEST(test_dedup_tracks_same_source_and_id);
    RUN_TEST(test_dedup_distinguishes_different_sources);
    RUN_TEST(test_dedup_handles_full_cache_and_bursts);
    RUN_TEST(test_dedup_evicts_old_entries_when_full);
    RUN_TEST(test_dedup_expiration_allows_reuse_after_ttl);
    RUN_TEST(test_replay_accepts_first_and_sequential_packets);
    RUN_TEST(test_replay_rejects_duplicate);
    RUN_TEST(test_replay_accepts_out_of_order_inside_window);
    RUN_TEST(test_replay_wraparound);
    RUN_TEST(test_mesh_sequence_advances_monotonically);
    RUN_TEST(test_tx_scheduler_queue_and_ack);
    RUN_TEST(test_tx_scheduler_rejects_wrong_and_duplicate_ack);
    RUN_TEST(test_tx_scheduler_queue_full);
    RUN_TEST(test_route_table_prefers_direct_route);
    RUN_TEST(test_route_table_prefers_better_hop_count_over_rssi);
    RUN_TEST(test_route_table_prefers_better_rssi_when_hops_equal);
    RUN_TEST(test_route_table_rejects_self_route);
    RUN_TEST(test_route_table_rejects_missing_route);
    RUN_TEST(test_route_table_rejects_stale_route);
    RUN_TEST(test_route_table_chooses_lowest_cost_among_candidates);
    RUN_TEST(test_route_table_excludes_previous_hop_self_candidate);
    RUN_TEST(test_route_table_keeps_fallback_when_no_route);
    RUN_TEST(test_route_guided_forwarding_allows_preferred_previous_hop);
    RUN_TEST(test_route_guided_forwarding_suppresses_non_preferred_previous_hop);
    RUN_TEST(test_route_guided_forwarding_falls_back_without_route);
    RUN_TEST(test_forwarded_packet_preserves_source_and_id);
    RUN_TEST(test_parse_mesh_packet_with_next_hop);
    RUN_TEST(test_parse_mesh_packet_without_next_hop_remains_legacy_compatible);
    RUN_TEST(test_parse_mesh_packet_v2_rejects_truncated_minimum_frame);
    RUN_TEST(test_parse_mesh_packet_v2_maps_reserved_broadcast_destination);
    RUN_TEST(test_build_forward_packet_v2_roundtrips_minimal_broadcast_packet);
    RUN_TEST(test_build_forward_packet_v2_accepts_zero_length_payload);
    RUN_TEST(test_build_forward_packet_v2_rejects_insufficient_output_buffer_with_guards);
    RUN_TEST(test_sync_response_batch_encoder_handles_zero_one_and_many_records);
    RUN_TEST(test_sync_response_batch_encoder_handles_exact_batch_and_one_byte_over_boundary);
    RUN_TEST(test_sync_response_batch_encoder_rejects_oversized_record);
    RUN_TEST(test_sync_response_decoder_rejects_too_many_or_truncated_fields);
    RUN_TEST(test_sync_response_batch_round_trip_multiple_records_preserves_order_and_fields);
    RUN_TEST(test_sync_response_record_decoder_handles_binary_records);
    RUN_TEST(test_sync_response_record_decoder_rejects_truncated_and_invalid_binary_records);
    RUN_TEST(test_sync_response_record_decoder_rejects_overlong_length);
    RUN_TEST(test_dns_parse_accepts_a_query);
    RUN_TEST(test_dns_parse_accepts_aaaa_query);
    RUN_TEST(test_dns_builds_a_response_for_a_query);
    RUN_TEST(test_dns_builds_no_answer_for_aaaa_query);
    RUN_TEST(test_dns_parse_rejects_multiple_questions);
    RUN_TEST(test_dns_parse_rejects_truncated_query);
    RUN_TEST(test_dns_parse_rejects_empty_packet);
    RUN_TEST(test_dns_parse_rejects_invalid_compression_pointer);
    RUN_TEST(test_dns_response_rejects_multiple_questions);
    RUN_TEST(test_string_utils_form_value_uses_production_parser);
    RUN_TEST(test_message_store_add_find_remove);
    RUN_TEST(test_roster_touch_inserts_and_updates);
    return UNITY_END();
}
