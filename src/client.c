/*
 * Code conventions:
 *  MyStructType
 *  myFunction()
 *  MyMacro()
 *  my_variable
 *  MY_CONSTANT
 * */
#include <string.h>
#include <math.h>
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
#define TURN_TICK_S (3)
#define TURN_TICK_LEN (GOAL_LOOPS_PER_S * TURN_TICK_S)
#define LOGIN_NAME_BUFFER_LEN 16
#define PARSED_SERVER_MESSAGE_THREAD_QUEUE_LEN 16
#define SBUFLEN (512)

///// TYPES
typedef enum Tab {
  TabDebug,
  TabChat,
  TabMap,
  TabShip,
  TabStation,
  Tab_Count,
} Tab;

typedef enum ShipTabStates {
  ShipTabStateMain,
  ShipTabStatePayoffModal,
  ShipTabStateLoading,
  ShipTabStates_Count,
} ShipTabStates;

typedef enum StationTabStates {
  StationTabStateTable,
  StationTabStateTransact,
  StationTabStateResult,
  StationTabStateLoading,
  StationTabStates_Count,
} StationTabStates;

typedef enum Screen {
  ScreenLogin,
  ScreenCreateCharacter,
  ScreenMainGame,
  ScreenDefeat,
  ScreenVictory,
  ScreenTurnTick,
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

typedef struct TransactionResult {
  bool buying;
  u32 qty;
  u32 cr;
} TransactionResult;

typedef struct ParsedServerMessage {
  Message type;
  u16 port;
  u16 port2;
  u32 ip;
  u32 ip2;
  u64 id;
  u64 server_frame;
  XYZ xyz;
  Pos2u8 positions[STAR_SYSTEM_COUNT];
  PlanetType planet_types[MAX_PLANETS*STAR_SYSTEM_COUNT];
  StarSystem sys;
  PlayerShip ship;
  TransactionResult tx_result;
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
  PlayerShip me;
  u64 server_frame;
  u64 loop_count;
  Arena entity_arena;
  StringArena string_arena;
  LoginState login_state;
  MenuState menu;
  MenuState row; // for tracking which row of a table the user has selected
  MenuState modal_choice;
  UDPMessage keep_alive_msg;
  UDPClient client;
  StringChunkList message_input;
  StarSystem map[STAR_SYSTEM_COUNT];
  Pos2 pos;
  u8 destination_sys_idx;
  StationTabStates station_tab_state;
  ShipTabStates ship_tab_states;
  TransactionResult tx_result;
  u64 turn_tick_started_on;
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
global str TAB_STRS[Tab_Count] = {"Debug", "Chat", "Map", "Ship", "Station"};

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
  arenaClear(&state.entity_arena);
  state.entities.capacity = 64;
  state.entities.length = 0;
  state.entities.items = arenaAllocArray(&state.entity_arena, Entity, state.entities.capacity);
}

fn void resetTabRow(Tab tab) {
  if (state.menu.selected_index == TabStation) {
    state.row.len = Commodity_Count;
    state.row.selected_index = 0;
  } else if (state.menu.selected_index == TabShip) {
    state.ship_tab_states = ShipTabStateMain;
    state.row.len = 2;
    state.row.selected_index = 0;
  }
}

fn void moveRowUpDown(bool user_pressed_up, bool user_pressed_down) {
  if (user_pressed_up) {
    if (state.row.selected_index == 0) {
      state.row.selected_index = state.row.len - 1;
    } else {
      state.row.selected_index -= 1;
    }
  } else if (user_pressed_down) {
    if (state.row.selected_index == state.row.len - 1) {
      state.row.selected_index = 0;
    } else {
      state.row.selected_index += 1;
    }
  }
}

fn void handleIncomingMessage(u8* message, u32 len, SocketAddress sender, i32 socket) {
  u64 msg_pos = 0;
  Message msg_type = message[msg_pos++];
  dbg("handleIncomingMessage() of len=%d, message=%s\n", len, MESSAGE_STRINGS[msg_type]);
  u8List bytes = {len, len, message};
  ParsedServerMessage parsed = {0};
  parsed.type = msg_type;
  switch (msg_type) {
    case MessagePayoffResult:
    case MessageTurnTick:
    case MessageNewAccountCreated:
    case MessageBadPw: {/*nothing to parse but the type*/} break;
    case MessageTransactionResult: {
      parsed.tx_result.buying = message[msg_pos++];
      parsed.tx_result.qty = readU32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.tx_result.cr = readU32FromBufferLE(message + msg_pos);
      msg_pos += 4;
    } break;
    case MessageStarPositions: {
      u32 star_msg_size = 2+MAX_PLANETS;
      for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
        parsed.positions[i].x = message[1+(i*star_msg_size)];
        parsed.positions[i].y = message[2+(i*star_msg_size)];
        for (u32 ii = 0; ii < MAX_PLANETS; ii++) {
          parsed.planet_types[i*MAX_PLANETS + ii] = message[(i*star_msg_size)+3+ii];
        }
      }
    } break;
    case MessageSystemCommodities: {
      parsed.id = (u64)message[msg_pos++];
      u32 planet_count = message[msg_pos++];
      for (u32 i = 0; i < planet_count; i++) {
        for (u32 ii = 0; ii < Commodity_Count; ii++) {
          parsed.sys.planets[i].commodities[ii] = message[msg_pos++];
        }
      }
    } break;
    case MessagePlayerDetails: {
      parsed.ship.type = message[msg_pos++];
      parsed.ship.system_idx = message[msg_pos++];
      parsed.ship.ready_to_depart = message[msg_pos++];
      parsed.ship.drive_efficiency = message[msg_pos++];
      parsed.ship.life_support_efficiency = message[msg_pos++];
      parsed.ship.vacuum_cargo_slots = readU16FromBufferLE(message + msg_pos);
      msg_pos += 2;
      parsed.ship.climate_cargo_slots = readU16FromBufferLE(message + msg_pos);
      msg_pos += 2;
      parsed.ship.passenger_berths = readU16FromBufferLE(message + msg_pos);
      msg_pos += 2;
      parsed.ship.passenger_amenities_flags = readU16FromBufferLE(message + msg_pos);
      msg_pos += 2;
      parsed.ship.smugglers_hold_cu_m = readU16FromBufferLE(message + msg_pos);
      msg_pos += 2;
      parsed.ship.remaining_mortgage = readU32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.ship.interest_rate = readF32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.ship.cu_m_fuel = readU32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.ship.cu_m_o2 = readU32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.ship.credits = readF32FromBufferLE(message + msg_pos);
      msg_pos += 4;
      parsed.ship.id = readU64FromBufferLE(message + msg_pos);
      msg_pos += 8;
      for (u32 i = 0; i < Commodity_Count; i++, msg_pos += 4) {
        parsed.ship.commodities[i] = readU32FromBufferLE(message + msg_pos);
      }
    } break;
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
  char sbuf[SBUFLEN] = {0};

  // process server messages
  u32 msg_iters = 0;
  ParsedServerMessage msg = {0};
  ParsedServerMessage* next_net_msg = psmThreadSafeNonblockingQueuePop(network_recv_queue, &msg);
  while (next_net_msg != NULL) {
    msg_iters += 0;
    switch (msg.type) {
      case MessagePayoffResult: {
        state->ship_tab_states = ShipTabStateMain;
      } break;
      case MessageTurnTick: {
        state->screen = ScreenTurnTick;
        state->turn_tick_started_on = loop_count;
      } break;
      case MessageTransactionResult: {
        state->station_tab_state = StationTabStateResult;
        MemoryCopyStruct(&state->tx_result, &msg.tx_result);
      } break;
      case MessagePlayerDetails: {
        if (msg.ship.id == state->me.id) {
          state->me.type = msg.ship.type;
          state->me.system_idx = msg.ship.system_idx;
          state->me.ready_to_depart = msg.ship.ready_to_depart;
          state->me.drive_efficiency = msg.ship.drive_efficiency;
          state->me.life_support_efficiency = msg.ship.life_support_efficiency;
          state->me.vacuum_cargo_slots = msg.ship.vacuum_cargo_slots;
          state->me.climate_cargo_slots = msg.ship.climate_cargo_slots;
          state->me.passenger_berths = msg.ship.passenger_berths;
          state->me.passenger_amenities_flags = msg.ship.passenger_amenities_flags;
          state->me.smugglers_hold_cu_m = msg.ship.smugglers_hold_cu_m;
          state->me.remaining_mortgage = msg.ship.remaining_mortgage;
          state->me.interest_rate = msg.ship.interest_rate;
          state->me.cu_m_fuel = msg.ship.cu_m_fuel;
          state->me.cu_m_o2 = msg.ship.cu_m_o2;
          state->me.credits = msg.ship.credits;
          for (u32 i = 0; i < Commodity_Count; i++) {
            state->me.commodities[i] = msg.ship.commodities[i];
          }
        }
      } break;
      case MessageStarPositions: {
        for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
          state->map[i].name = STAR_NAMES[i];
          state->map[i].x = msg.positions[i].x;
          state->map[i].y = msg.positions[i].y;
          for (u32 ii = 0; ii < MAX_PLANETS; ii++) {
            state->map[i].planets[ii].type = msg.planet_types[i*MAX_PLANETS + ii];
          }
        }
      } break;
      case MessageSystemCommodities: {
        u32 sys_idx = msg.id;
        StarSystem* sys = &state->map[sys_idx];
        for (u32 i = 0; i < MAX_PLANETS; i++) {
          for (u32 ii = 0; ii < Commodity_Count; ii++) {
            sys->planets[i].commodities[ii] = msg.sys.planets[i].commodities[ii];
          }
        }
      } break;
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
        state->row.selected_index = 0;
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
  bool user_pressed_tab = input_buffer[0] == ASCII_TAB && input_buffer[1] == 0;
  bool user_pressed_shift_tab = input_buffer[0] == '\x1b' && input_buffer[1] == '[' && input_buffer[2] == 'Z';
  bool user_pressed_esc = input_buffer[0] == ASCII_ESCAPE && input_buffer[1] == 0;
  bool user_pressed_a_number = input_buffer[0] >= '0' && input_buffer[0] <= '9' && input_buffer[1] == 0;
  bool user_pressed_up = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 65;
  bool user_pressed_down = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 66;
  bool user_pressed_left = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 68;
  bool user_pressed_right = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 67;
	bool user_pressed_backspace = input_buffer[0] == ASCII_BACKSPACE || input_buffer[0] == ASCII_DEL;
  bool user_pressed_enter = input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED;
  switch (state->screen) {
    case ScreenTurnTick: {
      u32 line = 2;
      StarSystem dest = state->map[state->destination_sys_idx];
      MemoryZero(sbuf, SBUFLEN);
      sprintf(sbuf, "Warping to %s...", dest.name);
      renderStrToBuffer(tui->frame_buffer, (screen_dimensions.width - (strlen(sbuf)+4))/2, line++, sbuf, screen_dimensions);

      MemoryZero(sbuf, SBUFLEN);
      u32 frames_left = (TURN_TICK_LEN - (loop_count - state->turn_tick_started_on));
      u32 seconds_left = (frames_left / GOAL_LOOPS_PER_S + 1);
      sprintf(sbuf, "in %d", seconds_left);
      renderStrToBuffer(tui->frame_buffer, (screen_dimensions.width - (strlen(sbuf)+4))/2, ++line, sbuf, screen_dimensions);

      if (state->turn_tick_started_on + TURN_TICK_LEN < loop_count) {
        state->screen = ScreenMainGame;
        state->menu.len = Tab_Count;
        state->menu.selected_index = TabMap;
      }
    } break;
    case ScreenCreateCharacter: {
      //// SIMULATION
      if (state->me.id != 0) {
        state->screen = ScreenMainGame;
        state->menu.selected_index = 0;
        state->menu.len = Tab_Count;
        break;
      }
      state->menu.len = ShipType_Count;

      if (input_buffer[0] == 'q' || user_pressed_esc) {
        should_quit = true;
      } else if (user_pressed_down || user_pressed_right) {
        state->menu.selected_index++;
      } else if (user_pressed_up || user_pressed_left) {
        state->menu.selected_index--;
      } else if (user_pressed_a_number) {
        if (input_buffer[0] != '0') {
          state->menu.selected_index = input_buffer[0] - '1';
        }
      } else if (user_pressed_enter) {
        // save ship choice to our gamestate
        ShipTemplate template = SHIPS[state->menu.selected_index];
        state->me.type = template.type;
        state->me.drive_efficiency = template.drive_efficiency;
        state->me.life_support_efficiency = template.life_support_efficiency;
        state->me.vacuum_cargo_slots = template.vacuum_cargo_slots;
        state->me.climate_cargo_slots = template.climate_cargo_slots;
        state->me.passenger_berths = template.passenger_berths;
        state->me.passenger_amenities_flags = template.passenger_amenities_flags;
        state->me.smugglers_hold_cu_m = template.smugglers_hold_cu_m;
        state->me.base_cost = template.base_cost;
        state->me.remaining_mortgage = template.base_cost - STARTING_DOWN_PAYMENT;
        state->me.interest_rate = calcInterestRate(template.base_cost, STARTING_DOWN_PAYMENT);
        // send character ship details to server
        UDPMessage msg = {0};
        msg.address = udp->server_address;
        msg.bytes_len = 2;
        // 1. msg type/CommandType
        msg.bytes[0] = CommandCreateCharacter;
        // 2. chosen ShipType
        msg.bytes[1] = SHIPS[state->menu.selected_index].type;

        outgoingMessageQueuePush(network_send_queue, &msg);

        state->screen = ScreenMainGame;
        state->menu.len = Tab_Count;
        state->menu.selected_index = 0;
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

      // 3. draw the ship choices
      str ship_label = "Choose your starting ship:";
      renderStrToBuffer(tui->frame_buffer, 8, ++line, ship_label, screen_dimensions);
      line++;
      str ship_desc = "You have 10,000 credits to use as a down-payment on a ship.";
      renderStrToBuffer(tui->frame_buffer, 6, ++line, ship_desc, screen_dimensions);
      u32 selected_ship_cost = SHIPS[state->menu.selected_index].base_cost;
      u32 principal_borrowed = selected_ship_cost - STARTING_DOWN_PAYMENT;
      f32 mortgage_rate = calcInterestRate(selected_ship_cost, STARTING_DOWN_PAYMENT);
      f32 r = mortgage_rate / 100.0;
      f32 rm = r / 12.0;
      u32 n = 12 * 4;
      u32 payment = principal_borrowed * (rm*pow((1.0+rm),n))/(pow((1+rm),n) - 1);
      sprintf(sbuf, "Mortgage: $%d @ %f%% = $%d minimum payment per turn", selected_ship_cost - STARTING_DOWN_PAYMENT, mortgage_rate, payment);
      renderStrToBuffer(tui->frame_buffer, 5, ++line, sbuf, screen_dimensions);
      line++;
      // the table
      TableDrawInfo info = {
        .x_offset = 5, .y_offset = line,
        .rows = ShipType_Count, .cols = SHIP_DETAIL_COUNT,
      };
      renderTable(tui, info, state->menu.selected_index, SHIP_FIELDS, 1, SHIPS, sizeof(SHIPS[0]));
    } break;
    case ScreenMainGame: {
      if (user_pressed_tab) {
        state->menu.selected_index += 1;
        if (state->menu.selected_index >= state->menu.len) {
          state->menu.selected_index = 0;
        }
        resetTabRow(state->menu.selected_index);
      } else if (user_pressed_shift_tab) {
        if (state->menu.selected_index > 0) {
          state->menu.selected_index -= 1;
        } else {
          state->menu.selected_index = state->menu.len - 1;
        }
        resetTabRow(state->menu.selected_index);
      }
      Tab tab = state->menu.selected_index;

      // per-tab simulation+rendering
      u32 tabs_y = 1;
      u32 tx = 2;
      for (u32 i = 0; i < Tab_Count; i++) {
        u32 tab_len = strlen(TAB_STRS[i]);
        Box b = { .x = tx, .y = tabs_y, .width = tab_len+1, .height = 1 };
        drawAnsiBox(tui->frame_buffer, b, screen_dimensions, state->menu.selected_index == i);
        renderStrToBuffer(tui->frame_buffer, tx+1, tabs_y+ 1, TAB_STRS[i], screen_dimensions);
        tx += (tab_len + 3);
      }
      // draw the tabs box
      u32 box_y = tabs_y + 3;
      Box box = {
        .x = 1,
        .y = box_y,
        .width = screen_dimensions.width - 4,
        .height = screen_dimensions.height - box_y - 2,
      };
      drawAnsiBox(tui->frame_buffer, box, screen_dimensions, true);
      StarSystem curr = state->map[state->me.system_idx];
      switch (tab) {
        case TabDebug: {
          if (input_buffer[0] == 'q' || user_pressed_esc) {
            should_quit = true;
          }
          renderSystemMessages(tui->frame_buffer, tui->screen_dimensions, box);
        } break;
        case TabChat: {
          renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+1, "You haven't heard anything interesting lately...", screen_dimensions);
        } break;
        case TabMap: {
          if (input_buffer[0] == 'q' || user_pressed_esc) {
            should_quit = true;
          }
          if (user_pressed_up) {
            state->pos.y -= 1;
          } else if (user_pressed_down) {
            state->pos.y += 1;
          } else if (user_pressed_left) {
            state->pos.x -= 1;
          } else if (user_pressed_right) {
            state->pos.x += 1;
          }
          u32 xw = 4;
          u32 yh = 2;
          if (box.width < MAP_WIDTH*xw) {
            renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+2, "Your screen needs to be wider...", screen_dimensions);
            break;
          }
          if (box.height < MAP_HEIGHT*yh) {
            renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+2, "Your screen needs to be taller...", screen_dimensions);
            break;
          }
          u32 x_off = box.x+((box.width - MAP_WIDTH*xw)/2);
          u32 y_off = box.y+1;
          StarSystem dest = state->map[state->destination_sys_idx];
          tui->cursor.x = x_off+state->pos.x*xw;
          tui->cursor.y = y_off+state->pos.y*yh;
          for (u32 i = 0; i < MAP_WIDTH*xw; i++) {
            for (u32 ii = 0; ii < MAP_HEIGHT*yh; ii++) {
              u32 sysx = i / xw;
              u32 sysy = ii / yh;
              u32 bufpos = XYToPos(x_off+i, y_off+ii, screen_dimensions.width);
              tui->frame_buffer[bufpos].bytes[0] = ' ';
              if (state->pos.x == sysx && state->pos.y == sysy) {
                tui->frame_buffer[bufpos].background = ANSI_WHITE;
                tui->frame_buffer[bufpos].foreground = ANSI_BLACK;
              } else if (curr.x == sysx && curr.y == sysy) {
                // highlight the system we are currently at
                tui->frame_buffer[bufpos].background = ANSI_HIGHLIGHT_BLUE;
                tui->frame_buffer[bufpos].foreground = ANSI_BLACK;
              } else if (dest.x == sysx && dest.y == sysy) {
                tui->frame_buffer[bufpos].background = ANSI_HIGHLIGHT_RED;
                tui->frame_buffer[bufpos].foreground = ANSI_BLACK;
              } else {
                tui->frame_buffer[bufpos].foreground = ANSI_WHITE;
                tui->frame_buffer[bufpos].background = ANSI_BLACK;
              }
            }
          }
          for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
            StarSystem sys = state->map[i];
            u32 sysx = x_off + xw*sys.x;
            u32 sysy = y_off + yh*sys.y;
            // clear the render-grid for this "tile" position
            for (u32 ii = 0; ii < xw; ii++) {
              for (u32 iii = 0; iii < yh; iii++) {
                u32 bufpos = XYToPos(sysx+ii, sysy+iii, screen_dimensions.width);
                tui->frame_buffer[bufpos].bytes[0] = 0;
              }
            }
            renderUtf8CodePoint(tui, sysx, sysy, "⭐");
            renderUtf8CodePoint(
              tui,
              sysx +2,
              sysy,
              strForPlanet(sys.planets[0].type)
            );
            renderUtf8CodePoint(
              tui,
              sysx,
              sysy +1,
              strForPlanet(sys.planets[1].type)
            );
            renderUtf8CodePoint(
              tui,
              sysx +2,
              sysy +1,
              strForPlanet(sys.planets[2].type)
            );
          }

          // map key
          y_off += 1+(MAP_HEIGHT*yh);
          Box current_key = { .height = yh, .width = xw, .x = x_off, .y = y_off };
          colorizeBox(tui, current_key, ANSI_HIGHLIGHT_BLUE, 0, ' ');
          renderStrToBuffer(tui->frame_buffer, x_off+5, y_off++, "Your current star system:", screen_dimensions);
          if (curr.name) {
            renderStrToBuffer(tui->frame_buffer, x_off+5, y_off++, curr.name, screen_dimensions);
          }
          y_off++;

          Box flight_dest_key = { .height = yh, .width = xw, .x = x_off, .y = y_off };
          colorizeBox(tui, flight_dest_key, ANSI_HIGHLIGHT_RED, 0, ' ');
          renderStrToBuffer(tui->frame_buffer, x_off+5, y_off++, "Flight Destination:", screen_dimensions);
          if (dest.name) {
            renderStrToBuffer(tui->frame_buffer, x_off+5, y_off++, dest.name, screen_dimensions);
          }
          y_off++;

          Pos2 dest_pos = {dest.x, dest.y};
          Pos2 curr_pos = {curr.x, curr.y};
          u32 projected_fuel_cost = fuelCostForTravel(state->me.drive_efficiency, curr_pos, dest_pos);
          bool have_enough_fuel = projected_fuel_cost < state->me.commodities[CommodityHydrogenFuel];
          MemoryZero(sbuf, SBUFLEN);
          if (have_enough_fuel) {
            sprintf(
              sbuf,
              "Fuel Cost: %dkg / %dkg",
              projected_fuel_cost,
              state->me.commodities[CommodityHydrogenFuel]
            );
          } else {
            sprintf(
              sbuf,
              "Not enough Fuel! Need %dkg more",
              projected_fuel_cost - state->me.commodities[CommodityHydrogenFuel]
            );
            Range1u32 range = {
              .min = XYToPos(x_off, y_off, screen_dimensions.width),
              .max = XYToPos(x_off+strlen(sbuf), y_off, screen_dimensions.width),
            };
            colorizeRange(tui, range, ANSI_HIGHLIGHT_RED, 0);
          }
          renderStrToBuffer(tui->frame_buffer, x_off, y_off++, sbuf, screen_dimensions);

          if (user_pressed_enter) {
            for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
              if (state->pos.x == state->map[i].x && state->pos.y == state->map[i].y) {
                state->destination_sys_idx = i;
                // send the destination to the server
                u32 msg_idx = 0;
                UDPMessage msg = {0};
                msg.address = udp->server_address;
                // 1. msg type/CommandType
                msg.bytes[msg_idx++] = CommandSetDestination;
                // 2. buy?
                msg.bytes[msg_idx++] = state->destination_sys_idx;
                msg.bytes_len = msg_idx;

                outgoingMessageQueuePush(network_send_queue, &msg);
              }
            }
          }
        } break;
        case TabShip: {
          if (input_buffer[0] == 'q' || user_pressed_esc) {
            should_quit = true;
          }

          moveRowUpDown(user_pressed_up, user_pressed_down);

          u32 yoff = box.y+1;
          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Player: %s", state->login_state.name.bytes);
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Ship Type: %s", SHIP_TYPE_STRINGS[state->me.type]);
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Remaining Mortgage: %d @ %.2f%%", state->me.remaining_mortgage, state->me.interest_rate);
          if (state->row.selected_index == 0) {
            Range1u32 range = {
              .min = XYToPos(box.x+2, yoff, screen_dimensions.width),
              .max = XYToPos(box.x+2+strlen(sbuf), yoff, screen_dimensions.width),
            };
            tui->cursor.x = box.x+2;
            tui->cursor.y = yoff;
            colorizeRange(tui, range, ANSI_WHITE, ANSI_BLACK);
            renderStrToBuffer(tui->frame_buffer, box.x+2+strlen(sbuf), yoff, " [ENTER] to pay off", screen_dimensions);

            if (user_pressed_enter && state->ship_tab_states == ShipTabStateMain) {
              // open the payoff modal
              state->ship_tab_states = ShipTabStatePayoffModal;
              user_pressed_enter = false; // to prevent immediately submitting the modal
            }
          }
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Credits: %.2f", state->me.credits);
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          u32 used_cargo = usedVacuumCargoSlots(state->me);
          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Cargo: %d / %d", used_cargo, state->me.vacuum_cargo_slots);
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "Ready To Depart: %s", state->me.ready_to_depart ? "yes" : "no");
          if (state->row.selected_index == 1) {
            Range1u32 range = {
              .min = XYToPos(box.x+2, yoff, screen_dimensions.width),
              .max = XYToPos(box.x+2+strlen(sbuf), yoff, screen_dimensions.width),
            };
            tui->cursor.x = box.x+2;
            tui->cursor.y = yoff;
            colorizeRange(tui, range, ANSI_WHITE, ANSI_BLACK);

            renderStrToBuffer(tui->frame_buffer, box.x+2+strlen(sbuf), yoff, " [ENTER] to toggle", screen_dimensions);

            if (user_pressed_enter && state->ship_tab_states == ShipTabStateMain) {
              // send the "ready" or "not ready" message to the server
              u32 msg_idx = 0;
              UDPMessage msg = {0};
              msg.address = udp->server_address;
              // 1. msg type/CommandType
              msg.bytes[msg_idx++] = CommandReadyStatus;
              // 2. buy?
              msg.bytes[msg_idx++] = !state->me.ready_to_depart;
              msg.bytes_len = msg_idx;

              outgoingMessageQueuePush(network_send_queue, &msg);
            }
          }
          renderStrToBuffer(tui->frame_buffer, box.x+2, yoff++, sbuf, screen_dimensions);

          for (u32 i = 0; i < Commodity_Count; i++) {
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "%-42s %d", COMMODITIES[i].name, state->me.commodities[i]);
            renderStrToBuffer(tui->frame_buffer, box.x+4, yoff+4+i, sbuf, screen_dimensions);
          }

          Box modal_outline = {
            .x = (tui->screen_dimensions.width - 50) / 2,
            .y = (tui->screen_dimensions.height - 15) / 2,
            .height = 15,
            .width = 50,
          };
          u32 y_off = modal_outline.y+2;
          if (state->ship_tab_states == ShipTabStatePayoffModal) {
            if (user_pressed_a_number) {
              String next_char = { 1, 2, (char*)input_buffer };
              stringChunkListAppend(&state->string_arena, &state->message_input, next_char);
            } else if (user_pressed_backspace) {
              stringChunkListDeleteLast(&state->string_arena, &state->message_input);
            }

            // draw the modal
            clearBox(tui, modal_outline);
            drawAnsiBox(tui->frame_buffer, modal_outline, tui->screen_dimensions, false);

            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "How much would you like to pay?");
            renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-strlen(sbuf))/2), y_off++, sbuf, screen_dimensions);

            renderStringChunkList(tui, &state->message_input, modal_outline.x+8, y_off);
            tui->cursor.x = modal_outline.x + 8 + state->message_input.total_size;
            tui->cursor.y = y_off++;

            String input_q_str = stringChunkToString(&permanent_arena, state->message_input);
            u32 input_quantity = atoi(input_q_str.bytes);
            u32 remaining = input_quantity > state->me.remaining_mortgage ? 0 : (state->me.remaining_mortgage - input_quantity);
            arenaDealloc(&permanent_arena, input_q_str.capacity);
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "Leaving $%d remaining to pay off", remaining);
            renderStrToBuffer(tui->frame_buffer, modal_outline.x+8, y_off++, sbuf, screen_dimensions);

            u32 exit_x = modal_outline.x+((modal_outline.width-3)/2);
            u32 pos = XYToPos(exit_x, y_off, tui->screen_dimensions.width);
            for (u32 i = 0; i < 3; i++) {
              tui->frame_buffer[pos+i].background = ANSI_WHITE;
              tui->frame_buffer[pos+i].foreground = ANSI_BLACK;
            }
            renderStrToBuffer(tui->frame_buffer, exit_x, y_off++, "PAY", screen_dimensions);

            if (user_pressed_enter) {
              // send the "payoff" message to server
              u32 msg_idx = 0;
              UDPMessage msg = {0};
              msg.address = udp->server_address;
              // 1. msg type/CommandType
              msg.bytes[msg_idx++] = CommandPayMortgage;
              // 2. buy?
              msg_idx += writeU64ToBufferLE(msg.bytes + msg_idx, input_quantity);
              msg.bytes_len = msg_idx;

              outgoingMessageQueuePush(network_send_queue, &msg);

              state->ship_tab_states = ShipTabStateLoading;
            }
          } else if (state->ship_tab_states == ShipTabStateLoading) {
            // draw the modal
            clearBox(tui, modal_outline);
            drawAnsiBox(tui->frame_buffer, modal_outline, tui->screen_dimensions, false);
            renderStrToBuffer(tui->frame_buffer, modal_outline.x+2, y_off, "Loading...", screen_dimensions);
          }
        } break;
        case TabStation: {
          if (input_buffer[0] == 'q' || user_pressed_esc) {
            switch (state->station_tab_state) {
              case StationTabStateTable:
                should_quit = true;
                break;
              case StationTabStateTransact:
                state->station_tab_state = StationTabStateTable;
                break;
              case StationTabStateResult:
                state->station_tab_state = StationTabStateTransact;
                break;
              case StationTabStateLoading:
                state->station_tab_state = StationTabStateTable;
                break;
              case StationTabStates_Count: assert(false && "something has gone horribly wrong");
            }
          }
          moveRowUpDown(user_pressed_up, user_pressed_down);

          renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+1, "Welcome to ", screen_dimensions);
          renderStrToBuffer(tui->frame_buffer, box.x+2+11, box.y+1, curr.name, screen_dimensions);

          // the table
          TableDrawInfo info = {
            .x_offset = box.x+2, .y_offset = box.y+3,
            .rows = Commodity_Count, .cols = 6,
          };
          MarketCommodity rows[Commodity_Count] = { 0 };
          for (u32 i = 0; i < Commodity_Count; i++) {
            Commodity c = COMMODITIES[i];
            u32 qty = curr.planets[0].commodities[i] + curr.planets[1].commodities[i] + curr.planets[2].commodities[i];
            rows[i].type = c.type;
            rows[i].unit = c.unit;
            rows[i].bid = priceForCommodity(i, qty, true);
            rows[i].ask = priceForCommodity(i, qty, false);
            rows[i].qty = qty;
            rows[i].owned = state->me.commodities[i];
          }
          renderTable(tui, info, state->row.selected_index, MARKET_COMMODITY_FIELDS, 1, rows, sizeof(MarketCommodity));

          if (state->station_tab_state != StationTabStateTable) {
            if (user_pressed_right) {
              state->modal_choice.selected_index = 1;
            } else if (user_pressed_left) {
              state->modal_choice.selected_index = 0;
            } else if (user_pressed_a_number) {
              String next_char = { 1, 2, (char*)input_buffer };
              stringChunkListAppend(&state->string_arena, &state->message_input, next_char);
            } else if (user_pressed_backspace) {
              stringChunkListDeleteLast(&state->string_arena, &state->message_input);
            }

            // draw the modal
            bool buy_selected = state->modal_choice.selected_index == 0;
            Box modal_outline = {
              .x = (tui->screen_dimensions.width - 50) / 2,
              .y = (tui->screen_dimensions.height - 15) / 2,
              .height = 15,
              .width = 50,
            };
            clearBox(tui, modal_outline);
            drawAnsiBox(tui->frame_buffer, modal_outline, tui->screen_dimensions, false);
            u32 y_off = modal_outline.y+2;
            str cname = COMMODITIES[state->row.selected_index].name;
            if (state->station_tab_state == StationTabStateResult) {
              if (state->tx_result.buying) {
                renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-10)/2), y_off++, "Purchased!", screen_dimensions);
              } else {
                renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-5)/2), y_off++, "Sold!", screen_dimensions);
              }
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%d %s", state->tx_result.qty, cname);
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-strlen(sbuf))/2), y_off++, sbuf, screen_dimensions);
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-3)/2), y_off++, "for", screen_dimensions);
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%d credits", state->tx_result.cr);
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-strlen(sbuf))/2), y_off++, sbuf, screen_dimensions);
              y_off++;
              u32 exit_x = modal_outline.x+((modal_outline.width-4)/2);
              u32 pos = XYToPos(exit_x, y_off, tui->screen_dimensions.width);
              for (u32 i = 0; i < 4; i++) {
                tui->frame_buffer[pos+i].background = ANSI_WHITE;
                tui->frame_buffer[pos+i].foreground = ANSI_BLACK;
              }
              tui->cursor.x = exit_x;
              tui->cursor.y = y_off;
              renderStrToBuffer(tui->frame_buffer, exit_x, y_off++, "EXIT", screen_dimensions);

              if (user_pressed_enter) {
                state->station_tab_state = StationTabStateTable;
              }
            } else if (state->station_tab_state == StationTabStateLoading) {
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+2, y_off, "Loading...", screen_dimensions);
            } else { // StationTabStateTransact
              // draw the "purchase order" modal
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+(modal_outline.width/2)-strlen(cname), modal_outline.y+1, cname, screen_dimensions);
              u32 buy_x = modal_outline.x+2;
              u32 sell_x = modal_outline.x+modal_outline.width-6;
              renderStrToBuffer(tui->frame_buffer, buy_x, y_off, "Buy", screen_dimensions);
              renderStrToBuffer(tui->frame_buffer, sell_x, y_off, "Sell", screen_dimensions);
              if (buy_selected) {
                u32 pos = XYToPos(buy_x, y_off, tui->screen_dimensions.width);
                tui->frame_buffer[pos].background = ANSI_WHITE;
                tui->frame_buffer[pos].foreground = ANSI_BLACK;
                tui->frame_buffer[pos+1].background = ANSI_WHITE;
                tui->frame_buffer[pos+1].foreground = ANSI_BLACK;
                tui->frame_buffer[pos+2].background = ANSI_WHITE;
                tui->frame_buffer[pos+2].foreground = ANSI_BLACK;
              } else {
                u32 pos = XYToPos(sell_x, y_off, tui->screen_dimensions.width);
                tui->frame_buffer[pos].background = ANSI_WHITE;
                tui->frame_buffer[pos].foreground = ANSI_BLACK;
                tui->frame_buffer[pos+1].background = ANSI_WHITE;
                tui->frame_buffer[pos+1].foreground = ANSI_BLACK;
                tui->frame_buffer[pos+2].background = ANSI_WHITE;
                tui->frame_buffer[pos+2].foreground = ANSI_BLACK;
                tui->frame_buffer[pos+3].background = ANSI_WHITE;
                tui->frame_buffer[pos+3].foreground = ANSI_BLACK;
              }
              y_off++;
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-8)/2), y_off++, "Quantity", screen_dimensions);
              renderStringChunkList(tui, &state->message_input, modal_outline.x+8, y_off);
              tui->cursor.x = modal_outline.x + 8 + state->message_input.total_size;
              tui->cursor.y = y_off++;
              if (buy_selected) {
                renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-4)/2), y_off++, "Cost", screen_dimensions);
              } else {
                renderStrToBuffer(tui->frame_buffer, modal_outline.x+((modal_outline.width-6)/2), y_off++, "Profit", screen_dimensions);
              }
              String input_q_str = stringChunkToString(&permanent_arena, state->message_input);
              u32 input_quantity = atoi(input_q_str.bytes);
              u32 credits = input_quantity * (buy_selected ? rows[state->row.selected_index].ask : rows[state->row.selected_index].bid);
              arenaDealloc(&permanent_arena, input_q_str.capacity);
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%d", credits);
              renderStrToBuffer(tui->frame_buffer, modal_outline.x+8, y_off++, sbuf, screen_dimensions);

              if (user_pressed_enter) {
                // send the "purchase" or "sell" message to server
                // get back new "status" of things
                u32 msg_idx = 0;
                UDPMessage msg = {0};
                msg.address = udp->server_address;
                // 1. msg type/CommandType
                msg.bytes[msg_idx++] = CommandTransact;
                // 2. buy?
                msg.bytes[msg_idx++] = buy_selected;
                // 3. quantity
                msg_idx += writeU32ToBufferLE(msg.bytes + msg_idx, input_quantity);
                // 4. commodity
                msg.bytes[msg_idx++] = rows[state->row.selected_index].type;
                msg.bytes_len = msg_idx;

                outgoingMessageQueuePush(network_send_queue, &msg);

                state->station_tab_state = StationTabStateLoading;
              }
            }
          } else {
            // we are on the table
            if (user_pressed_enter) {
              state->station_tab_state = StationTabStateTransact;
              state->modal_choice.len = 2;
              state->modal_choice.selected_index = 0;
            }
          }
        } break;
        case Tab_Count: {} break;
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
