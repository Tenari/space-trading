/*
 * Code conventions:
 *  MyStructType
 *  myFunction()
 *  MyMacro()
 *  my_variable
 *  MY_CONSTANT
 * */
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include "shared.h"
#include "base/impl.c"
#define NET_OUTGOING_MESSAGE_QUEUE_LEN 64
#include "lib/network.c"
#include "render.c"
#include "string_chunk.c"

///// CONSTANTS
#define LEFT_ROOM_ENTITES_LEN (KB(1))
#define ROOM_MAP_COLLISIONS_LEN MAX_ROOMS/8
#define CLIENT_COMMAND_LIST_LEN 8
#define SERVER_PORT 7777
#define SERVER_MAX_HEAP_MEMORY MB(256)
#define SERVER_MAX_CLIENTS 16
#define GAME_THREAD_CONCURRENCY 2
#define GOAL_NETWORK_SEND_LOOPS_PER_S 8
#define GOAL_NETWORK_SEND_LOOP_US 1000000/GOAL_NETWORK_SEND_LOOPS_PER_S
#define GOAL_GAME_LOOPS_PER_S 24
#define GOAL_GAME_LOOP_US 1000000/GOAL_GAME_LOOPS_PER_S
#define CLIENT_TIMEOUT_FRAMES GOAL_GAME_LOOPS_PER_S*3
#define CHUNK_SIZE 64
#define ACCOUNT_LEN (16)
#define PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN 64
#define SYSTEM_MESSAGES_LEN 32
#define MAX_SYSTEM_MESSAGE_LEN 512
#define SBUFLEN (512)

///// TypeDefs
typedef enum Tab {
  TabDebug,
  TabMap,
  Tab_Count,
} Tab;

typedef struct ParsedClientCommand {
  CommandType type;
  u8 byte;
  u8 people;
  u8 time_limit;
  u16 sender_port;
  u16 alt_port;
  u32 sender_ip;
  u32 alt_ip;
  u32 qty;
  CommodityType commodity;
  StringChunkList name;
  StringChunkList pass;
  u64 id;
} ParsedClientCommand;

typedef struct ParsedClientCommandThreadQueue {
  ParsedClientCommand items[PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN];
  u32 head;
  u32 tail;
  u32 count;
  Mutex mutex;
  Cond not_empty;
  Cond not_full;
} ParsedClientCommandThreadQueue;

typedef struct Account {
  u8 destination_sys_idx;
  bool changed;
  u32 id; // the index in the array
  String name;
  String pw;
  PlayerShip ship;
} Account;

typedef struct Client {
  u16 lan_port;
  i32 lan_ip;
  u64 account_id;
  SocketAddress address;
  CommandType commands[CLIENT_COMMAND_LIST_LEN];
  u64 last_ping;
} Client;

typedef struct ClientList {
  u64 length;
  u64 capacity;
  Client* items;
} ClientList;

typedef struct State {
  bool all_accounts_ready;
  Tab tab;
  u8 winner_id;
  bool someone_won;
  Mutex client_mutex;
  Mutex mutex;
  ClientList clients;
  u64 next_eid;
  Account accounts[ACCOUNT_LEN];
  u64 frame;
  Arena game_scratch;
  StringArena string_arena;
  ParsedClientCommandThreadQueue* network_recv_queue;
  OutgoingMessageQueue* network_send_queue;
  StarSystem map[STAR_SYSTEM_COUNT];
} State;

///// Global Variables
global State state = { 0 };
global str TAB_STRS[Tab_Count] = {"Debug", "Map"};
global Arena permanent_arena = { 0 };
global bool debug_mode = false;
global bool should_quit = false;
global u8List system_messages[SYSTEM_MESSAGES_LEN] = {0};
global u8 system_message_index = 0;

///// functionImplementations()
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

fn ParsedClientCommandThreadQueue* newPCCThreadQueue(Arena* a) {
  ParsedClientCommandThreadQueue* result = arenaAlloc(a, sizeof(ParsedClientCommandThreadQueue));
  MemoryZero(result, (sizeof *result));
  result->mutex = newMutex();
  result->not_full = newCond();
  result->not_empty = newCond();
  return result;
}

fn void pccThreadSafeQueuePush(ParsedClientCommandThreadQueue* queue, ParsedClientCommand* msg) {
  lockMutex(&queue->mutex); {
    while (queue->count == PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN) {
      waitForCondSignal(&queue->not_full, &queue->mutex);
    }

    MemoryCopy(&queue->items[queue->tail], msg, (sizeof *msg));
    queue->tail = (queue->tail + 1) % PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN;
    queue->count++;

    signalCond(&queue->not_empty);
  } unlockMutex(&queue->mutex);
}

fn ParsedClientCommand* pccThreadSafeNonblockingQueuePop(ParsedClientCommandThreadQueue* q, ParsedClientCommand* copy_target) {
  // immediately returns NULL if there's nothing in the ThreadQueue
  // copies the ParsedClientCommand into `copy_target` if there is something in the queue
  // and marks it as popped from the queue
  ParsedClientCommand* result = NULL;

  lockMutex(&q->mutex); {
    if (q->count > 0) {
      result = &q->items[q->head];
      MemoryCopy(copy_target, result, (sizeof *copy_target));
      q->head = (q->head + 1) % PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN;
      q->count--;

      signalCond(&q->not_full);
    }
  } unlockMutex(&q->mutex);

  return result;
}

fn Account* findAccountByName(String name) {
  for (u32 i = 0; i < ACCOUNT_LEN; i++) {
    if (stringsEq(&state.accounts[i].name, &name)) {
      return &state.accounts[i];
    }
  }
  return NULL;
}

fn void exitWithErrorMessage(ptr msg) {
  printf("error: %s", msg);
  exit(1);
}

fn u32 pushClient(ClientList* clients, SocketAddress addr) {
  Client new_client = {0};
  new_client.last_ping = state.frame;
  new_client.address = addr;

  // first, try to overwrite an old dc'ed client
  for (u32 i = 1; i < clients->length; i++) {
    if (clients->items[i].last_ping == 0) {
      clients->items[i] = new_client;
      return i;
    }
  }

  // if there are none, then add the client to the end of the list
  assert(clients->capacity > clients->length);//TODO grow the list if we need to
  clients->items[clients->length] = new_client;
  u32 result = clients->length;
  clients->length += 1;
  return result;
}

fn bool deleteClientByAccountId(ClientList* clients, u64 id) {
  bool succeeded = false;
  Client blank_client = {0};
  // i=1 because first client is null-client
  for (u32 i = 1; i < clients->length && !succeeded; i++) {
    if (clients->items[i].account_id == id) {
      clients->items[i] = blank_client;
      succeeded = true;
    }
  }
  return succeeded;
}

fn u32 findClientHandleByAccountId(ClientList* clients, u64 id) {
  for (u32 i = 0; i < clients->length; i++) {
    Client c = clients->items[i];
    if (c.account_id == id) {
      return i;
    }
  }
  return 0;
}

fn u32 findClientHandleBySocketAddress(ClientList* clients, SocketAddress address) {
  for (u32 i = 0; i < clients->length; i++) {
    Client c = clients->items[i];
    if (socketAddressEqual(address, c.address)) {
      return i;
    }
  }
  return 0;
}

fn StarSystem* findAccountsSystem(Account* a) {
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    if (i == a->ship.system_idx) {
      return &state.map[i];
    }
  }
  return &state.map[0];
}

fn u32 starSystemPlanetCount(StarSystem* sys) {
  u32 planet_count = 0;
  for (u32 i = 0; i < MAX_PLANETS; i++) {
    if (sys->planets[i].type != PlanetTypeNull) {
      planet_count++;
    }
  }
  return planet_count;
}

fn UDPMessage makeMessageSystemPassengers(StarSystem* sys) {
  UDPMessage outgoing_message = {0};
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessageSystemPassengers;
  outgoing_message.bytes[msg_i++] = (u8)sys->idx;
  for (u32 i = 0; i < MAX_PASSENGER_JOB_OFFERS; i++) {
    if (sys->offers[i].people) {
      outgoing_message.bytes[msg_i++] = sys->offers[i].goal_system_idx;
      outgoing_message.bytes[msg_i++] = sys->offers[i].people;
      outgoing_message.bytes[msg_i++] = sys->offers[i].time_limit;
      msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, sys->offers[i].offer);
    }
  }
  outgoing_message.bytes_len = msg_i;
  return outgoing_message;
}

fn UDPMessage makeMessageSystemCommodities(StarSystem* sys) {
  UDPMessage outgoing_message = {0};
  u32 planet_count = starSystemPlanetCount(sys);
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessageSystemCommodities;
  outgoing_message.bytes[msg_i++] = (u8)sys->idx;
  outgoing_message.bytes[msg_i++] = (u8)planet_count;
  for (u32 i = 0; i < planet_count; i++) {
    for (u32 ii = 0; ii < Commodity_Count; ii++) {
      outgoing_message.bytes[msg_i++] = sys->planets[i].commodities[ii];
    }
  }
  outgoing_message.bytes_len = msg_i;
  return outgoing_message;
}

fn void sendMessageTransactionResult(SocketAddress addr, u32 qty, bool buying, u32 credits) {
  UDPMessage outgoing_message = {.address = addr};
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessageTransactionResult;
  outgoing_message.bytes[msg_i++] = buying;
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, qty);
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, credits);
  outgoing_message.bytes_len = msg_i;
  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
  addSystemMessage((u8*)"Message TransactionResult sent");
}

fn UDPMessage makeMessagePlayerDetails(PlayerShip ship) {
  UDPMessage outgoing_message = {0};
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessagePlayerDetails;
  outgoing_message.bytes[msg_i++] = ship.type;
  outgoing_message.bytes[msg_i++] = ship.system_idx;
  outgoing_message.bytes[msg_i++] = ship.ready_to_depart;
  outgoing_message.bytes[msg_i++] = ship.drive_efficiency;
  outgoing_message.bytes[msg_i++] = ship.life_support_efficiency;
  msg_i += writeU16ToBufferLE(outgoing_message.bytes + msg_i, ship.vacuum_cargo_slots);
  msg_i += writeU16ToBufferLE(outgoing_message.bytes + msg_i, ship.climate_cargo_slots);
  msg_i += writeU16ToBufferLE(outgoing_message.bytes + msg_i, ship.passenger_berths);
  msg_i += writeU16ToBufferLE(outgoing_message.bytes + msg_i, ship.passenger_amenities_flags);
  msg_i += writeU16ToBufferLE(outgoing_message.bytes + msg_i, ship.smugglers_hold_cu_m);
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.remaining_mortgage);
  msg_i += writeF32ToBufferLE(outgoing_message.bytes + msg_i, ship.interest_rate);
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.cu_m_fuel);
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.cu_m_o2);
  msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.credits);
  msg_i += writeU64ToBufferLE(outgoing_message.bytes + msg_i, ship.id);
  for (u32 i = 0; i < Commodity_Count; i++) {
    msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.commodities[i]);
  }
  for (u32 i = 0; i < MAX_PASSENGER_BERTHS; i++) {
    outgoing_message.bytes[msg_i++] = ship.passengers[i].goal_system_idx;
    outgoing_message.bytes[msg_i++] = ship.passengers[i].people;
    outgoing_message.bytes[msg_i++] = ship.passengers[i].turns_remaining;
    msg_i += writeU32ToBufferLE(outgoing_message.bytes + msg_i, ship.passengers[i].reward);
  }
  outgoing_message.bytes_len = msg_i;
  return outgoing_message;
}

fn void sendMessageJobAcceptResult(SocketAddress addr, bool result) {
  UDPMessage outgoing_message = {0};
  outgoing_message.address = addr;
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessageJobAcceptResult;
  outgoing_message.bytes[msg_i++] = result;
  outgoing_message.bytes_len = msg_i;
  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
  addSystemMessage((u8*)"MessageJobAcceptResult sent");
}

fn void sendMessagePayoffResult(SocketAddress addr) {
  UDPMessage outgoing_message = {0};
  outgoing_message.address = addr;
  u32 msg_i = 0;
  outgoing_message.bytes[msg_i++] = (u8)MessagePayoffResult;
  outgoing_message.bytes_len = msg_i;
  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
  addSystemMessage((u8*)"MessagePayoffResult sent");
}

fn void sendMessageStarPositions(SocketAddress addr) {
  UDPMessage outgoing_message = {0};
  outgoing_message.address = addr;
  // tell the client about the map
  u32 star_msg_size = 2+MAX_PLANETS;
  outgoing_message.bytes_len = 1+(star_msg_size*STAR_SYSTEM_COUNT);
  outgoing_message.bytes[0] = (u8)MessageStarPositions;
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    outgoing_message.bytes[1+(i*star_msg_size)] = (u8)state.map[i].x;
    outgoing_message.bytes[2+(i*star_msg_size)] = (u8)state.map[i].y;
    for (u32 ii = 0; ii < MAX_PLANETS; ii++) {
      outgoing_message.bytes[3+ii+(i*star_msg_size)] = state.map[i].planets[ii].type;
    }
  }
  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
  addSystemMessage((u8*)"MessageStarPositions sent");
}

fn void sendMessagePlayerDetails(PlayerShip ship, SocketAddress addr) {
  UDPMessage outgoing_message = makeMessagePlayerDetails(ship);
  outgoing_message.address = addr;
  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
  addSystemMessage((u8*)"MessagePlayerDetails sent");
}

fn void handleIncomingMessage(u8* message, u32 len, SocketAddress sender, i32 socket) {
  dbg("%d: %s from %s:%d\n", len, command_type_strings[message[0]], inet_ntoa(sender.sin_addr), sender.sin_port);
  char sbuf[SBUFLEN] = {0};
  u32 msg_idx = 0;
  ParsedClientCommand parsed = {
    .type = (CommandType)message[msg_idx++],
    .sender_ip = sender.sin_addr.s_addr,
    .sender_port = sender.sin_port,
  };
  u8 temp_bytes[UDP_MAX_MESSAGE_LEN] = {0};
  String temp_str = {
    .bytes = (char*)temp_bytes,
    .length = 0,
    .capacity = 0,
  };
  switch (parsed.type) {
    case CommandLogin: {
      // parse the login command
      parsed.alt_port = ~readU16FromBufferLE(message + msg_idx);
      msg_idx += 2;
      parsed.alt_ip = ~readI32FromBufferLE(message + msg_idx);
      msg_idx += 4;

      // parse the name
      u8 name_len = message[msg_idx++];
      MemoryZero(temp_bytes, UDP_MAX_MESSAGE_LEN);
      temp_str.length = name_len;
      temp_str.capacity = temp_str.length+1;
      for (u32 j = 0; j < temp_str.length; j++) {
        temp_str.bytes[j] = message[msg_idx+j];
      }
      parsed.name = allocStringChunkList(&state.string_arena, temp_str);
      msg_idx += temp_str.length;

      // parse the password
      u32 pw_len = 0;
      while (message[msg_idx+pw_len]) {
        pw_len += 1;
      }
      MemoryZero(temp_bytes, UDP_MAX_MESSAGE_LEN);
      temp_str.length = pw_len;
      temp_str.capacity = temp_str.length+1;
      for (u32 j = 0; j < temp_str.length; j++) {
        temp_str.bytes[j] = message[msg_idx+j];
      }
      parsed.pass = allocStringChunkList(&state.string_arena, temp_str);
      msg_idx += temp_str.length;

      MemoryZero(sbuf, SBUFLEN);
      sprintf(sbuf, "Logging in player: %s %d %s\n", command_type_strings[parsed.type], name_len, message + 7);
      addSystemMessage((u8*)sbuf);
    } break;
    case CommandKeepAlive: break;
    case CommandPayMortgage: {
      parsed.id = readU64FromBufferLE(message + msg_idx);
      MemoryZero(sbuf, SBUFLEN);
      sprintf(sbuf, "paying off %lld", parsed.id);
      addSystemMessage((u8*)sbuf);
    } break;
    case CommandSetDestination: {
      parsed.byte = message[1]; // index of system in array
    } break;
    case CommandReadyStatus: {
      parsed.byte = message[1]; // boolean ready_to_depart
    } break;
    case CommandTransact: {
      addSystemMessage((u8*)"command transact received");
      parsed.byte = message[1]; // buy?
      parsed.qty = readU32FromBufferLE(message + 2);
      parsed.commodity = message[6];
    } break;
    case CommandCreateCharacter: {
      addSystemMessage((u8*)"command create character received");
      parsed.byte = message[1];
    } break;
    case CommandAcceptPassengerJob: {
      addSystemMessage((u8*)"command accept passenger job received\n");
      parsed.byte = message[1];
      parsed.people = message[2];
      parsed.time_limit = message[3];
      parsed.qty = readU32FromBufferLE(message + 4);
    } break;
    case CommandInvalid:
    case CommandType_Count: {
      dbg("invalid command type");
    } break;
  }

  pccThreadSafeQueuePush(state.network_recv_queue, &parsed);
}

fn void* receiveNetworkUpdates(void* udp) {
  UDPServer server = *(UDPServer*)udp;
  dbg("receiveNetworkUpdates() sock=%d\n", server.server_socket);
  infiniteReadUDPServer(&server, handleIncomingMessage);
  return NULL;
}

fn void* sendNetworkUpdates(void* sock) {
  ThreadContext tctx = {0};
  tctxInit(&tctx);
  i32* socket_ptr = (i32*)sock;
  i32 socket = *socket_ptr;
  char sbuf[SBUFLEN] = {0};
  u64 send_loop = 0;
  while (!should_quit) {
    send_loop += 1;
    u64 loop_start = osTimeMicrosecondsNow();

    // 1. clear out our "outgoingMessage" queue
    {
      UDPMessage to_send = { 0 };
      UDPMessage* next_to_send = outgoingMessageNonblockingQueuePop(state.network_send_queue, &to_send);
      u8List bytes_list = { 0 };
      while (next_to_send != NULL) {
        bytes_list.items = to_send.bytes;
        bytes_list.length = to_send.bytes_len;
        bytes_list.capacity = to_send.bytes_len;
        sendUDPu8List(socket, &to_send.address, &bytes_list);
        next_to_send = outgoingMessageNonblockingQueuePop(state.network_send_queue, &to_send);
      }
    }

    UDPMessage sys_udp_msg = { 0 };
    sys_udp_msg = makeMessageSystemCommodities(&state.map[send_loop/2 % STAR_SYSTEM_COUNT]);
    u8List sys_msg = {
      .capacity = UDP_MAX_MESSAGE_LEN,
      .items = sys_udp_msg.bytes,
      .length = sys_udp_msg.bytes_len,
    };
    UDPMessage sys_pass_udp_msg = { 0 };
    sys_pass_udp_msg = makeMessageSystemPassengers(&state.map[send_loop/2 % STAR_SYSTEM_COUNT]);
    u8List sys_pass_msg = {
      .capacity = UDP_MAX_MESSAGE_LEN,
      .items = sys_pass_udp_msg.bytes,
      .length = sys_pass_udp_msg.bytes_len,
    };
    lockMutex(&state.client_mutex); {
      // WARNING the `i` starts at 1 here because state.clients.items[0] is a "null" Client
      for (u32 i = 1; i < state.clients.length; i++) {
        Client client = state.clients.items[i];
        if (client.last_ping+CLIENT_TIMEOUT_FRAMES < state.frame) {
          memset(&state.clients.items[i], 0, sizeof(Client));
          continue;
        }

        if (send_loop % 2 == 0) {
          // every other "send-frame" we send each connnected client the current prices for a different system
          // so that the prices mostly stay up to date pretty quickly without having to track changes
          sendUDPu8List(socket, &client.address, &sys_msg);
          // and the passenger offers
          sendUDPu8List(socket, &client.address, &sys_pass_msg);
        }

        // update all the changed systems
        UDPMessage msg_data;
        for (u32 ii = 0; ii < STAR_SYSTEM_COUNT; ii++) {
          if (state.map[ii].changed == true) {
            // send commodities
            msg_data = makeMessageSystemCommodities(&state.map[ii]);
            u8List msg = {
              .capacity = UDP_MAX_MESSAGE_LEN,
              .items = msg_data.bytes,
              .length = msg_data.bytes_len,
            };
            sendUDPu8List(socket, &client.address, &msg);

            // send passenger jobs
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "sending passenger jobs for %s\n", STAR_NAMES[ii]);
            addSystemMessage((u8*)sbuf);
            msg_data = makeMessageSystemPassengers(&state.map[ii]);
            msg.capacity = UDP_MAX_MESSAGE_LEN;
            msg.items = msg_data.bytes;
            msg.length = msg_data.bytes_len;
            sendUDPu8List(socket, &client.address, &msg);
          }
        }
        // update all the changed accounts
        for (u32 ii = 0; ii < ACCOUNT_LEN; ii++) {
          if (state.accounts[ii].changed == true) {
            msg_data = makeMessagePlayerDetails(state.accounts[ii].ship);
            u8List msg = {
              .capacity = UDP_MAX_MESSAGE_LEN,
              .items = msg_data.bytes,
              .length = msg_data.bytes_len,
            };
            sendUDPu8List(socket, &client.address, &msg);
          }
        }
      }
    } unlockMutex(&state.client_mutex);

    lockMutex(&state.mutex); {
      for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
        state.map[i].changed = false;
      }
      for (u32 i = 0; i < ACCOUNT_LEN; i++) {
        state.accounts[i].changed = false;
      }
    } unlockMutex(&state.mutex);

    u32 loop_duration = osTimeMicrosecondsNow() - loop_start;
    i32 remaining_time = GOAL_NETWORK_SEND_LOOP_US - loop_duration;
    if (remaining_time > 0) {
      osSleepMicroseconds(remaining_time);
    }
  }
  return NULL;
}

fn bool accountIsEmpty(Account* a) {
  return a->id == 0 && a->name.length == 0;
}

fn bool shipIsNull(PlayerShip* ship) {
  return ship->id == 0 && ship->base_cost == 0;
}

fn void* gameLoop(void* params) {
  LaneCtx* lane_ctx = (LaneCtx*)params;
  ThreadContext tctx = {
    .lane_ctx = *lane_ctx,
  };
  tctxInit(&tctx);
  char sbuf[SBUFLEN] = {0};
  sprintf(sbuf, "Lane %lld (%lld) of %lld starting.", lane_ctx->lane_idx, LaneIdx(), lane_ctx->lane_count);
  addSystemMessage((u8*)sbuf);
  UDPMessage outgoing_message = {0};
  u64 loop_start;
  u64 last_burn = 0;
  u64 last_hp_regen = 0;
  Arena scratch_arena = {0};
  arenaInit(&scratch_arena);
  while (!should_quit) {
    loop_start = osTimeMicrosecondsNow();

    if (LaneIdx() == 0) { // narrow
      state.frame += 1;

      if (state.all_accounts_ready) {
        state.all_accounts_ready = false;
      }

      // 1. process client messages
      lockMutex(&state.client_mutex); lockMutex(&state.mutex); {

      u32 msg_iters = 0;
      SocketAddress sender = {0};
      ParsedClientCommand msg = {0};
      ParsedClientCommand* next_net_msg = pccThreadSafeNonblockingQueuePop(state.network_recv_queue, &msg);
      while (next_net_msg != NULL) {
        msg_iters += 0;
        sender.sin_addr.s_addr = msg.sender_ip;
        sender.sin_port = msg.sender_port;
        // find which client it is
        u32 client_handle = findClientHandleBySocketAddress(&state.clients, sender);
        Client* client = &state.clients.items[client_handle];
        switch (msg.type) {
          case CommandAcceptPassengerJob: {
            // move the offer out of the system and INTO the PlayerShip
            Account* account = &state.accounts[client->account_id];
            StarSystem* player_sys = &state.map[account->ship.system_idx];
            PassengerJobOffer pjo = {
              .goal_system_idx = msg.byte,
              .people = msg.people,
              .time_limit = msg.time_limit,
              .offer = msg.qty,
            };
            // find the matching job, and IF the ship has room,
            // "accept" it by clearing it from the system list and adding it to the ship
            bool succeeded = false;
            for (u32 i = 0; i < MAX_PASSENGER_JOB_OFFERS; i++) {
              if (passengerJobEq(player_sys->offers[i], pjo)) {
                if (shipAvailablePassengerBerths(account->ship) > 0) {
                  for (u32 ii = 0; ii < MAX_PASSENGER_BERTHS; ii++) {
                    if (account->ship.passengers[ii].people == 0) {
                      account->ship.passengers[ii].people = pjo.people;
                      account->ship.passengers[ii].goal_system_idx = pjo.goal_system_idx;
                      account->ship.passengers[ii].turns_remaining = pjo.time_limit;
                      account->ship.passengers[ii].reward = pjo.offer;
                      account->changed = true;
                      MemoryZero(&player_sys->offers[i], sizeof(PassengerJobOffer));
                      player_sys->changed = true;
                      succeeded = true;
                      sendMessageJobAcceptResult(sender, succeeded);
                      // end both for loops "break break;"
                      ii = MAX_PASSENGER_BERTHS;
                      i = MAX_PASSENGER_JOB_OFFERS;
                      break;
                    }
                  }
                }
              }
            }
            if (!succeeded) {
              sendMessageJobAcceptResult(sender, succeeded);
            }
          } break;
          case CommandPayMortgage: {
            Account* account = &state.accounts[client->account_id];
            u32 amount_to_pay = msg.id;
            if (amount_to_pay > account->ship.credits) {
              amount_to_pay = account->ship.credits;
            }
            if (amount_to_pay > account->ship.remaining_mortgage) {
              amount_to_pay = account->ship.remaining_mortgage;
            }
            account->ship.credits -= amount_to_pay;
            account->ship.remaining_mortgage -= amount_to_pay;
            account->changed = true;
            sendMessagePayoffResult(sender);
          } break;
          case CommandSetDestination: {
            Account* account = &state.accounts[client->account_id];
            account->destination_sys_idx = msg.byte;
          } break;
          case CommandReadyStatus: {
            Account* account = &state.accounts[client->account_id];
            account->ship.ready_to_depart = msg.byte;
            sendMessagePlayerDetails(account->ship, sender);
            state.all_accounts_ready = true;
            for (u32 i = 0; i < ACCOUNT_LEN; i++) {
              if (!accountIsEmpty(&state.accounts[i])) {
                if (state.accounts[i].ship.ready_to_depart == false) {
                  state.all_accounts_ready = false;
                }
              }
            }
          } break;
          case CommandTransact: {
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "Transact for client_handle=%d, on #%lld", client_handle, state.frame);
            addSystemMessage((u8*)sbuf);
            bool is_buying_from_system = msg.byte;
            Account* account = &state.accounts[client->account_id];
            StarSystem* sys = findAccountsSystem(account);
            u32 total_available = sys->planets[0].commodities[msg.commodity] + sys->planets[1].commodities[msg.commodity] + sys->planets[2].commodities[msg.commodity];
            u32 qty_traded = 0;
            u32 credit_value = 0;
            if (is_buying_from_system) {
              u32 ship_space = account->ship.cu_m_fuel - account->ship.commodities[CommodityHydrogenFuel];
              if (msg.commodity == CommodityOxygen) {
                ship_space = account->ship.cu_m_o2 - account->ship.commodities[CommodityOxygen];
              } else if (COMMODITIES[msg.commodity].unit == StorageUnitContainer) {
                ship_space = account->ship.vacuum_cargo_slots - usedVacuumCargoSlots(account->ship);
              }
              for (u32 amount_to_buy = Min(msg.qty, total_available); amount_to_buy > 0; amount_to_buy--, total_available--, ship_space--) {
                u32 price = priceForCommodity(msg.commodity, total_available, false);
                if (price > account->ship.credits || ship_space == 0) {
                  amount_to_buy = 0;
                  break;
                }
                credit_value += price;
                account->ship.credits -= price;
                account->ship.commodities[msg.commodity] += 1;
                qty_traded += 1;
                if (sys->planets[0].commodities[msg.commodity]) {
                  sys->planets[0].commodities[msg.commodity] -= 1;
                } else if (sys->planets[1].commodities[msg.commodity]) {
                  sys->planets[1].commodities[msg.commodity] -= 1;
                } else if (sys->planets[2].commodities[msg.commodity]) {
                  sys->planets[2].commodities[msg.commodity] -= 1;
                }
              }
            } else {
              // selling to system logic
              qty_traded = Min(msg.qty, account->ship.commodities[msg.commodity]);
              u32 planet_count = starSystemPlanetCount(sys);
              for (
                u32 amount_to_sell = qty_traded;
                amount_to_sell > 0;
                amount_to_sell--, total_available++
              ) {
                u32 price = priceForCommodity(msg.commodity, total_available, true);
                credit_value += price;
                account->ship.credits += price;
                account->ship.commodities[msg.commodity] -= 1;
                u8 planet_idx = rand() % planet_count;
                sys->planets[planet_idx].commodities[msg.commodity] += 1;
              }
            }
            sys->changed = true;
            sendMessagePlayerDetails(account->ship, sender);
            sendMessageTransactionResult(sender, qty_traded, is_buying_from_system, credit_value);
          } break;
          case CommandKeepAlive: {
            dbg("KeepAlive for client_handle=%d, %ld", client_handle, state.frame);
            state.clients.items[client_handle].last_ping = state.frame;
          } break;
          case CommandLogin: {
            if (client_handle == 0) {
              client_handle = pushClient(&state.clients, sender);
              client = &state.clients.items[client_handle];
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "pushed new client handle = %d\n", client_handle);
              addSystemMessage((u8*)sbuf);
            }
            // update/set the lan_ip/port info for p2p connections
            client->lan_ip = htonl(msg.alt_ip);
            client->lan_port = htons(msg.alt_port);

            /*
            struct in_addr ipaddr;
            ipaddr.s_addr = htonl(msg.alt_ip);
            printf("client #%d: SENDER=%s:%d\n", client_handle, inet_ntoa(sender.sin_addr), ntohs(sender.sin_port));
            printf("            LAN=%s:%d   %d vs %d vs %d\n", inet_ntoa(ipaddr), msg.alt_port, msg.alt_ip, htonl(msg.alt_ip), sender.sin_addr.s_addr);
            */

            String name = stringChunkToString(&permanent_arena, msg.name);
            releaseStringChunkList(&state.string_arena, &msg.name);

            String pw = stringChunkToString(&permanent_arena, msg.pass);
            releaseStringChunkList(&state.string_arena, &msg.pass);

            Account* existing_account = findAccountByName(name);
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "name(%d): %s pw(%d): %s acct?: %d\n", name.length, name.bytes, pw.length, pw.bytes, existing_account != NULL);
            addSystemMessage((u8*)sbuf);
            if (existing_account) {
              addSystemMessage((u8*)" existing account\n");
              bool pw_matches = stringsEq(&pw, &existing_account->pw);
              arenaDealloc(&permanent_arena, pw.capacity);
              arenaDealloc(&permanent_arena, name.capacity);
              if (pw_matches) {
                addSystemMessage((u8*)" pw matched\n");
                existing_account->changed = true;
                sendMessageStarPositions(sender);
              } else {
                // tell the client they did a bad pw
                outgoing_message.bytes[0] = (u8)MessageBadPw;
                outgoing_message.bytes_len = 1;
                outgoing_message.address = sender;
                outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
                addSystemMessage((u8*)"MessageBadPw sent\n");
                break;
              }
            } else {
              for (u32 i = 0; i < ACCOUNT_LEN; i++) {
                if (accountIsEmpty(&state.accounts[i])) {
                  existing_account = &state.accounts[i];
                  existing_account->id = i;
                  break;
                }
              }
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "new account id=%d\n", existing_account->id);
              addSystemMessage((u8*)sbuf);
              existing_account->name = name;
              existing_account->pw = pw;
            }
            client->account_id = existing_account->id;
            if (!shipIsNull(&existing_account->ship)) {
              // tell the client their account id
              outgoing_message.bytes[0] = (u8)MessageCharacterId;
              writeU64ToBufferLE(outgoing_message.bytes + 1, existing_account->ship.id);
              outgoing_message.bytes_len = 9;
              outgoing_message.address = sender;
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              addSystemMessage((u8*)"MessageCharacterId sent\n");
            } else {
              // tell the client they made a new account
              outgoing_message.bytes[0] = (u8)MessageNewAccountCreated;
              outgoing_message.bytes_len = 1;
              outgoing_message.address = sender;
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              addSystemMessage((u8*)"MessageNewAccountCreated sent\n");
            }
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "client_handle=%d, acct_id=%d\n", client_handle, existing_account->id);
            addSystemMessage((u8*)sbuf);
          } break;
          case CommandCreateCharacter: {
            Account* account = &state.accounts[client->account_id];
            MemoryZero(sbuf, SBUFLEN);
            sprintf(sbuf, "creating character for account id=%d, %s\n", account->id, SHIP_TYPE_STRINGS[msg.byte]);
            addSystemMessage((u8*)sbuf);
            if (shipIsNull(&account->ship)) {
              ShipTemplate template = SHIPS[msg.byte];
              PlayerShip player_ship = {
                .type = msg.byte,
                .drive_efficiency = template.drive_efficiency,
                .life_support_efficiency = template.life_support_efficiency,
                .vacuum_cargo_slots = template.vacuum_cargo_slots,
                .climate_cargo_slots = template.climate_cargo_slots,
                .passenger_berths = template.passenger_berths,
                .passenger_amenities_flags = template.passenger_amenities_flags,
                .smugglers_hold_cu_m = template.smugglers_hold_cu_m,
                .base_cost = template.base_cost,
                .remaining_mortgage = template.base_cost - STARTING_DOWN_PAYMENT,
                .interest_rate = calcInterestRate(template.base_cost, STARTING_DOWN_PAYMENT),
                .credits = 10000.0,
                .cu_m_fuel = template.cu_m_fuel,
                .cu_m_o2 = template.cu_m_o2,
                .id = account->id,
              };
              player_ship.commodities[CommodityHydrogenFuel] = player_ship.cu_m_fuel;
              player_ship.commodities[CommodityOxygen] = player_ship.cu_m_o2;
              account->ship = player_ship;

              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "ship_type=%s, client_handle=%d, acct_id=%d\n", SHIP_TYPE_STRINGS[msg.byte], client_handle, account->id);
              addSystemMessage((u8*)sbuf);

              u32 starting_system_idx = rand() % STAR_SYSTEM_COUNT;
              StarSystem starting_system = state.map[starting_system_idx];
              account->ship.system_idx = starting_system_idx;

              // tell the client their account id
              outgoing_message.address = sender;
              outgoing_message.bytes_len = 9;
              outgoing_message.bytes[0] = (u8)MessageCharacterId;
              writeU64ToBufferLE(outgoing_message.bytes + 1, account->ship.id);
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              addSystemMessage((u8*)"MessageCharacterId sent\n");

              sendMessagePlayerDetails(account->ship, sender);

              sendMessageStarPositions(sender);

              outgoing_message = makeMessageSystemCommodities(&starting_system);
              outgoing_message.address = sender;
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%s sent\n", MESSAGE_STRINGS[outgoing_message.bytes[0]]);
              addSystemMessage((u8*)sbuf);
            } else {
              addSystemMessage((u8*)"client tried to create a character when he already has one.");
            }
          } break;
          case CommandType_Count:
          case CommandInvalid:
            dbg("invalid message from queue");
            break;
        }
        next_net_msg = pccThreadSafeNonblockingQueuePop(state.network_recv_queue, &msg);
        msg_iters++;
      }

      if (state.all_accounts_ready) {
        for (u32 i = 1; i < SERVER_MAX_CLIENTS; i++) {
          if (state.clients.items[i].last_ping != 0) {
            outgoing_message.address = state.clients.items[i].address;
            outgoing_message.bytes_len = 1;
            outgoing_message.bytes[0] = (u8)MessageTurnTick;
            outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
          }
        }
      }

      if (state.someone_won) {
        for (u32 i = 1; i < SERVER_MAX_CLIENTS; i++) {
          if (state.clients.items[i].last_ping != 0) {
            outgoing_message.address = state.clients.items[i].address;
            outgoing_message.bytes_len = 2;
            outgoing_message.bytes[0] = (u8)MessageGameOver;
            outgoing_message.bytes[1] = state.winner_id;
            outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
          }
        }
      }

      } unlockMutex(&state.mutex); unlockMutex(&state.client_mutex);
    }

    if (state.someone_won) {
      return NULL;
    }

    LaneSync();

    u32 player_count = 0;
    for (u32 i = 0; i < ACCOUNT_LEN; i++) {
      if (!accountIsEmpty(&state.accounts[i])) {
        player_count += 1;
      }
    }

    // 2. tick non-user entities
    if (state.all_accounts_ready) {
      addSystemMessage((u8*)"NEW TURN: ticking all_accounts_ready");
      // tick all the star systems
      StarSystem* sys = NULL;
      Range1u64 sys_range = LaneRange(STAR_SYSTEM_COUNT);
      for (u32 i = sys_range.min; i < sys_range.max; i++) {
        sys = &state.map[i];
        sys->changed = true;
        // consume and produce commodities
        for (u32 ii = 0; ii < Commodity_Count; ii++) {
          for (u32 iii = 0; iii < MAX_PLANETS; iii++) {
            if (sys->planets[iii].type != PlanetTypeNull) {
              if (sys->planets[iii].commodities[ii] > COMMODITIES[ii].consumption) {
                sys->planets[iii].commodities[ii] -= COMMODITIES[ii].consumption;
              } else {
                sys->planets[iii].commodities[ii] = 0;
              }
              sys->planets[iii].commodities[ii] += sys->planets[iii].production[ii];
            }
          }
        }
        // potentially create a new passenger job
        u32 planet_divisor = ((MAX_PLANETS - starSystemPlanetCount(sys))+1);
        u32 max_passenger_job_count = 1 + ((MAX_PASSENGER_JOB_OFFERS-1) / planet_divisor);
        for (u32 ii = 0; ii < max_passenger_job_count; ii++) {
          if (sys->offers[ii].people == 0) {
            // TODO choose a system further away
            sys->offers[ii].goal_system_idx = (i+1+(rand() % 6)) % STAR_SYSTEM_COUNT;
            sys->offers[ii].people = 1 + (rand() % (MAX_PASSENGER_JOB_PEOPLE - 1));
            sys->offers[ii].time_limit = 3 + (rand() % 5);
            // TODO compute based on distance between people and time_limit
            sys->offers[ii].offer = 1000 + (rand() % MAX_PASSENGER_JOB_PRICE);
            break;
          } else { //increase the offer amount each turn
            sys->offers[ii].offer = (u32)((f32)sys->offers[ii].offer * 1.05);
          }
        }
      }

      Range1u64 ship_range = LaneRange(player_count);
      for (u32 i = ship_range.min; i < ship_range.max; i++) {
        Account* acct = &state.accounts[i];

        // move all the players
        StarSystem dest = state.map[acct->destination_sys_idx];
        StarSystem current = state.map[acct->ship.system_idx];
        Pos2 dest_pos = {dest.x, dest.y};
        Pos2 curr_pos = {current.x, current.y};
        u32 projected_fuel_cost = fuelCostForTravel(acct->ship.drive_efficiency, curr_pos, dest_pos);
        bool have_enough_fuel = projected_fuel_cost <= acct->ship.commodities[CommodityHydrogenFuel];
        u32 projected_oxygen_cost = oxyCostForTravel(&acct->ship, curr_pos, dest_pos);
        bool have_enough_oxy = projected_oxygen_cost <= acct->ship.commodities[CommodityOxygen];
        if (have_enough_fuel || have_enough_oxy) {
          // remove the fuel they used on the journey
          acct->ship.commodities[CommodityHydrogenFuel] -= projected_fuel_cost;
          // remove the oxygen they used on the journey
          acct->ship.commodities[CommodityOxygen] -= projected_oxygen_cost;

          // set the pos to be the system they had as destination
          acct->ship.system_idx = acct->destination_sys_idx;
        }
        acct->ship.ready_to_depart = false;
        acct->changed = true;

        // increase the mortgage from interest
        if (!shipIsNull(&acct->ship)) {
          if (acct->ship.remaining_mortgage > 0) {
            f64 compounding = ((f64)acct->ship.remaining_mortgage) * (f64)(acct->ship.interest_rate / 100.0);
            acct->ship.remaining_mortgage += (u32)compounding;
          }
        }

        // check for the completion of a passenger job (MUST COME AFTER acct->ship.system_idx is updated)
        for (u32 ii = 0; ii < MAX_PASSENGER_BERTHS; ii++) {
          Passenger* job = &acct->ship.passengers[ii];
          bool is_valid_job = job->people > 0;
          bool is_at_job_destination = job->goal_system_idx == acct->ship.system_idx;
          if (is_valid_job) {
            if (is_at_job_destination) {
              acct->ship.credits += job->reward;
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%d credits awarded to %s for finishing passenger job\n", job->reward, acct->name.bytes);
              addSystemMessage((u8*)sbuf);
              MemoryZero(job, sizeof(Passenger));
              MemoryZero(sbuf, SBUFLEN);
              sprintf(sbuf, "%s FINISHED a passenger job!\n", acct->name.bytes);
              addSystemMessage((u8*)sbuf);
              // send a job completion message?
              for (u32 i = 1; i < state.clients.length; i++) {
                if (state.clients.items[i].last_ping != 0 && state.clients.items[i].account_id == acct->id) {
                  outgoing_message.address = state.clients.items[i].address;
                  outgoing_message.bytes_len = 2;
                  outgoing_message.bytes[0] = (u8)MessageJobComplete;
                  outgoing_message.bytes[1] = true;
                  outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
                  MemoryZero(sbuf, SBUFLEN);
                  sprintf(sbuf, "sent MessageJobComplete to %s\n", acct->name.bytes);
                  addSystemMessage((u8*)sbuf);
                  break;
                }
              }
            } else {
              job->turns_remaining -= 1;
              if (job->turns_remaining == 0) {
                // they failed the job
                MemoryZero(job, sizeof(Passenger));
                MemoryZero(sbuf, SBUFLEN);
                sprintf(sbuf, "%s failed a passenger job\n", acct->name.bytes);
                addSystemMessage((u8*)sbuf);
                // send a job failed message?
                for (u32 i = 1; i < state.clients.length; i++) {
                  if (state.clients.items[i].last_ping != 0 && state.clients.items[i].account_id == acct->id) {
                    outgoing_message.address = state.clients.items[i].address;
                    outgoing_message.bytes_len = 2;
                    outgoing_message.bytes[0] = (u8)MessageJobComplete;
                    outgoing_message.bytes[1] = false;
                    outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
                    MemoryZero(sbuf, SBUFLEN);
                    sprintf(sbuf, "sent MessageJobComplete to %s\n", acct->name.bytes);
                    addSystemMessage((u8*)sbuf);
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }

    // look for a winner
    Range1u64 ship_range = LaneRange(player_count);
    for (u32 i = ship_range.min; i < ship_range.max; i++) {
      Account* acct = &state.accounts[i];
      if (!shipIsNull(&acct->ship)) {
        if (acct->ship.remaining_mortgage == 0) {
          state.winner_id = acct->id;
          state.someone_won = true;
          MemoryZero(sbuf, SBUFLEN);
          sprintf(sbuf, "%s WON!!! %d\n", acct->name.bytes, i);
          addSystemMessage((u8*)sbuf);
          acct->changed = true;
          break;
        }
      }
    }

    // 3. scratch cleanup
    arenaClear(&scratch_arena);

    // 4. loop timing
    u32 loop_duration = osTimeMicrosecondsNow() - loop_start;
    i32 remaining_time = GOAL_GAME_LOOP_US - loop_duration;
    if (remaining_time > 0) {
      osSleepMicroseconds(remaining_time);
    }
  }
  return NULL;
}

fn void setHighProductionCommodity(Planet* p, CommodityType t) {
  p->commodities[t] = (COMMODITIES[t].qty / 2) + (rand() % COMMODITIES[t].qty);
  p->production[t] = COMMODITIES[t].consumption + (rand() % COMMODITIES[t].consumption);
}

fn void setLowProductionCommodity(Planet* p, CommodityType t) {
  p->commodities[t] = rand() % (COMMODITIES[t].qty / 2);
  p->production[t] = rand() % (COMMODITIES[t].consumption / 2);
}

fn bool updateAndRender(TuiState* tui, void* s, u8* input_buffer, u64 loop_count) {
  State* state = (State*) s;
  Dim2 screen_dimensions = tui->screen_dimensions;
  if (screen_dimensions.height > MAX_SCREEN_HEIGHT || screen_dimensions.width > MAX_SCREEN_WIDTH) {
    printf("\033[2J\033[%d;%df Your screen is too damn big. Shrink it to %dx%d max, bro... geez", 1,1, MAX_SCREEN_WIDTH, MAX_SCREEN_HEIGHT);
    fflush(stdout);
    return should_quit;
  }
  char sbuf[SBUFLEN] = {0};
  
  // operate on screen-based input+state
  bool user_pressed_tab = input_buffer[0] == ASCII_TAB && input_buffer[1] == 0;
  bool user_pressed_shift_tab = input_buffer[0] == '\x1b' && input_buffer[1] == '[' && input_buffer[2] == 'Z';
  bool user_pressed_space = input_buffer[0] == ' ' && input_buffer[1] == 0;
  bool user_pressed_esc = input_buffer[0] == ASCII_ESCAPE && input_buffer[1] == 0;
  bool user_pressed_a_number = input_buffer[0] >= '0' && input_buffer[0] <= '9' && input_buffer[1] == 0;
  bool user_pressed_up = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 65;
  bool user_pressed_down = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 66;
  bool user_pressed_left = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 68;
  bool user_pressed_right = input_buffer[0] == 27 && input_buffer[1] == 91 && input_buffer[2] == 67;
  bool user_pressed_backspace = input_buffer[0] == ASCII_BACKSPACE || input_buffer[0] == ASCII_DEL;
  bool user_pressed_enter = input_buffer[0] == ASCII_RETURN || input_buffer[0] == ASCII_LINE_FEED;

  if (user_pressed_tab || user_pressed_shift_tab) {
    state->tab = state->tab == TabDebug ? TabMap : TabDebug;
  }

  // draw the tabs box
  u32 tabs_y = 1;
  u32 box_y = tabs_y + 2;
  Box box = {
    .x = 1,
    .y = box_y,
    .width = screen_dimensions.width - 3,
    .height = screen_dimensions.height - box_y - 2,
  };
  drawAnsiBox(tui->frame_buffer, box, screen_dimensions, true);
  u32 tx = 2;
  // draw the actual tabs
  for (u32 i = 0; i < Tab_Count; i++) {
    u32 tab_len = strlen(TAB_STRS[i]);
    Box b = { .x = tx, .y = tabs_y, .width = tab_len+1, .height = 1 };
    drawAnsiBox(tui->frame_buffer, b, screen_dimensions, state->tab == i);
    renderStrToBuffer(tui->frame_buffer, tx+1, tabs_y+ 1, TAB_STRS[i], screen_dimensions);
    tx += (tab_len + 3);
  }

  switch (state->tab) {
    case TabDebug: {
      if (input_buffer[0] == 'q' || user_pressed_esc) {
        should_quit = true;
      }
      renderSystemMessages(tui->frame_buffer, tui->screen_dimensions, box);
    } break;
    case TabMap: {
      u8 COLORS[] = {ANSI_HIGHLIGHT_RED, ANSI_HIGHLIGHT_BLUE, ANSI_HIGHLIGHT_YELLOW, ANSI_HIGHLIGHT_GREEN, ANSI_HIGHLIGHT_GRAY};
      u32 xw = 4;
      u32 yh = 2;
      if (box.width <= MAP_WIDTH*xw) {
        renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+2, "Your screen needs to be wider...", screen_dimensions);
        break;
      }
      if (box.height < MAP_HEIGHT*yh) {
        renderStrToBuffer(tui->frame_buffer, box.x+2, box.y+2, "Your screen needs to be taller...", screen_dimensions);
        break;
      }

      // render the damn map
      u32 x_off = box.x+((box.width - MAP_WIDTH*xw)/2);
      u32 y_off = box.y+1;
      for (u32 i = 0; i < MAP_WIDTH*xw; i++) {
        for (u32 ii = 0; ii < MAP_HEIGHT*yh; ii++) {
          u32 sysx = i / xw;
          u32 sysy = ii / yh;
          u32 bufpos = XYToPos(x_off+i, y_off+ii, screen_dimensions.width);
          tui->frame_buffer[bufpos].bytes[0] = ' ';
          tui->frame_buffer[bufpos].foreground = ANSI_WHITE;
          tui->frame_buffer[bufpos].background = ANSI_BLACK;
          // try to colorize the map cell if a player is on it
          for (u32 iii = 0; iii < ACCOUNT_LEN; iii++) {
            Account* acct = &state->accounts[iii];
            if (acct->name.length > 0) {
              StarSystem sys = state->map[acct->ship.system_idx];
              if (sys.x == sysx && sys.y == sysy) {
                Box current_key = { .height = yh, .width = xw, .x = x_off+i, .y = y_off+ii };
                colorizeBox(tui, current_key, COLORS[iii], 0, ' ');
              }
            }
          }
          //if (state->pos.x == sysx && state->pos.y == sysy) {
          //  tui->frame_buffer[bufpos].background = ANSI_WHITE;
          //  tui->frame_buffer[bufpos].foreground = ANSI_BLACK;
          //} else {
          //}
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
      for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
        StarSystem sys = state->map[i];
        u32 sysx = x_off + xw*sys.x;
        u32 sysy = y_off + yh*sys.y;
        if (sysx == tui->cursor.x && sysy == tui->cursor.y) {
          renderStrToBuffer(tui->frame_buffer, sysx, sysy+2, STAR_NAMES[i], screen_dimensions);
        }
      }
      // TODO fix the background bleed from previous line

      // map key
      y_off += 1+(MAP_HEIGHT*yh);
      u32 original_x_off = x_off;
      for (u32 i = 0; i < ACCOUNT_LEN; i++) {
        Account* acct = &state->accounts[i];
        if (acct->name.length > 0) {
          Box current_key = { .height = yh, .width = xw, .x = x_off, .y = y_off };
          colorizeBox(tui, current_key, COLORS[i], 0, ' ');
          renderStrToBuffer(tui->frame_buffer, x_off+5, y_off, acct->name.bytes, screen_dimensions);

          if (i % 2 == 0) {
            x_off += 20;
          } else if (i % 2 == 1) {
            y_off += 3;
            x_off = original_x_off;
          }
        }
      }

    } break;
    case Tab_Count: {} break;
  }
  
  return should_quit;
}


// THE SERVER
i32 main(i32 argc, ptr argv[]) {
  osInit();
  // multi-thread architecture:
  //  - the gameLoop() which just inifinite loops every "tick" and processes user input and updates gameworld state
  //    - TODO: actually make this N "lanes" as described https://www.rfleury.com/p/multi-core-by-default
  //      such that we can split room-work among all the various cores on our machine
  //  - one is the sendNetworkUpdates() infinite loop, which sends a UDP snapshot-or-delta update to each connected client N/sec
  //  - one is the receiveNetworkUpdates() infinte loop, which waits for new UDP messages from clients

  // 1. initialize gameworld, and spin off infinite game-loop thread
  // 2. spin off sendNetworkUpdates() infinite loop thread
  // 3. infinitely wait for incoming UDP messages and process them (usually by just dropping user-commands into the relevant block of shared memory)

  // 1. initialize gameworld, and spin off infinite game-loop thread
  arenaInit(&permanent_arena);
  arenaInit(&state.game_scratch);
  arenaInit(&state.string_arena.a);
  state.string_arena.mutex = newMutex();
  state.client_mutex = newMutex();
  state.mutex = newMutex();
  state.network_recv_queue = newPCCThreadQueue(&permanent_arena);
  state.network_send_queue = newOutgoingMessageQueue(&permanent_arena);
  // alloc the global hashmap of rooms
  // init + alloc clients
  state.clients.capacity = SERVER_MAX_CLIENTS;
  state.clients.length = 1; // making entry 0 to be a "null" client 
  state.clients.items = (Client*)arenaAllocArray(&permanent_arena, Client, SERVER_MAX_CLIENTS);
  for (i32 i = 0; i < SYSTEM_MESSAGES_LEN; i++) {
    system_messages[i].capacity = MAX_SYSTEM_MESSAGE_LEN;
    system_messages[i].length = 0;
    system_messages[i].items = arenaAllocArraySized(&permanent_arena, sizeof(u8), MAX_SYSTEM_MESSAGE_LEN);
  }

  // GAME LOGIC SETUP

  // set position of all star systems
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    state.map[i].idx = i;
    state.map[i].name = STAR_NAMES[i];
    state.map[i].crime = rand() % 100;
    state.map[i].x = rand() % MAP_WIDTH;
    state.map[i].y = rand() % MAP_HEIGHT;
    // ensure all the star systems are "spaced out" a bit
    bool conflicting_pos = false;
    for (u32 ii = 0; ii < i; ii++) {
      bool same_pos = state.map[ii].x == state.map[i].x && state.map[ii].y == state.map[i].y;
      bool north_pos = state.map[ii].x == state.map[i].x && state.map[ii].y == state.map[i].y+1;
      bool south_pos = state.map[ii].x == state.map[i].x && state.map[ii].y+1 == state.map[i].y;
      bool west_pos = state.map[ii].x == state.map[i].x+1 && state.map[ii].y == state.map[i].y;
      bool east_pos = state.map[ii].x+1 == state.map[i].x && state.map[ii].y == state.map[i].y;
      if (same_pos || north_pos || south_pos || west_pos || east_pos) {
        conflicting_pos = true;
      } 
    }
    if (conflicting_pos) {
      i--; // re-do the position calc
    }
  }

  // now, build out the commodity info and passenger jobs for the systems
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    u32 planet_count = rand() % MAX_PLANETS + 1;
    for (u32 ii = 0; ii < planet_count; ii++) {
      Planet* p = &state.map[i].planets[ii];
      p->type = 1+(PlanetType)(rand() % (PlanetType_Count -1));
      // default commodity + production roll
      for (u32 iii = 0; iii < Commodity_Count; iii++) {
        p->commodities[iii] = (COMMODITIES[iii].qty / 5) + (rand() % COMMODITIES[iii].qty);
        p->production[iii] = 1 + (rand() % COMMODITIES[iii].consumption);
      }
      // now, override for the "specialty" of the planet
      switch (p->type) {
        case PlanetTypeEarth: {
          setHighProductionCommodity(p, CommodityWater);
          setHighProductionCommodity(p, CommodityFertilizer);
          setHighProductionCommodity(p, CommodityGrain);
          setHighProductionCommodity(p, CommodityMeat);
          setHighProductionCommodity(p, CommoditySpices);
          setHighProductionCommodity(p, CommodityAlcohol);

          setLowProductionCommodity(p, CommodityHydrogenFuel);
          setLowProductionCommodity(p, CommodityLowGradeOre);
          setLowProductionCommodity(p, CommodityHighGradeOre);
          setLowProductionCommodity(p, CommodityCommonMetals);
          setLowProductionCommodity(p, CommodityRareMetals);
          //setLowProductionCommodity(p, CommodityPreciousMetals);
        } break;
        case PlanetTypeGas: {
          // a lot of fuel and chemicals
          setHighProductionCommodity(p, CommodityHydrogenFuel);
          setHighProductionCommodity(p, CommodityOxygen);
          setHighProductionCommodity(p, CommodityWater);
          setHighProductionCommodity(p, CommodityIndustrialChemicals);
          setHighProductionCommodity(p, CommodityPlastics);

          setLowProductionCommodity(p, CommodityLowGradeOre);
          setLowProductionCommodity(p, CommodityHighGradeOre);
          setLowProductionCommodity(p, CommodityCommonMetals);
          setLowProductionCommodity(p, CommodityRareMetals);
          //setLowProductionCommodity(p, CommodityPreciousMetals);
          setLowProductionCommodity(p, CommodityClothes);
          setLowProductionCommodity(p, CommodityRawTextiles);
          setLowProductionCommodity(p, CommodityGlass);
          setLowProductionCommodity(p, CommodityPersonalSundries);
          setLowProductionCommodity(p, CommodityHandTools);
          setLowProductionCommodity(p, CommodityMeat);
        } break;
        case PlanetTypeMoon: {
          // a lot of manufactured goods
          setHighProductionCommodity(p, CommodityPersonalSundries);
          setHighProductionCommodity(p, CommodityClothes);
          setHighProductionCommodity(p, CommodityAlcohol);
          setHighProductionCommodity(p, CommodityElectronics);

          setLowProductionCommodity(p, CommodityHighGradeOre);
          setLowProductionCommodity(p, CommodityHydrogenFuel);
          setLowProductionCommodity(p, CommodityOxygen);
          setLowProductionCommodity(p, CommodityWater);
          setLowProductionCommodity(p, CommodityFertilizer);
        } break;
        case PlanetTypeAsteroid: {
          // a lot of raw metals
          setHighProductionCommodity(p, CommodityLowGradeOre);
          setHighProductionCommodity(p, CommodityHighGradeOre);
          setHighProductionCommodity(p, CommodityCommonMetals);
          setHighProductionCommodity(p, CommodityRareMetals);
          //setHighProductionCommodity(p, CommodityPreciousMetals);
          setHighProductionCommodity(p, CommoditySemiConductors);

          setLowProductionCommodity(p, CommodityHydrogenFuel);
          setLowProductionCommodity(p, CommodityOxygen);
          setLowProductionCommodity(p, CommodityWater);
          setLowProductionCommodity(p, CommodityFertilizer);
          setLowProductionCommodity(p, CommodityPersonalSundries);
          setLowProductionCommodity(p, CommodityClothes);
          setLowProductionCommodity(p, CommodityElectronics);
          setLowProductionCommodity(p, CommodityPlastics);
          setLowProductionCommodity(p, CommodityHandTools);
        } break;
        case PlanetTypeStation: {
          // a lot of ??? drugs?
          setHighProductionCommodity(p, CommodityAlcohol);
          setHighProductionCommodity(p, CommodityPersonalSundries);
          setHighProductionCommodity(p, CommodityClothes);
          setHighProductionCommodity(p, CommodityElectronics);
          setHighProductionCommodity(p, CommodityPlastics);
          setHighProductionCommodity(p, CommodityHandTools);

          setLowProductionCommodity(p, CommodityHydrogenFuel);
          setLowProductionCommodity(p, CommodityOxygen);
          setLowProductionCommodity(p, CommodityWater);
          setLowProductionCommodity(p, CommodityFertilizer);
          setLowProductionCommodity(p, CommodityMeat);
          setLowProductionCommodity(p, CommodityGrain);
          setLowProductionCommodity(p, CommoditySpices);
        } break;
        case PlanetTypeNull:
        case PlanetType_Count:
          break;
      }
    }

    u32 planet_divisor = ((MAX_PLANETS - planet_count)+1);
    u32 offer_count = 1 + (rand() % (MAX_PASSENGER_JOB_OFFERS / planet_divisor));
    for (u32 ii = 0; ii < offer_count; ii++) {
      // TODO choose a system further away
      state.map[i].offers[ii].goal_system_idx = (i+1+(rand() % 6)) % STAR_SYSTEM_COUNT;
      state.map[i].offers[ii].people = 1 + (rand() % (MAX_PASSENGER_JOB_PEOPLE - 1));
      state.map[i].offers[ii].time_limit = 1 + (rand() % 3);
      // TODO compute based on distance between people and time_limit
      state.map[i].offers[ii].offer = 1000 + (rand() % MAX_PASSENGER_JOB_PRICE);
    }
  }

  // 2. spin off sendNetworkUpdates() infinite loop thread
  UDPServer listener = createUDPServer(SERVER_PORT);
  if (!listener.ready) {
    exitWithErrorMessage("Couldn't start the udp server");
  }
  Thread send_thread = spawnThread(&sendNetworkUpdates, &listener.server_socket);
  // 3. infinitely wait for incoming UDP messages and process them (usually by just dropping user-commands into the relevant block of shared memory)
  Thread recv_thread = spawnThread(&receiveNetworkUpdates, &listener);

  u64 lane_broadcast_val = 0;
  Barrier barrier = osBarrierAlloc(GAME_THREAD_CONCURRENCY);
  LaneCtx lane_ctxs[GAME_THREAD_CONCURRENCY] = {0};
  Thread game_threads[GAME_THREAD_CONCURRENCY] = {0};
  for (u32 i = 0; i < GAME_THREAD_CONCURRENCY; i++) {
    lane_ctxs[i].lane_idx = i;
    lane_ctxs[i].lane_count = GAME_THREAD_CONCURRENCY;
    lane_ctxs[i].barrier = barrier;
    lane_ctxs[i].broadcast_memory = &lane_broadcast_val;
    game_threads[i] = spawnThread(&gameLoop, &lane_ctxs[i]);
  }
  infiniteUILoop(
    MAX_SCREEN_WIDTH,
    MAX_SCREEN_HEIGHT,
    GOAL_GAME_LOOP_US,
    &state,
    updateAndRender
  );
  for (u32 i = 0; i < GAME_THREAD_CONCURRENCY; i++) {
    osThreadJoin(game_threads[i], MAX_u64);
  }

  osThreadJoin(recv_thread, MAX_u64);
  osThreadJoin(send_thread, MAX_u64);

  return 0;
}

