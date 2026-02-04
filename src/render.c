#include "shared.h"
#include "lib/tui.c"

#define MAX_SCREEN_HEIGHT 300
#define MAX_SCREEN_WIDTH 800

fn str charForEntity(EntityType e) {
  switch (e) {
    case EntityStarSystem:
      return "⭐";
    case EntityCharacter:
      return "🧙";
      //return "웃";
    case EntityNull:
    case EntityType_Count:
    //default: // commented out so that we get a warning for missing entity
      return "  ";
  }
}

/* HERE IS WHERE I USUALLY PUT THE "room" OR "map" DRAWING FUNCTION
 *
fn void renderRoom(Pixel* buf, u32 x, u32 y, RenderableRoom* room, Dim2 screen_dimensions, bool active) {
  // x,y is starting cursor location of upper-left corner

  Box b = { .x = x, .y = y, .width = ROOM_WIDTH*2, .height = ROOM_HEIGHT };
  drawAnsiBox(buf, b, screen_dimensions, active);

  // start printing the rows
  // move cursor to beginning of the room
  for (i32 j = 0; j < ROOM_HEIGHT; j++) {
    for (i32 i = 0; i < ROOM_WIDTH; i++) {
      u32 roompos = XYToPos(i, j, ROOM_WIDTH);
      if (!room->visible[roompos] && room->memory[roompos] == RememberedTileQualityNone) {
        continue;
      }
      u32 bufpos = (x+1+(i*2)) + (screen_dimensions.width*(y+1+j));

      str fg_char = charForEntity(room->foreground[roompos]);
      RGB background_color = colorForTile(room->background[roompos]);
      if (room->visible[roompos]) {
        buf[bufpos].foreground = ansiColorForEntity(room->foreground[roompos]);
        if (room->light[roompos] < VISIBLE_BRIGHTNESS_CUTOFF) {
          fg_char = charForEntity(EntityMurkyUnknown);
          background_color = rgbDarken(background_color, 0.8);
        }
      } else if (room->memory[roompos] == RememberedTileQualityDim) {
        background_color = rgbDarken(background_color, 0.4);
        buf[bufpos].foreground = ansiColorForEntity(room->foreground[roompos]);
        fg_char = charForEntity(EntityMurkyUnknown);
      } else if (room->memory[roompos] == RememberedTileQualityClear) {
        background_color = rgbDarken(background_color, 0.6);
        buf[bufpos].foreground = ansiColorForEntity(room->foreground[roompos]);
      }
      buf[bufpos].background = rgbToAnsi(background_color);
      Utf8Character first_character_class = classifyUtf8Character((u8)fg_char[0]);
      // if it's a single ASCII character
      if (first_character_class == Utf8CharacterAscii && fg_char[1] == '\0') {
        buf[bufpos].bytes[0] = fg_char[0];
      // if it's a pair of ASCII characters
      } else if (first_character_class == Utf8CharacterAscii && fg_char[1] > 0 && fg_char[2] == '\0') {
        buf[bufpos].bytes[0] = fg_char[0];
        buf[bufpos+1].bytes[0] = fg_char[1];
        buf[bufpos+1].background = buf[bufpos].background;
        buf[bufpos+1].foreground = buf[bufpos].foreground;
      } else if (first_character_class == Utf8CharacterThreeByte) {
        if (strlen(fg_char) == 3) {// handle 2-wide characters
          buf[bufpos+1].background = buf[bufpos].background;
          buf[bufpos+1].foreground = buf[bufpos].foreground;
        }
        for (u32 k = 0; k < strlen(fg_char)/3; k++) {
          if (k != 0) {
            buf[bufpos+k].background = buf[bufpos].background;
            buf[bufpos+k].foreground = buf[bufpos].foreground;
          }
          for (u32 l = 0; l < 3; l++) {
            buf[bufpos+k].bytes[l] = fg_char[l+(k*3)];
          }
        }
      } else if (first_character_class == Utf8CharacterFourByte) {
        if (strlen(fg_char) == UTF8_MAX_WIDTH) {// assume all of these characters are 2-wide
          buf[bufpos+1].background = buf[bufpos].background;
          buf[bufpos+1].foreground = buf[bufpos].foreground;
        }
        for (u32 k = 0; k < strlen(fg_char)/UTF8_MAX_WIDTH; k++) {
          for (u32 l = 0; l < UTF8_MAX_WIDTH; l++) {
            buf[bufpos+k].bytes[l] = fg_char[l+(k*UTF8_MAX_WIDTH)];
          }
        }
        // handle secondary bytes that didn't divide evenly
        for (u32 k = 0; k < strlen(fg_char)%UTF8_MAX_WIDTH; k++) {
          buf[bufpos+1].background = buf[bufpos].background;
          buf[bufpos+1].foreground = buf[bufpos].foreground;
          buf[bufpos+1].bytes[k] = fg_char[UTF8_MAX_WIDTH+k];
        }
      } else {
        assert(false && "unhandled bullshit");
      }
    }
  }
}
*/
