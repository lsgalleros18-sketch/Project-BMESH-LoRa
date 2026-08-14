#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <unity.h>

#include "mesh_protocol.h"
#include "mesh/mesh_sequence.h"
#include "mesh/replay_protection.h"
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

static void test_mesh_sequence_advances_monotonically(void)
{
    mesh_sequence_init();
    mesh_sequence_update(41);
    TEST_ASSERT_EQUAL_UINT32(41, mesh_sequence_peek());
    TEST_ASSERT_EQUAL_UINT32(42, mesh_sequence_next());
    TEST_ASSERT_EQUAL_UINT32(42, mesh_sequence_peek());
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
    RUN_TEST(test_replay_accepts_first_and_sequential_packets);
    RUN_TEST(test_replay_rejects_duplicate);
    RUN_TEST(test_replay_accepts_out_of_order_inside_window);
    RUN_TEST(test_mesh_sequence_advances_monotonically);
    RUN_TEST(test_route_table_prefers_direct_route);
    RUN_TEST(test_route_table_prefers_better_hop_count_over_rssi);
    RUN_TEST(test_route_table_prefers_better_rssi_when_hops_equal);
    RUN_TEST(test_route_table_rejects_self_route);
    RUN_TEST(test_route_table_rejects_missing_route);
    RUN_TEST(test_route_table_rejects_stale_route);
    RUN_TEST(test_route_table_chooses_lowest_cost_among_candidates);
    RUN_TEST(test_route_table_excludes_previous_hop_self_candidate);
    RUN_TEST(test_route_table_keeps_fallback_when_no_route);
    RUN_TEST(test_dns_parse_accepts_a_query);
    RUN_TEST(test_dns_parse_accepts_aaaa_query);
    RUN_TEST(test_dns_parse_rejects_multiple_questions);
    RUN_TEST(test_dns_parse_rejects_truncated_query);
    RUN_TEST(test_dns_parse_rejects_empty_packet);
    RUN_TEST(test_dns_parse_rejects_invalid_compression_pointer);
    RUN_TEST(test_string_utils_form_value_uses_production_parser);
    RUN_TEST(test_message_store_add_find_remove);
    RUN_TEST(test_roster_touch_inserts_and_updates);
    return UNITY_END();
}
