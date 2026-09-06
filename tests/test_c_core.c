#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "c_vector.h"
#include "dns_packet.h"
#include "firmware_image.h"
#include "text_format.h"

static void make_query(uint8_t* packet) {
  memset(packet, 0, 64);
  packet[0] = 0x12;
  packet[1] = 0x34;
  packet[2] = 1;
  packet[5] = 1;
  packet[12] = 3;
  memcpy(packet + 13, "abc", 3);
  packet[18] = 1;
  packet[20] = 1;
}
static void test_dns(void) {
  uint8_t packet[64];
  make_query(packet);
  assert(dns_reply(packet, 21, sizeof(packet), 0x0104a8c0) == 37);
  assert(packet[0] == 0x12 && packet[1] == 0x34 && packet[7] == 1);
  assert(!memcmp(packet + 33, (uint8_t[]){192, 168, 4, 1}, 4));
  make_query(packet);
  packet[18] = 28;
  assert(dns_reply(packet, 21, sizeof(packet), 0) == 21 && !packet[7]);
  make_query(packet);
  packet[12] = 0xc0;
  assert(!dns_reply(packet, 21, sizeof(packet), 0));
  make_query(packet);
  for (size_t size = 0; size < 21; ++size)
    assert(!dns_reply(packet, size, sizeof(packet), 0));
  assert(!dns_reply(packet, 21, 36, 0));
  packet[5] = 2;
  assert(!dns_reply(packet, 21, sizeof(packet), 0));
  assert(!dns_reply(NULL, 21, 64, 0));
  /* Arbitrary malformed input must never modify bytes beyond capacity. */
  uint32_t random = 1;
  for (size_t trial = 0; trial < 10000; ++trial) {
    for (size_t i = 0; i < sizeof(packet); ++i) {
      random = random * 1664525u + 1013904223u;
      packet[i] = (uint8_t)(random >> 24);
    }
    size_t cap = 12 + trial % 40, size = trial % (cap + 1);
    uint8_t guard[64];
    memcpy(guard, packet, sizeof(packet));
    assert(dns_reply(packet, size, cap, 0) <= cap);
    assert(!memcmp(packet + cap, guard + cap, sizeof(packet) - cap));
  }
}
static void test_image(void) {
  uint8_t image[288] = {0};
  image[0] = 0xe9;
  image[12] = 9;
  memcpy(image + 32, (uint8_t[]){0x32, 0x54, 0xcd, 0xab}, 4);
  memcpy(image + 80, "esp32_div_v2", 13);
  assert(firmware_matches_image(image, sizeof(image), 9, "esp32_div_v2"));
  assert(!firmware_matches_image(image, 287, 9, "esp32_div_v2"));
  assert(!firmware_matches_image(image, sizeof(image), 0, "esp32_div_v2"));
  assert(!firmware_matches_image(image, sizeof(image), 9, "esp32_div_v1"));
  assert(!firmware_matches_image(image, sizeof(image), 9, "esp32_div"));
  assert(!firmware_matches_image(NULL, sizeof(image), 9, "esp32_div_v2"));
  image[92] = 'x';
  assert(!firmware_matches_image(image, sizeof(image), 9, "esp32_div_v2"));
}
static void test_text_and_buffers(void) {
  text_t value = TEXT("  aBc  ");
  text_trim(&value);
  text_upper(&value);
  assert(text_equal(value, TEXT("ABC")));
  text_append(&value, text_character(','));
  text_append(&value, TEXT(42));
  assert(!strcmp(value.data, "ABC,42"));
  assert(text_equal(text_slice(value, 4, SIZE_MAX), TEXT("42")));
  assert(text_equal(TEXT_FORMAT(255, 16), TEXT("FF")));
  assert(text_equal(TEXT_FORMAT(1.25, 2), TEXT("1.25")));
  char long_text[512];
  memset(long_text, 'x', sizeof(long_text));
  long_text[511] = 0;
  value = TEXT(long_text);
  assert(text_length(value) == TEXT_CAPACITY - 1);
  text_append(&value, TEXT("tail"));
  assert(value.data[TEXT_CAPACITY - 1] == 0);
  c_vector_t items = {0};
  for (uint32_t i = 0; i < 1000; ++i) C_VECTOR_PUSH(&items, i);
  assert(items.size == 1000);
  for (size_t i = 0; i < items.size; ++i)
    assert(((uint32_t*)items.data)[i] == i);
  c_vector_t moved = c_vector_move(&items);
  assert(!items.data && !items.size);
  assert(moved.size == 1000);
  c_vector_free(&moved);
  assert(!moved.data);
  c_vector_t narrow = {0};
  assert(C_VECTOR_PUSH_AS(&narrow, uint16_t, 700));
  assert(narrow.element_size == sizeof(uint16_t) &&
         ((uint16_t*)narrow.data)[0] == 700);
  assert(!C_VECTOR_PUSH_AS(&narrow, uint32_t, 900));
  assert(narrow.size == 1 && ((uint16_t*)narrow.data)[0] == 700);
  assert(!c_vector_reserve(&narrow, SIZE_MAX, sizeof(uint16_t)));
  assert(!c_vector_push(&narrow, NULL, sizeof(uint16_t)));
  assert(!c_vector_reserve(&narrow, 0, 0));
  c_vector_free(&narrow);
}
static void test_logical_lines(void) {
  FILE* file = tmpfile();
  assert(file);
  for (int i = 0; i < 4096; ++i) assert(fputc('x', file) != EOF);
  assert(fputs("\n\nlast", file) >= 0);
  rewind(file);
  char* line = NULL;
  size_t capacity = 0;
  assert(c_read_line(file, &line, &capacity) == 4096);
  assert(strlen(line) == 4096 && line[4095] == 'x');
  assert(c_read_line(file, &line, &capacity) == 0 && line[0] == 0);
  assert(c_read_line(file, &line, &capacity) == 4 && !strcmp(line, "last"));
  assert(c_read_line(file, &line, &capacity) == -1);
  free(line);
  fclose(file);
}
int main(void) {
  test_dns();
  test_image();
  test_text_and_buffers();
  test_logical_lines();
  puts("C core checks passed");
  return 0;
}
