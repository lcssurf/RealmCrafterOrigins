#pragma once
#ifndef RCO_PROTOCOL_PACKETS_H
#define RCO_PROTOCOL_PACKETS_H

#include <cstddef>
#include <cstdint>

// RCO shared protocol — C++ mirror of shared/protocol/packets.go
// This header is the single source of truth for packet IDs and result codes
// on the C++ side (client, tools). Keep in sync with packets.go.
//
// Packet header layout (HeaderSize bytes):
//   [0..1]  uint16  packet type  (little-endian)
//   [2..5]  uint32  payload length in bytes  (little-endian)

namespace rco::proto {

// ---------------------------------------------------------------------------
// Packet type IDs
// Values mirror RC 1.26 Packets.bb where applicable.
// RCO-specific extensions start at 100.
// ---------------------------------------------------------------------------

// Client -> Server: create a new account
constexpr uint16_t kCreateAccount    = 1;
// Client -> Server: verify / login to an existing account
constexpr uint16_t kVerifyAccount    = 2;
// Client -> Server: request character list for this account
constexpr uint16_t kFetchCharacter   = 3;
// Client -> Server: create a new character
constexpr uint16_t kCreateCharacter  = 4;
// Client -> Server: delete an existing character
constexpr uint16_t kDeleteCharacter  = 5;
// Client -> Server: change account password
constexpr uint16_t kChangePassword   = 6;
// Client -> Server: request actor (NPC/player) data for current area
constexpr uint16_t kFetchActors      = 7;
// Client -> Server: request item definitions
constexpr uint16_t kFetchItems       = 8;
// Server -> Client: notify client of area change
constexpr uint16_t kChangeArea       = 9;
// Server -> Client: a new actor has appeared in the area
constexpr uint16_t kNewActor         = 11;
// Server -> Client: game session starts (enter world)
constexpr uint16_t kStartGame        = 12;
// Server -> Client: an actor has left the area
constexpr uint16_t kActorGone        = 13;
// Bidirectional: position/state update for an actor
constexpr uint16_t kStandardUpdate   = 14;
// Server -> Client: player inventory changed
constexpr uint16_t kInventoryUpdate  = 15;
// Bidirectional: chat message
constexpr uint16_t kChatMessage      = 16;
// Server -> Client: weather state changed
constexpr uint16_t kWeatherChange    = 17;
// Client -> Server: player attacking an actor
constexpr uint16_t kAttackActor      = 18;
// Server -> Client: an actor has died
constexpr uint16_t kActorDead        = 19;
// Client -> Server: right-click interaction on an actor/object
constexpr uint16_t kRightClick       = 20;
// Server -> Client: open NPC dialog
constexpr uint16_t kDialog           = 21;
// Server -> Client: player stat values updated (HP, energy, attributes)
constexpr uint16_t kStatUpdate       = 22;
// Server -> Client: quest log updated
constexpr uint16_t kQuestLog         = 23;
// Server -> Client: player gold amount changed
constexpr uint16_t kGoldChange       = 24;
// Server -> Client: actor name changed
constexpr uint16_t kNameChange       = 25;
// Server -> Client: known spells list updated
constexpr uint16_t kKnownSpellUpdate = 26;
// Server -> Client: spell effect applied to actor
constexpr uint16_t kSpellUpdate      = 27;
// Server -> Client: create a particle emitter in the world
constexpr uint16_t kCreateEmitter    = 28;
// Server -> Client: play a sound effect
constexpr uint16_t kSound            = 29;
// Server -> Client: play an animation on an actor
constexpr uint16_t kAnimateActor     = 30;
// Server -> Client: action bar slots updated
constexpr uint16_t kActionBarUpdate  = 31;
// Server -> Client: player XP total updated
constexpr uint16_t kXPUpdate         = 32;
// Server -> Client: flash the screen with a colour (damage, heal, etc.)
constexpr uint16_t kScreenFlash      = 33;
// Server -> Client: change background music track
constexpr uint16_t kMusic            = 34;
// Server -> Client: open the player trading window
constexpr uint16_t kOpenTrading      = 35;
// Server -> Client: visual effect applied to an actor
constexpr uint16_t kActorEffect      = 36;
// Server -> Client: spawn a projectile
constexpr uint16_t kProjectile       = 37;
// Server -> Client: party member list / status updated
constexpr uint16_t kPartyUpdate      = 38;
// Server -> Client: actor appearance (gear/cosmetics) changed
constexpr uint16_t kAppearanceUpdate = 39;
// Server -> Client: close the trading window
constexpr uint16_t kCloseTrading     = 40;
// Bidirectional: update items currently offered in a trade
constexpr uint16_t kUpdateTrading    = 41;
// Client -> Server: player selected a scenery object
constexpr uint16_t kSelectScenery    = 42;
// Server -> Client: run an item script on the client
constexpr uint16_t kItemScript       = 43;
// Client -> Server: player consumes (eats/drinks) an item
constexpr uint16_t kEatItem          = 44;
// Server -> Client: item durability / health updated
constexpr uint16_t kItemHealth       = 45;
// Client -> Server: player jumped
constexpr uint16_t kJump             = 46;
// Client -> Server: player dismounts a mount
constexpr uint16_t kDismount         = 47;
// Server -> Client: display a floating number above an actor (damage, heal)
constexpr uint16_t kFloatingNumber   = 48;
// Server -> Client: teleport / hard-set an actor's position
constexpr uint16_t kRepositionActor  = 49;
// Server -> Client: display speech text above an actor's head
constexpr uint16_t kSpeech           = 50;
// Server -> Client: show or update a progress bar
constexpr uint16_t kProgressBar      = 51;
// Server -> Client: show a speech bubble on an actor
constexpr uint16_t kBubbleMessage    = 52;
// Server -> Client: request text input from the player via a script
constexpr uint16_t kScriptInput      = 53;
// Server -> Client: player has been kicked from the server
constexpr uint16_t kKickedPlayer     = 60;

// RCO extensions (>= 100) ---------------------------------------------------

// Server -> Client: result of a login / account-verify attempt
constexpr uint16_t kLoginResult      = 100;
// Server -> Client: full character list for this account
constexpr uint16_t kCharListResult   = 101;
// Server -> Client: result of a character creation request
constexpr uint16_t kCreateCharResult = 102;
// Server -> Client: result of a character deletion request
constexpr uint16_t kDeleteCharResult = 103;
// Bidirectional: keep-alive ping
constexpr uint16_t kPing             = 104;
// Bidirectional: keep-alive pong reply
constexpr uint16_t kPong             = 105;

// ---------------------------------------------------------------------------
// Result codes
// Returned in the first byte of most server response payloads.
// ---------------------------------------------------------------------------

// Operation succeeded.
constexpr uint8_t kResultOK            = 0;
// Login failed — wrong username or password.
constexpr uint8_t kResultInvalidCreds  = 1;
// Account creation failed — username already taken.
constexpr uint8_t kResultAccountExists = 2;
// Account is banned.
constexpr uint8_t kResultBanned        = 3;
// Account is already logged in from another connection.
constexpr uint8_t kResultAlreadyOnline = 4;
// Server is at player capacity.
constexpr uint8_t kResultServerFull    = 5;
// Character creation failed — name already taken.
constexpr uint8_t kResultCharExists    = 6;
// Character name failed validation (length, characters, profanity).
constexpr uint8_t kResultInvalidName   = 7;

// ---------------------------------------------------------------------------
// Packet header
// ---------------------------------------------------------------------------

// Every packet begins with a fixed-size header:
//   bytes 0-1  uint16  packet type  (little-endian)
//   bytes 2-5  uint32  payload length in bytes  (little-endian)
constexpr size_t kHeaderSize = 6;

} // namespace rco::proto

#endif // RCO_PROTOCOL_PACKETS_H
