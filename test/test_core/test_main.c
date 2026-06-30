#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#define FIELD_LEN 32
#define PAYLOAD_LEN 160
#define PACKET_LEN 320
#define MAX_SEEN_PACKETS 4
#define SEEN_PACKET_TTL_MS 60000

typedef struct {
    bool valid;
    uint32_t id;
    int hops;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char relay[FIELD_LEN];
    char location[FIELD_LEN];
    char payload[PAYLOAD_LEN];
} mesh_packet_t;

typedef struct {
    uint32_t id;
    uint32_t seen_tick;
    char source[FIELD_LEN];
} seen_packet_t;

static seen_packet_t seen_packets[MAX_SEEN_PACKETS];
static size_t seen_packet_count;
static uint32_t fake_tick;

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

static bool parse_mesh_packet(const char *packet, mesh_packet_t *parsed)
{
    char packet_copy[PACKET_LEN];
    char *fields[10] = {0};
    char *cursor = packet_copy;
    size_t field_count = 0;

    memset(parsed, 0, sizeof(*parsed));
    copy_field(packet_copy, sizeof(packet_copy), packet);

    while (field_count < sizeof(fields) / sizeof(fields[0]) && cursor != NULL) {
        fields[field_count++] = cursor;
        if (field_count == sizeof(fields) / sizeof(fields[0])) {
            break;
        }

        cursor = strchr(cursor, '|');
        if (cursor != NULL) {
            *cursor = '\0';
            cursor++;
        }
    }

    if (field_count < 10 || strcmp(fields[0], "BEMS") != 0) {
        return false;
    }

    parsed->valid = true;
    parsed->id = (uint32_t)strtoul(fields[1], NULL, 10);
    parsed->hops = strncmp(fields[6], "HOPS=", 5) == 0 ? atoi(fields[6] + 5) : 0;
    copy_field(parsed->source, sizeof(parsed->source), fields[2]);
    copy_field(parsed->destination, sizeof(parsed->destination), fields[3]);
    copy_field(parsed->type, sizeof(parsed->type), fields[4]);
    copy_field(parsed->priority, sizeof(parsed->priority), fields[5]);
    copy_field(parsed->relay, sizeof(parsed->relay), fields[7]);
    copy_field(parsed->location, sizeof(parsed->location), fields[8]);
    copy_field(parsed->payload, sizeof(parsed->payload), fields[9]);
    return true;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else if (*read_ptr == '%' && hex_value(read_ptr[1]) >= 0 && hex_value(read_ptr[2]) >= 0) {
            *write_ptr++ = (char)((hex_value(read_ptr[1]) << 4) | hex_value(read_ptr[2]));
            read_ptr += 3;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

static bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end == NULL ? strlen(value_start) : (size_t)(value_end - value_start);
            size_t copy_len = value_len < output_size - 1 ? value_len : output_size - 1;

            memcpy(output, value_start, copy_len);
            output[copy_len] = '\0';
            url_decode(output);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}

static bool packet_seen(const char *source, uint32_t id)
{
    for (size_t i = 0; i < seen_packet_count;) {
        if ((fake_tick - seen_packets[i].seen_tick) > SEEN_PACKET_TTL_MS) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        if (seen_packets[i].id == id && strcmp(seen_packets[i].source, source) == 0) {
            return true;
        }

        i++;
    }

    return false;
}

static void remember_packet(const char *source, uint32_t id)
{
    seen_packet_t *seen_packet;
    size_t slot_index = 0;

    for (size_t i = 0; i < seen_packet_count;) {
        if ((fake_tick - seen_packets[i].seen_tick) > SEEN_PACKET_TTL_MS) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        i++;
    }

    if (seen_packet_count < MAX_SEEN_PACKETS) {
        seen_packet = &seen_packets[seen_packet_count++];
    } else {
        for (size_t i = 1; i < seen_packet_count; i++) {
            if ((fake_tick - seen_packets[i].seen_tick) > (fake_tick - seen_packets[slot_index].seen_tick)) {
                slot_index = i;
            }
        }
        seen_packet = &seen_packets[slot_index];
    }

    seen_packet->id = id;
    seen_packet->seen_tick = fake_tick;
    copy_field(seen_packet->source, sizeof(seen_packet->source), source);
}

void setUp(void)
{
    memset(seen_packets, 0, sizeof(seen_packets));
    seen_packet_count = 0;
    fake_tick = 1000;
}

void tearDown(void)
{
}

static void test_parse_mesh_packet_valid_packet(void)
{
    mesh_packet_t parsed;

    TEST_ASSERT_TRUE(parse_mesh_packet("BEMS|42|NODE01|ALL|FLOOD|HIGH|HOPS=5|RELAY=1|LOC=HALL|Water rising", &parsed));
    TEST_ASSERT_TRUE(parsed.valid);
    TEST_ASSERT_EQUAL_UINT32(42, parsed.id);
    TEST_ASSERT_EQUAL_INT(5, parsed.hops);
    TEST_ASSERT_EQUAL_STRING("NODE01", parsed.source);
    TEST_ASSERT_EQUAL_STRING("ALL", parsed.destination);
    TEST_ASSERT_EQUAL_STRING("FLOOD", parsed.type);
    TEST_ASSERT_EQUAL_STRING("HIGH", parsed.priority);
    TEST_ASSERT_EQUAL_STRING("RELAY=1", parsed.relay);
    TEST_ASSERT_EQUAL_STRING("LOC=HALL", parsed.location);
    TEST_ASSERT_EQUAL_STRING("Water rising", parsed.payload);
}

static void test_parse_mesh_packet_rejects_bad_prefix(void)
{
    mesh_packet_t parsed;

    TEST_ASSERT_FALSE(parse_mesh_packet("NOPE|42|NODE01|ALL|FLOOD|HIGH|HOPS=5|RELAY=1|LOC=HALL|Water rising", &parsed));
}

static void test_url_decode_decodes_spaces_and_hex(void)
{
    char value[] = "Barangay+Hall%2FClinic";

    url_decode(value);
    TEST_ASSERT_EQUAL_STRING("Barangay Hall/Clinic", value);
}

static void test_form_value_extracts_and_decodes_field(void)
{
    char output[FIELD_LEN] = {0};

    TEST_ASSERT_TRUE(form_value("node_id=BRGY01&location=Purok+3%2C+Hall", "location", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("Purok 3, Hall", output);
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

    fake_tick += SEEN_PACKET_TTL_MS + 1;
    TEST_ASSERT_FALSE(packet_seen("NODE01", 10));
    TEST_ASSERT_EQUAL_UINT(0, seen_packet_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_mesh_packet_valid_packet);
    RUN_TEST(test_parse_mesh_packet_rejects_bad_prefix);
    RUN_TEST(test_url_decode_decodes_spaces_and_hex);
    RUN_TEST(test_form_value_extracts_and_decodes_field);
    RUN_TEST(test_packet_seen_uses_source_and_id);
    RUN_TEST(test_packet_seen_expires_after_ttl);
    return UNITY_END();
}
