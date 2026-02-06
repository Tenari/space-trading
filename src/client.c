/*
 * Code conventions:
 *  MyStructType
 *  myFunction()
 *  MyMacro()
 *  my_variable
 *  MY_CONSTANT
 * */
#include <string.h>
#include "base/impl.c"
#include "lib/network.c"
#include "render.c"
#include "string_chunk.c"
//#include "assets/asset1.h"
//#include "assets/asset2.h"
//#include "assets/asset3.h"

///// #define a bunch of client-only tunable game constants
#define SYSTEM_MESSAGES_LEN 32
#define MAX_SYSTEM_MESSAGE_LEN 512
#define GOAL_LOOPS_PER_S 50
#define GOAL_LOOP_US 1000000/GOAL_LOOPS_PER_S
#define LOGIN_NAME_BUFFER_LEN 16
#define PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN 16
#define MAIN_GAME_TAB_COUNT (2)

///// TYPES
typedef enum Screen {
  ScreenLogin,
  ScreenCreateCharacter,
  ScreenMainGame,
  ScreenDefeat,
  ScreenVictory,
  Screen_Count
} Screen;

typedef enum LoginScreenState {
  LoginScreenStateInit,
  LoginScreenStateLoading,
  LoginScreenStateBadPw,
  LoginScreenState_Count
} LoginScreenState;

typedef enum SpeechTailDirection {
  SpeechTailDirectionBottomRight,
  SpeechTailDirectionBottomLeft,
  SpeechTailDirectionRight,
  SpeechTailDirectionLeft,
  SpeechTailDirection_Count
} SpeechTailDirection;

typedef struct Entity {
  EntityType type;
  u8 x;
  u8 y;
  u8 color;
  u64 features;
  u64 id;
  StringChunkList name;
} Entity;

typedef struct EntityList {
  u64 length; // the currently used length
  u64 capacity;
  Entity* items;
} EntityList;

typedef struct ParsedServerMessage {
  Message type;
  u16 port;
  u16 port2;
  u32 ip;
  u32 ip2;
  u64 id;
  u64 server_frame;
  XYZ xyz;
  //Entity entities[PARSED_CLIENT_ENTITY_LEN];
  //u64 ids[PARSED_IDS_LEN];
} ParsedServerMessage;

typedef struct ParsedServerMessageThreadQueue {
  ParsedServerMessage items[PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN];
  u32 head;
  u32 tail;
  u32 count;
  Mutex mutex;
  Cond not_empty;
  Cond not_full;
} ParsedServerMessageThreadQueue;

typedef struct LoginState {
  LoginScreenState state;
  u8 selected_field;
  u8 field_index;
  String name;
  String password;
} LoginState;

typedef struct MenuState {
  u8 selected_index;
  u8 len;
  u64 id;
} MenuState;

typedef struct GameState {
  Screen screen;
  Screen old_screen;
  EntityList entities;
  Entity me;
  u64 server_frame;
  u64 loop_count;
  Arena entity_arena;
  StringArena string_arena;
  LoginState login_state;
  MenuState menu;
  MenuState section; // for tabbing through selected "portions" of the screen
  u8 choices[4];
  UDPMessage keep_alive_msg;
  UDPClient client;
  StringChunkList message_input;
} GameState;

///// GLOBALS
global bool should_quit;
global bool debug_mode = false;
global Arena permanent_arena = {0};
global u8List system_messages[SYSTEM_MESSAGES_LEN] = {0};
global u8 system_message_index = 0;
global GameState state = {0};
global OutgoingMessageQueue* network_send_queue = {0};
global ParsedServerMessageThreadQueue* network_recv_queue = {0};
global str TABS[] = {"Debug", "Speak"};

///// FUNCTIONS
fn ParsedServerMessageThreadQueue* newPSMThreadQueue(Arena* a) {
  ParsedServerMessageThreadQueue* result = arenaAlloc(a, sizeof(ParsedServerMessageThreadQueue));
  MemoryZero(result, (sizeof *result));
  result->mutex = newMutex();
  result->not_full = newCond();
  result->not_empty = newCond();
  return result;
}

fn void psmThreadSafeQueuePush(ParsedServerMessageThreadQueue* queue, ParsedServerMessage* msg) {
  lockMutex(&queue->mutex); {
    while (queue->count == PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN) {
      waitForCondSignal(&queue->not_full, &queue->mutex);
    }

    MemoryCopy(&queue->items[queue->tail], msg, (sizeof *msg));
    queue->tail = (queue->tail + 1) % PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN;
    queue->count++;

    signalCond(&queue->not_empty);
  } unlockMutex(&queue->mutex);
}

fn ParsedServerMessage* psmThreadSafeNonblockingQueuePop(ParsedServerMessageThreadQueue* q, ParsedServerMessage* copy_target) {
  // immediately returns NULL if there's nothing in the ThreadQueue
  // copies the ParsedServerMessage into `copy_target` if there is something in the queue
  // and marks it as popped from the queue
  ParsedServerMessage* result = NULL;

  lockMutex(&q->mutex); {
    if (q->count > 0) {
      result = &q->items[q->head];
      MemoryCopy(copy_target, result, (sizeof *copy_target));
      q->head = (q->head + 1) % PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN;
      q->count--;

      signalCond(&q->not_full);
    }
  } unlockMutex(&q->mutex);

  return result;
}

fn void entityPush(EntityList* list, Entity e) {
  if (list->length >= list->capacity) {
    arenaAllocArray(&state.entity_arena, Entity, list->capacity);
    list->capacity = list->capacity * 2;
  }
  list->items[list->length] = e;
  list->length += 1;
}

fn bool entityDelete(EntityList* list, u64 id) {
  assert(list->length > 0);
  if (list->items[list->length - 1].id == id) {
    list->length -= 1;
    return true;
  } else {
    for (u32 i = 0; i < list->length; i++) {
      if (list->items[i].id == id) {
        // copy the last one over the one we are deleting
        list->items[i] = list->items[list->length - 1];
        list->length -= 1;
        return true;
      }
    }
  }
  return false;
}

fn void addSystemMessage(u8* msg) {
  // save the message to our system_messages ring buffer
  memset(system_messages[system_message_index].items, 0, SYSTEM_MESSAGES_LEN);
  sprintf((char*)system_messages[system_message_index].items, "%s", msg);
  system_messages[system_message_index].length = strlen((char*)system_messages[system_message_index].items);
  system_message_index += 1;
  if (system_message_index == SYSTEM_MESSAGES_LEN) {
    system_message_index = 0;
  }
}

fn void renderSystemMessages(Pixel* buf, Dim2 screen_dimensions, Box sys_msg_box) {
  i32 printable_lines = sys_msg_box.height - 2;
  if (printable_lines > SYSTEM_MESSAGES_LEN) {
    printable_lines = SYSTEM_MESSAGES_LEN;
  }
  for (i32 i = 0; i < printable_lines; i++) {
    i32 index = (system_message_index - 1 - i);
    if (index < 0) {
      index = SYSTEM_MESSAGES_LEN + index;
    }
    u32 y = sys_msg_box.y + (sys_msg_box.height - i) - 1;
    u8List sys_msg = system_messages[index];
    for (i32 j = 0; j < MAX_SYSTEM_MESSAGE_LEN && j < sys_msg_box.width-4; j++) {
      u32 pos = (sys_msg_box.x + 2+j) + (screen_dimensions.width * y);
      if (j < sys_msg.length) {
        if (sys_msg.items[j] != '\n') {
          buf[pos].bytes[0] = sys_msg.items[j];
        }
      }
    }
  }
}

fn void renderPercentBar(TuiState* tui, u16 x, u16 y, u16 width, u8 ansi_color, u64 value, u64 max) {
  Pixel* buf = tui->frame_buffer;
  Dim2 screen_dimensions = tui->screen_dimensions;
  u16 pos = x + (screen_dimensions.width * y);
  buf[pos].bytes[0] = '[';
  pos = x+width + (screen_dimensions.width * y);
  buf[pos].bytes[0] = ']';
  pos = x+1 + (screen_dimensions.width * y);
  f32 base_ratio = 0;
  if (max != 0) {
    base_ratio = ((f32)value / (f32)max);
  }
  f32 raw_ratio = base_ratio * (width-2);
  u32 full_spaces_count = (u32) raw_ratio;
  f32 remainder = raw_ratio - full_spaces_count;
  for (u32 i = 0; i < full_spaces_count; i++) {
    buf[pos+i].foreground = ansi_color;
    renderUtf8CharToBuffer(buf, x+1+i, y, "█", screen_dimensions);
  }
  buf[pos+full_spaces_count].foreground = ansi_color;
  if (value == max) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "█", screen_dimensions);
  } else if (remainder > 0.875) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▉", screen_dimensions);
  } else if (remainder > 0.75) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▊", screen_dimensions);
  } else if (remainder > 0.625) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▋", screen_dimensions);
  } else if (remainder > 0.5) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▌", screen_dimensions);
  } else if (remainder > 0.375) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▍", screen_dimensions);
  } else if (remainder > 0.25) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▎", screen_dimensions);
  } else if (remainder > 0.125) {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, "▏", screen_dimensions);
  } else {
    renderUtf8CharToBuffer(buf, x+1+full_spaces_count, y, " ", screen_dimensions);
  }
}

fn void renderSpeechBubble(TuiState* tui, u16 x, u16 y, u16 max_width, String message, SpeechTailDirection dir) {
  Pixel* buf = tui->frame_buffer;
  Dim2 screen_dimensions = tui->screen_dimensions;
  // calculate dimenions
  u32 height = (message.length / max_width) + 1;
  u32 width = message.length;
  if (message.length > max_width) {
    width = max_width;
  }
  if (width < 12) {
    width = 12; // minimum width
  }

  // print the upper border
  renderUtf8CharToBuffer(buf, x, y, "╭", screen_dimensions);
  for (i32 i = 0; i < width+1; i++) {
    renderUtf8CharToBuffer(buf, x+1+i, y, "─", screen_dimensions);
  }
  renderUtf8CharToBuffer(buf, x+width+1, y, "╮", screen_dimensions);

  // start printing the rows
  for (u32 i = 0; i < height; i++) {
    renderUtf8CharToBuffer(buf, x, y+1+i, "│", screen_dimensions);
    // print the line of text
    for (u32 j = 0; j < width; j++) {
      u32 character_index = (i*(width-1))+j;
      u16 pos = (x+1+j) + (screen_dimensions.width * (y+1+i));
      if (message.length > character_index) {
        buf[pos].bytes[0] = message.bytes[character_index];
      } else {
        buf[pos].bytes[0] = ' ';
      }
    }
    renderUtf8CharToBuffer(buf, x+1+width, y+1+i, "│", screen_dimensions);
  }

  // print the bottom box border
  renderUtf8CharToBuffer(buf, x, y+1+height, "╰", screen_dimensions);
  for (i32 i = 0; i < width+1; i++) {
    renderUtf8CharToBuffer(buf, x+1+i, y+1+height, "─", screen_dimensions);
  }
  renderUtf8CharToBuffer(buf, x+width+1, y+1+height, "╯", screen_dimensions);
  // TODO actually print the speech tail
}

fn void renderStaticAssetToPixelBuffer(TuiState* tui, u8* asset, u32 len, u16 x, u16 y) {
  u16 line = 0;
  u16 x_in_line = 0;
  u16 pos = x + (tui->screen_dimensions.width * y);
  for (u32 i = 0; i < len; i++, x_in_line++) {
    if (asset[i] == '\n') {
      line += 1;
      x_in_line = 0;
      pos = x + (tui->screen_dimensions.width * (y+line));
    } else if (asset[i] == ' ') {
      // do nothing, we skip spaces in our assets
    } else {
      tui->frame_buffer[pos+x_in_line].bytes[0] = asset[i];
    }
  }
}

fn void clearServerSentState() {
  //memset(&state.current_room, 0, sizeof(RenderableRoom));

  arenaClear(&state.entity_arena);
  state.entities.capacity = 64;
  state.entities.length = 0;
  state.entities.items = arenaAllocArray(&state.entity_arena, Entity, state.entities.capacity);
}

fn void handleIncomingMessage(u8* message, u32 len, SocketAddress sender, i32 socket) {
  Message msg_type = message[0];
  dbg("handleIncomingMessage() of len=%d, message=%s\n", len, MESSAGE_STRINGS[msg_type]);
  u8List bytes = {len, len, message};
  u64 msg_pos = 1;
  ParsedServerMessage parsed = {0};
  parsed.type = msg_type;
  switch (msg_type) {
    case MessageNewAccountCreated:
    case MessageBadPw: {/*nothing to parse but the type*/} break;
    case MessageCharacterId: {
      parsed.id = readU64FromBufferLE(message + 1);
    } break;
    case Message_Count:
      assert(false && "invalid msg_type detected");
      break;
    case MessageInvalid:
      addSystemMessage((u8*)"NOT IMPLEMENTED");
      break;
  }
  psmThreadSafeQueuePush(network_recv_queue, &parsed);
}

fn void* receiveNetworkUpdates(void* udp) {
  UDPClient client = *(UDPClient*)udp;
  dbg("receiveNetworkUpdates() sock=%d\n", client.socket);
  UDPServer server = {
    .ready = true,
    .server_address = client.server_address,
    .server_socket = client.socket
  };
  infiniteReadUDPServer(&server, handleIncomingMessage);
  return NULL;
}

fn void* sendNetworkUpdates(void* udp) {
  i32 socket_fd = ((UDPClient*)udp)->socket;
  dbg("sendNetworkUpdates() sock=%d\n", socket_fd);
  u8List bytes_list = {0};
  while (!should_quit) {
    UDPMessage msg = {0};
    outgoingMessageQueuePop(network_send_queue, &msg);
    bytes_list.items = msg.bytes;
    bytes_list.length = msg.bytes_len;
    bytes_list.capacity = msg.bytes_len;
    sendUDPu8List(socket_fd, &msg.address, &bytes_list);
  }
  return NULL;
}

fn bool updateAndRender(TuiState* tui, void* s, u8* input_buffer, u64 loop_count) {
  GameState* state = (GameState*) s;
  state->loop_count = loop_count;
  UDPClient* udp = &state->client;
  state->old_screen = state->screen;
  Dim2 screen_dimensions = tui->screen_dimensions;
  bool game_screen_changed = state->old_screen != state->screen; // detect new screens
  tui->redraw = tui->redraw || game_screen_changed;
  if (screen_dimensions.height > MAX_SCREEN_HEIGHT || screen_dimensions.width > MAX_SCREEN_WIDTH) {
    printf("\033[2J\033[%d;%df Your screen is too damn big. Shrink it to %dx%d max, bro... geez", 1,1, MAX_SCREEN_WIDTH, MAX_SCREEN_HEIGHT);
    fflush(stdout);
    return should_quit;
  }

  // process server messages
  u32 msg_iters = 0;
  ParsedServerMessage msg = {0};
  ParsedServerMessage* next_net_msg = psmThreadSafeNonblockingQueuePop(network_recv_queue, &msg);
  while (next_net_msg != NULL) {
    msg_iters += 0;
    switch (msg.type) {
      case MessageNewAccountCreated: {
        state->screen = ScreenCreateCharacter;
      } break;
      case MessageBadPw: {
        state->login_state.state = LoginScreenStateBadPw;
      } break;
      case MessageCharacterId: {
        state->me.id = msg.id;
        char bytes[256] = {0};
        sprintf(bytes,"got character id: %lld", msg.id);
        addSystemMessage((u8*)bytes);
        state->screen = ScreenMainGame;
        state->menu.selected_index = 0;
        state->section.selected_index = 0;
      } break;
      case Message_Count:
      case MessageInvalid:
        assert(false && "invalid message from queue");
        break;
    }
    next_net_msg = psmThreadSafeNonblockingQueuePop(network_recv_queue, &msg);
    msg_iters++;
  }
  
  // operate on screen-based input+state
  bool user_pressed_esc = input_buffer[0] == ASCII_ESCAPE && input_buffer[1] == 0;
  bool user_pressed_a_number = input_buffer[0] >= '1' && input_buffer[0] <= '9' && input_buffer[1] == 0;
  bool user_pressed_up = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 65;
  bool user_pressed_down = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 66;
  bool user_pressed_left = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 68;
  bool user_pressed_right = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 67;
	bool user_pressed_backspace = input_buffer[0] == ASCII_BACKSPACE || input_buffer[0] == ASCII_DEL;
  bool user_pressed_enter = input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED;
  switch (state->screen) {
    case ScreenCreateCharacter: {
      //// SIMULATION
      if (state->me.id != 0) {
        state->screen = ScreenMainGame;
        break;
      }
      if (state->section.selected_index == 0) {
        state->menu.len = ShipType_Count;
      }

      if (input_buffer[0] == 'q' || user_pressed_esc) {
        should_quit = true;
      } else if (user_pressed_down || user_pressed_right) {
        state->menu.selected_index++;
      } else if (user_pressed_up || user_pressed_left) {
        state->menu.selected_index--;
      } else if (user_pressed_a_number) {
        state->menu.selected_index = input_buffer[0] - '1';
      } else if (input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED) {
        if (state->section.selected_index == 0) {
          state->choices[0] = state->menu.selected_index;
          state->section.selected_index += 1;
          state->menu.selected_index = 0;
          state->menu.len = 2;
        } else {
          // send character color to server
          UDPMessage msg = {0};
          msg.address = udp->server_address;
          msg.bytes_len = 2;
          // 1. msg type/CommandType
          msg.bytes[0] = CommandCreateCharacter;
          // 2. what color
          msg.bytes[1] = ANSI_HP_RED;//WIZARD_COLORS[state->choices[0]];

          outgoingMessageQueuePush(network_send_queue, &msg);

          state->screen = ScreenMainGame;
        }
      }
      if (state->menu.selected_index >= state->menu.len) {
        state->menu.selected_index = 0;
      }

      //// RENDERING
      u16 line = 2;
      // 1. draw the outline
      Box b = { .x = 2, .y = line, .width = screen_dimensions.width - 5, .height = screen_dimensions.height - 5 };
      drawAnsiBox(tui->frame_buffer, b, screen_dimensions, true);

      // 2. draw the header label
      str label = "Create Character";
      renderStrToBuffer(tui->frame_buffer, (screen_dimensions.width - (strlen(label)+4))/2, ++line, label, screen_dimensions);

      line++;
      // TODO: game-specific character creation stuff
      // 3. draw the ship choices
      str ship_label = "Choose your starting ship:";
      line++;
      // the table
      str cols[SHIP_DETAIL_COUNT] = {"Type", "Cost", "DriveEff", "LifeSupp", "V.Cargo", "C.Cargo", "Passengers", "SmugglersHold"};
      u32 table_x = 5;
      u32 table_col_pad = 1;
      // render the headers
      u32 col_x_pos = 0;
      for (u32 i = 0; i < SHIP_DETAIL_COUNT; col_x_pos += SHIP_FIELDS[i].width+table_col_pad, i++) {
        renderStrToBuffer(tui->frame_buffer, table_x+col_x_pos, line, cols[i], screen_dimensions);
        for (u32 ii = 0; ii < SHIP_FIELDS[i].width; ii++) {
          renderUtf8CharToBuffer(tui->frame_buffer, table_x+col_x_pos+ii, line+1, "━", screen_dimensions);
        }
      }
      line++;
      line++;
      // render the columns
      char tmp_buffer[32] = {0};
      for (u32 i = 0; i < ShipType_Count; line++, i++) {
        col_x_pos = 0;
        ShipTemplate ship = SHIPS[i];
        for (u32 ii = 0; ii < SHIP_DETAIL_COUNT; col_x_pos += SHIP_FIELDS[ii].width+table_col_pad, ii++) {
          FieldDescriptor details = SHIP_FIELDS[ii];
          void *field_ptr = (char *)&ship + details.offset;
          switch (details.type) {
            case FieldTypeEnum: {
              renderStrToBuffer(tui->frame_buffer, table_x+col_x_pos, line, details.enum_vals[ship.type], screen_dimensions);
            } break;
            case FieldTypeU8: {
              MemoryZero(tmp_buffer, 32);
              sprintf(tmp_buffer, "%d", *(u8 *)field_ptr);
              renderStrToBuffer(tui->frame_buffer, table_x+col_x_pos, line, tmp_buffer, screen_dimensions);
            } break;
            case FieldTypeU16: {
              MemoryZero(tmp_buffer, 32);
              sprintf(tmp_buffer, "%d", *(u16 *)field_ptr);
              renderStrToBuffer(tui->frame_buffer, table_x+col_x_pos, line, tmp_buffer, screen_dimensions);
            } break;
            case FieldTypeU32: {
              MemoryZero(tmp_buffer, 32);
              sprintf(tmp_buffer, "%d", *(u32 *)field_ptr);
              renderStrToBuffer(tui->frame_buffer, table_x+col_x_pos, line, tmp_buffer, screen_dimensions);
            } break;
          }
        }
      }
      /*
      renderStrToBuffer(tui->frame_buffer, 5, ++line, ship_label, screen_dimensions);
      line++;
      renderChoiceMenu(
        tui,
        5,
        line,
        (ptr*)SHIP_TYPE_STRINGS,
        ShipType_Count,
        state->section.selected_index == 0,
        state->menu.selected_index,
        NULL
      );
      */
    } break;
    case ScreenMainGame: {
      // SIMULATION
      if (input_buffer[0] == 'q' || user_pressed_esc) {
        should_quit = true;
      }

      // RENDERING
      renderStrToBuffer(tui->frame_buffer, 5, 1, "Games typically would render something here...", screen_dimensions);
      bool room_active = state->section.selected_index == 0;
      u32 tabs_y = 1 + 2 + 2;
      u32 tx = 2;
      for (u32 i = 0; i < MAIN_GAME_TAB_COUNT; i++) {
        u32 tab_len = strlen(TABS[i]);
        Box b = { .x = tx, .y = tabs_y, .width = tab_len+1, .height = 1 };
        drawAnsiBox(tui->frame_buffer, b, screen_dimensions, state->menu.selected_index == i);
        renderStrToBuffer(tui->frame_buffer, tx+1, tabs_y+ 1, TABS[i], screen_dimensions);
        tx += (tab_len + 3);
      }
      // draw the system messages box
      u32 box_y = tabs_y + 3;
      Box box = {
        .x = 1,
        .y = box_y,
        .width = screen_dimensions.width - 4,
        .height = screen_dimensions.height - box_y - 2,
      };
      drawAnsiBox(tui->frame_buffer, box, screen_dimensions, !room_active);
      if (state->menu.selected_index == 0) { // 0 is TABS[0] is "Debug"
        renderSystemMessages(tui->frame_buffer, tui->screen_dimensions, box);
      } else if (state->menu.selected_index == 1) { // 1 is TABS[1] is "Speak"
        renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+1, "You haven't heard anything interesting lately...", screen_dimensions);
      }
    } break;
    case ScreenLogin: {
      // SIMULATION
      if (user_pressed_esc) {
        should_quit = true;
      }
      if (state->login_state.state == LoginScreenStateLoading) {
        break;
      }
      String* field;
      if (state->login_state.selected_field == 0) {
        field = &state->login_state.name;
      } else {
        field = &state->login_state.password;
      }
      if (isAlphaUnderscoreSpace(input_buffer[0])) {
        field->bytes[state->login_state.field_index] = input_buffer[0];
        if (state->login_state.field_index+1 < field->capacity) {
          state->login_state.field_index += 1;
          field->length += 1;
        }
      } else if (user_pressed_backspace) {
        field->bytes[state->login_state.field_index] = 0;
        if (state->login_state.field_index > 0) {
          state->login_state.field_index -= 1;
          field->length -= 1;
        }
      } else if (input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED) {
        if (state->login_state.selected_field == 0) {
          state->login_state.selected_field = 1;
          state->login_state.field_index = 0;
        } else {
          u32 msg_idx = 0;
          // drop the login message into the network_send_queue
          UDPMessage msg = {0};
          msg.address = udp->server_address;
          // 1. msg type/CommandType
          msg.bytes[msg_idx++] = CommandLogin;
          // 2. our LAN-IP to handle the case where we are on the same LAN as the guy we are trying to fight
          // and our "listened" UDP port
          msg_idx += writeU16ToBufferLE(msg.bytes + msg_idx, ~(udp->client_port));
          msg_idx += writeI32ToBufferLE(msg.bytes + msg_idx, ~osLanIPAddress());
          // 2. how long is the name
          msg.bytes[msg_idx++] = state->login_state.name.length;
          // 3. the name
          memcpy(msg.bytes + msg_idx, state->login_state.name.bytes, state->login_state.name.length);
          // 4. the password
          memcpy(msg.bytes + msg_idx + state->login_state.name.length, state->login_state.password.bytes, state->login_state.password.length);
          msg.bytes_len = msg_idx + state->login_state.name.length + state->login_state.password.length;
          addSystemMessage((u8*)state->login_state.name.bytes);
          addSystemMessage((u8*)state->login_state.password.bytes);
          outgoingMessageQueuePush(network_send_queue, &msg);
          // show loading signal, stop accepting input
          state->login_state.state = LoginScreenStateLoading;
        }
      }

      // RENDERING
      // TODO: 1. DiffieHelman exchange
      // 2. encrypt password
      // 3. send [CommandLogin uname-len username encrypted_pass]
      if (screen_dimensions.height < 30 || screen_dimensions.width < 80) {
        str warning_label = "Screen too smol. Plz resize.";
        renderStrToBuffer(tui->frame_buffer, 6, 2, warning_label, screen_dimensions);
      } else {
        u32 width = screen_dimensions.width / 3;
        u32 height = screen_dimensions.height / 4;
        u32 x = (screen_dimensions.width - width) / 2; // center
        u32 y = (screen_dimensions.height - height) / 2; // center
        // outline
        Box b = { .x = x, .y = y, .width = width, .height = height };
        drawAnsiBox(tui->frame_buffer, b, screen_dimensions, true);
        // labels
        str login_label = " Please login:";
        renderStrToBuffer(tui->frame_buffer, x+1, y+1, login_label, screen_dimensions);
        str name_label = "    Name: ";
        renderStrToBuffer(tui->frame_buffer, x+1, y+2, name_label, screen_dimensions);
        str pw_label = "    Password: ";
        renderStrToBuffer(tui->frame_buffer, x+1, y+3, pw_label, screen_dimensions);
        // render name
        u16 pos = ((y+2)*screen_dimensions.width) + (x+11);
        for (u32 i = 0; i < state->login_state.name.length; i++) {
          tui->frame_buffer[pos+i].bytes[0] = state->login_state.name.bytes[i];
        }
        // render password
        pos = ((y+3)*screen_dimensions.width) + (x+15);
        for (u32 i = 0; i < state->login_state.password.length; i++) {
          tui->frame_buffer[pos+i].bytes[0] = '*';
        }
        if (state->login_state.state == LoginScreenStateLoading) {
          renderStrToBuffer(tui->frame_buffer, x+5, y+5, "Loading...", screen_dimensions);
        } else if (state->login_state.state == LoginScreenStateBadPw) {
          renderStrToBuffer(tui->frame_buffer, x+5, y+5, "Bad password, dumbass", screen_dimensions);
        }
        // calculate where to put the cursor
        if (state->login_state.selected_field == 0) {
          tui->cursor.y = y+2;
          tui->cursor.x = x+11+state->login_state.name.length;
        } else {
          tui->cursor.y = y+3;
          tui->cursor.x = x+15+state->login_state.password.length;
        }
      }
    } break;
    case ScreenVictory: {
      // SIMULATION
      if (user_pressed_esc || input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED) {
        state->screen = ScreenMainGame;
      }
      
      // RENDERING
      renderStrToBuffer(tui->frame_buffer, 10, 10, "You WIN!!!", screen_dimensions);
    } break;
    case ScreenDefeat: {
      // SIMULATION
      if (user_pressed_esc || input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED) {
        state->me.id = 0;
        state->menu.selected_index = 0;
        state->section.selected_index = 0;
        clearServerSentState();
        state->screen = ScreenCreateCharacter;
      }

      // RENDERING
      renderStrToBuffer(tui->frame_buffer, 10, 10, "You died...", screen_dimensions);
      renderStrToBuffer(tui->frame_buffer, 10, 11, "[press ESC to continue]", screen_dimensions);
    } break;
    case Screen_Count:
      assert(false && "unhandled screen");
      break;
  }
  
  if (loop_count % 10 == 0) {
    outgoingMessageQueuePush(network_send_queue, &state->keep_alive_msg);
    //outgoingMessageQueuePush(network_send_queue, &testm);
  }

  return should_quit;
}

i32 main(i32 argc, ptr argv[]) {
  assert(Message_Count < 256);
  assert(CommandType_Count < 256);
  osInit();
  // 3 threads, each their own loop:
  //  1. recvNetwork()
  //  2. sendNetwork()
  //  3. input/update/draw "normal" gameloop
  //    - which is actually "wide" doing each room in parallel across N threads

  // clear and init the state
  arenaInit(&permanent_arena);
  arenaInit(&state.entity_arena);
  state.entities.capacity = 64;
  state.entities.length = 0;
  state.entities.items = arenaAllocArray(&state.entity_arena, Entity, state.entities.capacity);
  arenaInit(&state.string_arena.a);
  state.string_arena.mutex = newMutex();
  state.message_input = stringChunkListInit(&state.string_arena);
  for (i32 i = 0; i < SYSTEM_MESSAGES_LEN; i++) {
    system_messages[i].capacity = MAX_SYSTEM_MESSAGE_LEN;
    system_messages[i].length = 0;
    system_messages[i].items = arenaAllocArraySized(&permanent_arena, sizeof(u8), MAX_SYSTEM_MESSAGE_LEN);
  }

  // network queues
  network_send_queue = newOutgoingMessageQueue(&permanent_arena);
  network_recv_queue = newPSMThreadQueue(&permanent_arena);
 
  // draw login screen, get input username + password
  state.screen = ScreenLogin;
  state.login_state.name.capacity = LOGIN_NAME_BUFFER_LEN;
  state.login_state.name.bytes = arenaAllocArray(&permanent_arena, char, state.login_state.name.capacity);
  state.login_state.password.capacity = LOGIN_NAME_BUFFER_LEN;
  state.login_state.password.bytes = arenaAllocArray(&permanent_arena, char, state.login_state.password.capacity);

  // network incantation for connecting to the server
  if (osInitNetwork() != true) {
    printf("bad network startup");
    exit(1);
  }
  state.client = createUDPClient(7777, argc > 1 ? argv[1] : NULL);

  // "hardcoded" keep alive message to periodically send to server
  state.keep_alive_msg.address = state.client.server_address;
  state.keep_alive_msg.bytes[0] = CommandKeepAlive;
  state.keep_alive_msg.bytes_len = 1;

  Thread recv_thread = spawnThread(&receiveNetworkUpdates, &state.client);
  Thread send_thread = spawnThread(&sendNetworkUpdates, &state.client);

  // game loop (read input, simulate next frame, render)
  infiniteUILoop(
    MAX_SCREEN_WIDTH,
    MAX_SCREEN_HEIGHT,
    GOAL_LOOP_US,
    &state,
    updateAndRender
  );
  return 0;
}
