/* 

*/
#include "recoveryJournal.h"

#include <cstring>

namespace {
//low-level big-endian helpers
void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
	out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
	out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u64_be(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint16_t read_u16_be(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return (static_cast<uint16_t>(bytes[offset]) << 8) |
           static_cast<uint16_t>(bytes[offset + 1]);
}

uint32_t read_u32_be(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

uint64_t read_u64_be(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return (static_cast<uint64_t>(bytes[offset]) << 56) |
           (static_cast<uint64_t>(bytes[offset + 1]) << 48) |
           (static_cast<uint64_t>(bytes[offset + 2]) << 40) |
           (static_cast<uint64_t>(bytes[offset + 3]) << 32) |
           (static_cast<uint64_t>(bytes[offset + 4]) << 24) |
           (static_cast<uint64_t>(bytes[offset + 5]) << 16) |
           (static_cast<uint64_t>(bytes[offset + 6]) << 8) |
           static_cast<uint64_t>(bytes[offset + 7]);
}

} // namespace

namespace RecoveryJournal {

bool hasEncodedJournal(const TimedMidiEvent& event) {
    return !event.journalBytes.empty();
}

bool isRecoveryJournalType(uint16_t journalType) {
    return journalType == kJournalTypeCheckpoint ||
           journalType == kJournalTypeDiscontinuity;
}

const char* journalTypeToString(uint16_t journalType){
	switch (journalType) {
		case kJournalTypeNone:
			return "none";
		case kJournalTypeCheckpoint:
			return "checkpoint";
		case kJournalTypeDiscontinuity:
			return "discontinuity";
		default:
			return "unknown";
	}
}

std::vector<uint8_t> encode(const Snapshot& snapshot) {
	std::vector<uint8_t> out;
	out.reserve(24 + snapshot.recoveryCommands.size());

	uint16_t flags = 0;
	if (snapshot.transportRunning) {
		flags |= kJournalFlagTransportRunning;
	}
	if (snapshot.hasSongPosition) {
		flags |= kJournalFlagHasSongPosition;
	}
	if (snapshot.transportDiscontinuity) {
		flags |= kJournalFlagTransportDiscontinuity;
	}
	if (!snapshot.recoveryCommands.empty()) {
		flags |= kJournalFlagHasRecoveryCommands;
	}

	append_u16_be(out, snapshot.wireVersion);
	append_u16_be(out, snapshot.journalType);
	append_u16_be(out, flags);
	append_u16_be(out, static_cast<uint16_t>(snapshot.recoveryCommands.size()));

	append_u32_be(out, snapshot.sequenceBase);
	append_u64_be(out, snapshot.transportEpoch);

	const uint32_t encodedSongPosition = 
		snapshot.hasSongPosition && snapshot.songPositionBeats >= 0 
		? static_cast<uint32_t>(snapshot.songPositionBeats)
		: 0xFFFFFFFFu;

	append_u32_be(out, encodedSongPosition);

	out.insert(out.end(), 
				snapshot.recoveryCommands.begin(),
				snapshot.recoveryCommands.end());

	return out;
}

DecodeResult decode(const std::vector<uint8_t>& bytes) {
	DecodeResult result;
	/*A hard minimum like this is important because binary parsers should reject malformed input early and explicitly*/
	constexpr std::size_t kMinimumSize = 2 + 2 + 2 + 2 + 4 + 8 + 4;
	if (bytes.size() < kMinimumSize) {
		result.error = "journal too short";
		return result;
	}
	/*Load the raw data into a structured snapshot.*/
	Snapshot snapshot;
	snapshot.wireVersion = read_u16_be(bytes, 0);
	snapshot.journalType = read_u16_be(bytes, 2);
	const uint16_t flags = read_u16_be(bytes, 4);
	const uint16_t recoveryCommandLength = read_u16_be(bytes, 6);
	snapshot.sequenceBase = read_u32_be(bytes, 8);
	snapshot.transportEpoch = read_u64_be(bytes, 12);

	const uint32_t encodedSongPosition = read_u32_be(bytes, 20);

	if (snapshot.wireVersion != kJournalWireVersion) {
		result.error = "unsupported journal wire version";
		return result;
	}

	if(!isRecoveryJournalType(snapshot.journalType) &&
		snapshot.journalType != kJournalTypeNone) {
		result.error = "unknown journal type";
		return result;
	}

	snapshot.transportRunning = (flags & kJournalFlagTransportRunning) != 0;
	snapshot.hasSongPosition = (flags & kJournalFlagHasSongPosition) != 0;
	snapshot.transportDiscontinuity = (flags & kJournalFlagTransportDiscontinuity) != 0;
	/*This is where the raw bits become meaningful recovery state*/
	if (snapshot.hasSongPosition) {
		if (encodedSongPosition == 0xFFFFFFFFu) {
			result.error = "journal flagged song position but none encoded";
			return result;
		}
		snapshot.songPositionBeats = static_cast<uint32_t>(encodedSongPosition);
	} else {
		snapshot.songPositionBeats = -1;
	}

	const std::size_t expectedSize = kMinimumSize +recoveryCommandLength;
	if (bytes.size() != expectedSize) {
		result.error = "journal size mismatch";
		return result;
	}

	if ((flags & kJournalFlagHasRecoveryCommands) != 0) {
		snapshot.recoveryCommands.assign(bytes.begin() + kMinimumSize, bytes.end());
	} else if (recoveryCommandLength != 0) {
		result.error = "journal recovery command length present without flag";
		return result;
	}

	result.ok = true;
	result.snapshot = std::move(snapshot);
	return result;
}
/*make the journal snapshot populate the event model*/
bool applyToEvent(const Snapshot& snapshot, TimedMidiEvent& event) {
    event.journalType = snapshot.journalType;
    event.journalBytes = encode(snapshot);

    event.transportEpoch = snapshot.transportEpoch;
    event.transportRunning = snapshot.transportRunning;
    event.hasSongPosition = snapshot.hasSongPosition;
    event.songPositionBeats = snapshot.hasSongPosition
        ? snapshot.songPositionBeats
        : -1;
    event.transportDiscontinuity = snapshot.transportDiscontinuity;

    return true;
}

bool extractFromEvent(const TimedMidiEvent& event, Snapshot& snapshot) {
    snapshot = Snapshot{};
    snapshot.sequenceBase = event.sequence;

    if (!event.journalBytes.empty()) {
        const auto decoded = decode(event.journalBytes);
        if (!decoded.ok) {
            return false;
        }

        snapshot = decoded.snapshot;
        snapshot.sequenceBase = event.sequence;

        if (snapshot.journalType == kJournalTypeNone && event.journalType != kJournalTypeNone) {
            snapshot.journalType = event.journalType;
        }

        if (snapshot.transportEpoch == 0) {
            snapshot.transportEpoch = event.transportEpoch;
        }

        if (!snapshot.transportRunning && event.transportRunning) {
            snapshot.transportRunning = event.transportRunning;
        }

        if (!snapshot.hasSongPosition && event.hasSongPosition) {
            snapshot.hasSongPosition = true;
            snapshot.songPositionBeats = static_cast<int32_t>(event.songPositionBeats);
        }

        if (!snapshot.transportDiscontinuity && event.transportDiscontinuity) {
            snapshot.transportDiscontinuity = true;
        }

        return true;
    }

    snapshot.journalType = event.journalType;
    snapshot.transportEpoch = event.transportEpoch;
    snapshot.transportRunning = event.transportRunning;
    snapshot.hasSongPosition = event.hasSongPosition;
    snapshot.songPositionBeats = event.hasSongPosition
        ? static_cast<int32_t>(event.songPositionBeats)
        : -1;
    snapshot.transportDiscontinuity = event.transportDiscontinuity;
    snapshot.recoveryCommands.clear();

    if (snapshot.journalType == kJournalTypeNone) {
        if (snapshot.transportDiscontinuity) {
            snapshot.journalType = kJournalTypeDiscontinuity;
        } else if (snapshot.transportRunning ||
                   snapshot.hasSongPosition ||
                   snapshot.transportEpoch != 0) {
            snapshot.journalType = kJournalTypeCheckpoint;
        }
    }

    return true;
}

} // namespace RecoveryJournal