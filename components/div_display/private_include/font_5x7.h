static uint8_t glyph_row(char c, int row) {
  if (c < 32 || c > 126) {
    c = '?';
  }
  if (c == ' ') {
    return 0;
  }
  if (c >= '0' && c <= '9') {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    return digits[c - '0'][row];
  }
  if (c >= 'a' && c <= 'z') {
    static const uint8_t letters[26][7] = {
        {0, 0, 14, 1, 15, 17, 15},    {16, 16, 30, 17, 17, 17, 30},
        {0, 0, 14, 16, 16, 17, 14},   {1, 1, 15, 17, 17, 17, 15},
        {0, 0, 14, 17, 31, 16, 14},   {6, 8, 8, 28, 8, 8, 8},
        {0, 15, 17, 17, 15, 1, 14},   {16, 16, 30, 17, 17, 17, 17},
        {4, 0, 12, 4, 4, 4, 14},      {2, 0, 6, 2, 2, 18, 12},
        {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14},
        {0, 0, 26, 21, 21, 21, 21},   {0, 0, 30, 17, 17, 17, 17},
        {0, 0, 14, 17, 17, 17, 14},   {0, 0, 30, 17, 30, 16, 16},
        {0, 0, 15, 17, 15, 1, 1},     {0, 0, 22, 25, 16, 16, 16},
        {0, 0, 15, 16, 14, 1, 30},    {8, 8, 28, 8, 8, 9, 6},
        {0, 0, 17, 17, 17, 19, 13},   {0, 0, 17, 17, 17, 10, 4},
        {0, 0, 17, 17, 21, 21, 10},   {0, 0, 17, 10, 4, 10, 17},
        {0, 0, 17, 17, 15, 1, 14},    {0, 0, 31, 2, 4, 8, 31}};
    return letters[c - 'a'][row];
  }
  switch (c) {
    case 'A': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'B': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
      return r[row];
    }
    case 'C': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
      return r[row];
    }
    case 'D': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
      return r[row];
    }
    case 'E': {
      static const uint8_t r[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
      return r[row];
    }
    case 'F': {
      static const uint8_t r[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
      return r[row];
    }
    case 'G': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
      return r[row];
    }
    case 'H': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'I': {
      static const uint8_t r[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
      return r[row];
    }
    case 'J': {
      static const uint8_t r[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
      return r[row];
    }
    case 'K': {
      static const uint8_t r[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
      return r[row];
    }
    case 'L': {
      static const uint8_t r[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
      return r[row];
    }
    case 'M': {
      static const uint8_t r[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'N': {
      static const uint8_t r[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
      return r[row];
    }
    case 'O': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
      return r[row];
    }
    case 'P': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
      return r[row];
    }
    case 'Q': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
      return r[row];
    }
    case 'R': {
      static const uint8_t r[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
      return r[row];
    }
    case 'S': {
      static const uint8_t r[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
      return r[row];
    }
    case 'T': {
      static const uint8_t r[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
      return r[row];
    }
    case 'U': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
      return r[row];
    }
    case 'V': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
      return r[row];
    }
    case 'W': {
      static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
      return r[row];
    }
    case 'X': {
      static const uint8_t r[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
      return r[row];
    }
    case 'Y': {
      static const uint8_t r[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
      return r[row];
    }
    case 'Z': {
      static const uint8_t r[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
      return r[row];
    }
    case '.': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
      return r[row];
    }
    case ',': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0x0C, 0x08};
      return r[row];
    }
    case ':': {
      static const uint8_t r[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0};
      return r[row];
    }
    case ';': {
      static const uint8_t r[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x08, 0};
      return r[row];
    }
    case '-': {
      static const uint8_t r[7] = {0, 0, 0, 0x1F, 0, 0, 0};
      return r[row];
    }
    case '_': {
      static const uint8_t r[7] = {0, 0, 0, 0, 0, 0, 0x1F};
      return r[row];
    }
    case '/': {
      static const uint8_t r[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
      return r[row];
    }
    case '\\': {
      static const uint8_t r[7] = {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01};
      return r[row];
    }
    case '+': {
      static const uint8_t r[7] = {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
      return r[row];
    }
    case '*': {
      static const uint8_t r[7] = {0, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0};
      return r[row];
    }
    case '!': {
      static const uint8_t r[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04};
      return r[row];
    }
    case '?': {
      static const uint8_t r[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04};
      return r[row];
    }
    case '%': {
      static const uint8_t r[7] = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13};
      return r[row];
    }
    case '#': {
      static const uint8_t r[7] = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A};
      return r[row];
    }
    case '[': {
      static const uint8_t r[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E};
      return r[row];
    }
    case ']': {
      static const uint8_t r[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E};
      return r[row];
    }
    case '(': {
      static const uint8_t r[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
      return r[row];
    }
    case ')': {
      static const uint8_t r[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
      return r[row];
    }
    case '<': {
      static const uint8_t r[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
      return r[row];
    }
    case '>': {
      static const uint8_t r[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
      return r[row];
    }
    default: {
      static const uint8_t r[7] = {0x1F, 0x11, 0x15, 0x11, 0x15, 0x11, 0x1F};
      return r[row];
    }
  }
}
