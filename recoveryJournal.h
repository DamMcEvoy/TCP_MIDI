/*The struct Snapshot is the heart of the journal API. 
It is the decoded meaning of the journal, not the raw bytes themselves. 
It maps directly onto the recovery-related fields already present in TimedMidiEvent*/

#ifndef RECOVERY_JOURNAL_H
#define RECOVERY_JOURNAL_H

#include <cstdint>
#include <string>
#include <vector>

#include <TimedMidiEvent.h>

namespace RecoveryJournal {

constexpr uint16_t kJournalWireVersion = 1;
constexpr uint16_t kJournalTypeNone = 0;
constexpr uint16_t kJournalTypeCheckpoint = 1;
constexpr uint16_t kJournalTypeDiscontinuity = 2;

constexpr uint16_t kJournalFlagTransportRunning = 0x0001;
constexpr uint16_t kJournalFlagHasSongPosition = 0x0002;
constexpr uint16_t kJournalFlagTransportDiscontinuity = 0x0004;
constexpr uint16_t kJournalFlagHasRecoveryCommands = 0x0008;

struct Snapshot {
	uint16_t wireVersion = kJournalWireVersion;
	uint16_t journalType = kJournalTypeNone;

	uint32_t sequenceBase = 0;

	uint64_t transportEpoch = 0;
	bool transportRunning = false;
	bool hasSongPosition = false;
	int32_t songPositionBeats = -1;
	bool transportDiscontinuity = false;

	std::vector<uint8_t> recoveryCommands;
};

struct DecodeResult {
	bool ok = false;
	std::string error;
	Snapshot snapshot;
};

std::vector<uint8_t> encode(const Snapshot& snapshot);

DecodeResult decode(const std::vector<uint8_t>& bytes);

bool applyToEvent(const Snapshot& snapshot, TimedMidiEvent& event);

bool extractFromEvent(const TimedMidiEvent& event, Snapshot& snapshot);

bool hasEncodedJournal(const TimedMidiEvent& event);

bool isRecoveryJournalType(uint16_t journalType);

const char* journalTypeToString(uint16_t journalType);

} //namespace RecoveryJournal

#endif //RECOVERY_JOURNAL_H