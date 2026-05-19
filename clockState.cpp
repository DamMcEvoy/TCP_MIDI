#include "clockState.h"

ClockState::ClockState() = default;

void ClockState::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    hasSongPosition_ = false;
    pulseCount_ = 0;
    songPositionBeats_ = -1;
    transportRatePpq_ = static_cast<double>(kMidiClockPpq);
}

void ClockState::handleClockMessage(const MidiInputHandler::ClockMessage& clockMessage) {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (clockMessage.type) {
        case MidiInputHandler::ClockMessageType::TimingClock:
            ++pulseCount_;
            break;

        case MidiInputHandler::ClockMessageType::Start:
            running_ = true;
            pulseCount_ = 0;
            if (hasSongPosition_) {
                pulseCount_ = static_cast<uint64_t>(songPositionBeats_) * 6ULL;
            }
            break;

        case MidiInputHandler::ClockMessageType::Continue:
            running_ = true;
            if (hasSongPosition_) {
                pulseCount_ = static_cast<uint64_t>(songPositionBeats_) * 6ULL;
            }
            break;

        case MidiInputHandler::ClockMessageType::Stop:
            running_ = false;
            break;

        case MidiInputHandler::ClockMessageType::SongPositionPointer:
            if (clockMessage.songPosition >= 0) {
                applySongPositionPointer(clockMessage.songPosition);
            }
            break;

        default:
            break;
    }
}

ClockState::Snapshot ClockState::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Snapshot s;
    s.running = running_;
    s.hasSongPosition = hasSongPosition_;
    s.pulseCount = pulseCount_;
    s.quarterNoteCount = pulseCount_ / kMidiClockPpq;
    s.barBeatPulse = pulseCount_ % kMidiClockPpq;
    s.songPositionBeats = songPositionBeats_;
    s.transportRatePpq = transportRatePpq_;
    return s;
}

bool ClockState::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

uint64_t ClockState::pulseCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pulseCount_;
}

double ClockState::transportRatePpq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transportRatePpq_;
}

void ClockState::applySongPositionPointer(int songPositionBeats) {
    hasSongPosition_ = true;
    songPositionBeats_ = songPositionBeats;
    pulseCount_ = static_cast<uint64_t>(songPositionBeats_) * 6ULL;
}
