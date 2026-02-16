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
#define MAX_ENTITIES (2<<18)
#define LEFT_ROOM_ENTITES_LEN (KB(1))
#define ROOM_MAP_COLLISIONS_LEN MAX_ROOMS/8
#define CLIENT_COMMAND_LIST_LEN 8
#define SERVER_PORT 7777
#define SERVER_MAX_HEAP_MEMORY MB(256)
#define SERVER_MAX_CLIENTS 16
#define GAME_THREAD_CONCURRENCY 4
#define GOAL_NETWORK_SEND_LOOPS_PER_S 4
#define GOAL_NETWORK_SEND_LOOP_US 1000000/GOAL_NETWORK_SEND_LOOPS_PER_S
#define GOAL_GAME_LOOPS_PER_S 4
#define GOAL_GAME_LOOP_US 1000000/GOAL_GAME_LOOPS_PER_S
#define CLIENT_TIMEOUT_FRAMES GOAL_GAME_LOOPS_PER_S*3
#define CHUNK_SIZE 64
#define ACCOUNT_LEN (16)
#define PARSED_CLIENT_COMMAND_THREAD_QUEUE_LEN 64

///// TypeDefs
typedef struct ParsedClientCommand {
  CommandType type;
  u8 byte;
  u16 sender_port;
  u16 alt_port;
  u32 sender_ip;
  u32 alt_ip;
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

typedef struct Entity {
  bool changed;
  u8 x;
  u8 y;
  u8 color;
  EntityType type;
  u32 misc;
  u64 id;
  u64 features;
} Entity;

typedef struct Account {
  u32 id; // the index in the array
  Pos2 pos;
  String name;
  String pw;
  PlayerShip ship;
} Account;

typedef struct EntityList {
  u64 length; // the currently used length
  u64 capacity;
  Entity* items;
} EntityList;

typedef struct EntityChunk {
  u64 length; // the currently used # of entities in this chunk
  u64 capacity; // the "chunk size" / space in this chunk
  struct EntityChunk* next; // the next chunk
  Entity* items; // the actual entities
} EntityChunk;

typedef struct ChunkedEntityList {
  u64 length; // the current number of entities
  u64 chunk_size; // the # of entities per chunk
  u64 chunks; // the # of chunks in this list so far
  EntityChunk* first; // the first chunk of entities
} ChunkedEntityList;

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
global Arena permanent_arena = { 0 };
global const Entity NULL_ENTITY = { 0 };
global bool debug_mode = false;
global ChunkedEntityList free_chunks = { 0, CHUNK_SIZE, 0, NULL };

///// functionImplementations()
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

fn u64 entitySerialize(Entity current, Account* acct, u64 index, u8 bytes[]) {
  // send entity header (common to all entity types)
  index += writeU64ToBufferLE(bytes + index, current.id);
  index += writeU64ToBufferLE(bytes + index, current.features);
  bytes[index++] = current.x;
  bytes[index++] = current.y;
  bytes[index++] = (u8)current.type;  // EntityType

  //if (CheckFlag(current.features, FeatureRegensHp)) {
  //  index += writeU16ToBufferLE(bytes + index, current.hp);
  //  index += writeU16ToBufferLE(bytes + index, current.max_hp);
  //}

  // send character-specific details (color and name)
  if (current.type == EntityCharacter) {
    bytes[index++] = current.color;
    // send name string
    index += writeU64ToBufferLE(bytes + index, acct->name.length);
    for (u32 j = 0; j < acct->name.length; j++) {
      bytes[index++] = acct->name.bytes[j];
    }
  }
  return index;
}

fn u64 entityFeaturesFromType(EntityType type) {
  u64 result = 0;
  switch (type) {
    case EntityCharacter: {
      SetFlag(result, FeatureWalksAround);
      SetFlag(result, FeatureCanFight);
    } break;
    default: {
    } break;
  }
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

fn u64 pushEntity(EntityChunk* chunk, Entity e) {
  assert(chunk->length < chunk->capacity);
  chunk->items[chunk->length] = e;
  u64 result = chunk->length;
  chunk->length += 1;
  return result;
}

fn bool isSpaceInChunk(EntityChunk* chunk) {
  return chunk->length < chunk->capacity;
}

fn bool allChunksFull(ChunkedEntityList list) {
  return list.length >= (list.chunk_size * list.chunks);
}

fn EntityChunk* pushNewChunk(Arena* a, ChunkedEntityList* list) {
  EntityChunk* new_chunk;
  if (free_chunks.length > 0) {
    new_chunk = free_chunks.first;
    free_chunks.length -= 1;
    free_chunks.first = new_chunk->next;
  } else {
    new_chunk = arenaAlloc(a, sizeof(EntityChunk));
    // alloc the new chunk of entities
    new_chunk->length = 0;
    new_chunk->capacity = list->chunk_size;
    new_chunk->next = NULL;
    new_chunk->items = arenaAllocArray(a, Entity, list->chunk_size);
  }
  // bookeeping in the list
  list->chunks += 1;
  if (list->first == NULL) {
    list->first = new_chunk;
  } else {
    EntityChunk* last = list->first;
    while (last->next != NULL) {
      last = last->next;
    }
    last->next = new_chunk;
  }
  return new_chunk;
}

fn Entity* entityPtrFromChunkList(ChunkedEntityList* list, i32 index) {
  Entity* result = (Entity*)&NULL_ENTITY;
  i32 chunk_index = index / list->chunk_size;
  EntityChunk* chunk = list->first;
  for (i32 i = 0; i < chunk_index; i++) {
    chunk = chunk->next;
  }
  result = &chunk->items[index % list->chunk_size];
  return result;
}

fn Entity entityFromChunkList(ChunkedEntityList* list, i32 index) {
  Entity result = {0};
  i32 chunk_index = index / list->chunk_size;
  EntityChunk* chunk = list->first;
  for (i32 i = 0; i < chunk_index; i++) {
    chunk = chunk->next;
  }
  result = chunk->items[index % list->chunk_size];
  return result;
}

fn bool deleteLastEntity(ChunkedEntityList* list, EntityChunk* last_chunk, EntityChunk* second_to_last_chunk) {
  last_chunk->length -= 1;
  list->length -= 1;
  // don't delete the room's only chunk, but otherwise move the chunk to the free-list
  if (last_chunk->length == 0 && last_chunk != list->first) {
    list->chunks -= 1;
    second_to_last_chunk->next = NULL;
    free_chunks.length += 1;
    EntityChunk* last_free_chunk = free_chunks.first;
    if (last_free_chunk) {
      while (last_free_chunk->next != NULL) {
        last_free_chunk = last_free_chunk->next;
      }
      last_free_chunk->next = last_chunk;
    } else {
      free_chunks.first = last_chunk;
    }
  }
  return true;
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

fn void handleIncomingMessage(u8* message, u32 len, SocketAddress sender, i32 socket) {
  dbg("%d: %s from %s:%d\n", len, command_type_strings[message[0]], inet_ntoa(sender.sin_addr), sender.sin_port);
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

      printf("Logging in player: %s %d %s\n", MESSAGE_STRINGS[parsed.type], name_len, message + 7);
    } break;
    case CommandKeepAlive:
      break;
    case CommandCreateCharacter: {
      printf("command create character received\n");
      parsed.byte = message[1];
    } break;
    case CommandInvalid:
    case CommandType_Count: {
      dbg("invalid command type");
    } break;
  }

  pccThreadSafeQueuePush(state.network_recv_queue, &parsed);
  fflush(stdout);
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
  while (true) {
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

    lockMutex(&state.client_mutex); {
      // WARNING the `i` starts at 1 here because state.clients.items[0] is a "null" Client
      for (u32 i = 1; i < state.clients.length; i++) {
        Client client = state.clients.items[i];
        if (client.last_ping+CLIENT_TIMEOUT_FRAMES < state.frame) {
          memset(&state.clients.items[i], 0, sizeof(Client));
          continue;
        }
        //if (client.character_eid == 0) {
        //  continue; // they are still creating their character
        //}

      }
    } unlockMutex(&state.client_mutex);

    u32 loop_duration = osTimeMicrosecondsNow() - loop_start;
    i32 remaining_time = GOAL_NETWORK_SEND_LOOP_US - loop_duration;
    if (remaining_time > 0) {
      osSleepMicroseconds(remaining_time);
    }
  }
  return NULL;
}

fn void* gameLoop(void* params) {
  LaneCtx* lane_ctx = (LaneCtx*)params;
  ThreadContext tctx = {
    .lane_ctx = *lane_ctx,
  };
  tctxInit(&tctx);
  printf("Lane %lld (%lld) of %lld starting.\n", lane_ctx->lane_idx, LaneIdx(), lane_ctx->lane_count);
  fflush(stdout);
  UDPMessage outgoing_message = {0};
  u64 loop_start;
  u64 last_burn = 0;
  u64 last_hp_regen = 0;
  Arena scratch_arena = {0};
  arenaInit(&scratch_arena);
  while (true) {
    loop_start = osTimeMicrosecondsNow();

    if (LaneIdx() == 0) { // narrow
      state.frame += 1;

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
          case CommandKeepAlive: {
            dbg("KeepAlive for client_handle=%d, %ld", client_handle, state.frame);
            state.clients.items[client_handle].last_ping = state.frame;
          } break;
          case CommandLogin: {
            if (client_handle == 0) {
              client_handle = pushClient(&state.clients, sender);
              client = &state.clients.items[client_handle];
              printf("pushed new client handle = %d\n", client_handle);
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
            printf("name(%d): %s pw(%d): %s acct?: %d\n", name.length, name.bytes, pw.length, pw.bytes, existing_account != NULL);
            fflush(stdout);
            if (existing_account) {
              printf(" existing account\n");
              bool pw_matches = stringsEq(&pw, &existing_account->pw);
              arenaDealloc(&permanent_arena, pw.capacity);
              arenaDealloc(&permanent_arena, name.capacity);
              if (pw_matches) {
                printf(" pw matched\n");
              } else {
                // tell the client they did a bad pw
                outgoing_message.bytes[0] = (u8)MessageBadPw;
                outgoing_message.bytes_len = 1;
                outgoing_message.address = sender;
                outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
                printf("MessageBadPw sent\n");
                break;
              }
            } else {
              for (u32 i = 0; i < ACCOUNT_LEN; i++) {
                if (state.accounts[i].id == 0 && state.accounts[i].name.length == 0) {
                  existing_account = &state.accounts[i];
                  existing_account->id = i;
                }
              }
              existing_account->name = name;
              existing_account->pw = pw;
            }
            client->account_id = existing_account->id;
            if (existing_account->ship.id != 0) {
              // tell the client their account id
              outgoing_message.bytes[0] = (u8)MessageCharacterId;
              writeU64ToBufferLE(outgoing_message.bytes + 1, existing_account->id);
              outgoing_message.bytes_len = 9;
              outgoing_message.address = sender;
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              printf("MessageCharacterId sent\n");
            } else {
              // tell the client they made a new account
              outgoing_message.bytes[0] = (u8)MessageNewAccountCreated;
              outgoing_message.bytes_len = 1;
              outgoing_message.address = sender;
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              printf("MessageNewAccountCreated sent\n");
            }
            printf("client_handle=%d, acct_id=%d\n", client_handle, existing_account->id);
          } break;
          case CommandCreateCharacter: {
            Account account = state.accounts[client->account_id];
            if (account.ship.id == 0) {
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
                //.cu_m_fuel; // we are just saying you can buy as much fuel as you want
                //.cu_m_o2; // we are just saying you can buy as much o2 as you want
                .id = ++state.next_eid, // start from 1
              };
              account.ship = player_ship;
              printf("ship_type=%s, client_handle=%d, acct_id=%d\n", SHIP_TYPE_STRINGS[msg.byte], client_handle, account.id);
              u32 starting_system_idx = rand() % STAR_SYSTEM_COUNT;
              StarSystem starting_system = state.map[starting_system_idx];
              account.pos.x = starting_system.x;
              account.pos.y = starting_system.y;

              // tell the client their account id
              outgoing_message.address = sender;
              outgoing_message.bytes_len = 9;
              outgoing_message.bytes[0] = (u8)MessageCharacterId;
              writeU64ToBufferLE(outgoing_message.bytes + 1, account.id);
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              printf("MessageCharacterId sent\n");

              // TODO tell the client about the map
              u32 star_msg_size = 2+MAX_PLANETS;
              outgoing_message.address = sender;
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
              printf("%s sent\n", MESSAGE_STRINGS[outgoing_message.bytes[0]]);

              u32 planet_count = 0;
              for (u32 i = 0; i < MAX_PLANETS; i++) {
                if (starting_system.planets[i].type != PlanetTypeNull) {
                  planet_count++;
                }
              }
              outgoing_message.bytes_len = 1 + 1 + 1 + (planet_count * Commodity_Count);
              u32 msg_i = 0;
              outgoing_message.bytes[msg_i++] = (u8)MessageSystemCommodities;
              outgoing_message.bytes[msg_i++] = (u8)starting_system_idx;
              outgoing_message.bytes[msg_i++] = (u8)planet_count;
              for (u32 i = 0; i < planet_count; i++) {
                for (u32 ii = 0; ii < Commodity_Count; ii++) {
                  outgoing_message.bytes[msg_i++] = starting_system.planets[i].commodities[ii];
                }
              }
              outgoingMessageQueuePush(state.network_send_queue, &outgoing_message);
              printf("%s sent\n", MESSAGE_STRINGS[outgoing_message.bytes[0]]);

            } else {
              printf("client tried to create a character when he already has one.");
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

      } unlockMutex(&state.mutex); unlockMutex(&state.client_mutex);
    }

    LaneSync();

    // 2. tick non-user entities
    // iterate all the rooms
    /*
    Room* room = NULL;
    Range1u64 room_range = LaneRange(MAX_ROOMS);
    for (u32 i = room_range.min; i < room_range.max; i++) {
      room = &state.rooms->items[i];
      simulateRoom(&scratch_arena, room, burn_tick, regen_hp_tick);
    }
    */

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
  // set position of all star systems
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    state.map[i].name = STAR_NAMES[i];
    state.map[i].crime = rand() % 100;
    state.map[i].x = rand() % MAP_WIDTH;
    state.map[i].y = rand() % MAP_HEIGHT;
    bool conflicting_pos = false;
    for (u32 ii = 0; ii < i; ii++) {
      if (state.map[ii].x == state.map[i].x && state.map[ii].y == state.map[i].y) {
        conflicting_pos = true;
      } 
    }
    if (conflicting_pos) {
      i--;
    }
  }
  for (u32 i = 0; i < STAR_SYSTEM_COUNT; i++) {
    u32 planet_count = rand() % MAX_PLANETS + 1;
    for (u32 ii = 0; ii < planet_count; ii++) {
      state.map[i].planets[ii].type = 1+(PlanetType)(rand() % (PlanetType_Count -1));
      for (u32 iii = 0; iii < Commodity_Count; iii++) {
        state.map[i].planets[ii].commodities[iii] = rand() % 1000;
        state.map[i].planets[ii].production[iii] = rand() % 10;
      }
      switch (state.map[i].planets[ii].type) {
        case PlanetTypeEarth: {
          // TODO: roll the initial commodity counts
          //  AND roll the initial (excess) production levels
          //  earth planets do a lot of food 
        } break;
        case PlanetTypeGas: {
          // a lot of fuel and chemicals
        } break;
        case PlanetTypeMoon: {
          // a lot of manufactured goods
        } break;
        case PlanetTypeAsteroid: {
          // a lot of raw metals
        } break;
        case PlanetTypeStation: {
          // a lot of ??? drugs?
        } break;
        case PlanetTypeNull:
        case PlanetType_Count:
          break;
      }
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
  for (u32 i = 0; i < GAME_THREAD_CONCURRENCY; i++) {
    osThreadJoin(game_threads[i], MAX_u64);
  }

  return 0;
}

