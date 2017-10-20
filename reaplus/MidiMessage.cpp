#include "MidiMessage.h"

namespace reaplus {
  int MidiMessage::frameOffset() const {
    return frameOffset_;
  }

  int MidiMessage::size() const {
    return (int) bytes_.size();
  }

  unsigned char MidiMessage::byte(int i) const {
    return bytes_.at((size_t) i);
  }

  MidiMessage::MidiMessage(unsigned char statusByte, unsigned char dataByte1, unsigned char dataByte2,
                           int frameOffset) : frameOffset_(frameOffset) {
    bytes_.reserve((size_t) 3);
    bytes_.push_back(statusByte);
    bytes_.push_back(dataByte1);
    bytes_.push_back(dataByte2);
  }

  MidiMessage::MidiMessage(const MIDI_event_t& event) : frameOffset_(event.frame_offset) {
    bytes_.reserve((size_t) event.size);
    for (int i = 0; i < event.size; i++) {
      bytes_.push_back(event.midi_message[i]);
    }
  }

  MidiMessageType MidiMessage::type() const {
    // Truncate low nibble from status byte
    const auto lowNibble = (unsigned char) (statusByte() & 0xf0);
    return static_cast<MidiMessageType>(lowNibble);
  }

  unsigned char MidiMessage::statusByte() const {
    return byte(0);
  }

  unsigned char MidiMessage::dataByte1() const {
    return byte(1);
  }

  unsigned char MidiMessage::dataByte2() const {
    return byte(2);
  }

  bool MidiMessage::isNoteOn() const {
    return type() == MidiMessageType::NoteOn && velocity() > 0;
  }

  bool MidiMessage::isNoteOff() const {
    return type() == MidiMessageType::NoteOff || type() == MidiMessageType::NoteOn && velocity() == 0;
  }

  bool MidiMessage::isNote() const {
    return type() == MidiMessageType::NoteOn || type() == MidiMessageType::NoteOff;
  }

  unsigned char MidiMessage::note() const {
    return dataByte1();
  }

  unsigned char MidiMessage::velocity() const {
    return dataByte2();
  }

  unsigned char MidiMessage::channel() const {
    // Extract low nibble from status byte
    return (unsigned char) (statusByte() & 0xf);
  }

  MidiMessage MidiMessage::simple(MidiMessageType type, int channel, int data1, int data2, int frameOffset) {
    return MidiMessage(
        static_cast<unsigned char>(channel) | static_cast<unsigned char>(type),
        static_cast<unsigned char>(data1),
        static_cast<unsigned char>(data2),
        frameOffset
    );
  }

  MidiMessage MidiMessage::noteOn(int channel, int noteNumber, int velocity, int frameOffset) {
    return simple(MidiMessageType::NoteOn, channel, noteNumber, velocity, frameOffset);
  }

  MidiMessage MidiMessage::noteOff(int channel, int noteNumber, int velocity, int frameOffset) {
    return simple(MidiMessageType::NoteOff, channel, noteNumber, velocity, frameOffset);
  }

  MidiMessage MidiMessage::cc(int channel, int ccNumber, int ccValue, int frameOffset) {
    return simple(MidiMessageType::Cc, channel, ccNumber, ccValue, frameOffset);
  }

  MidiMessage MidiMessage::pitchWheel(int channel, int pitchValue, int frameOffset) {
    return simple(MidiMessageType::PitchWheel, channel, pitchValue & 0x7F, pitchValue >> 7, frameOffset);
  }

  MidiMessage MidiMessage::channelAftertouch(int channel, int aftertouchValue, int frameOffset) {
    return simple(MidiMessageType::ChannelAftertouch, channel, aftertouchValue, 0, frameOffset);
  }

  MidiMessage MidiMessage::programChange(int channel, int programIndex, int frameOffset) {
    return simple(MidiMessageType::ProgramChange, channel, programIndex, 0, frameOffset);
  }

  std::pair<MidiMessage, MidiMessage>
  MidiMessage::fourteenBitCc(int channel, int ccNumber, int ccValue, int frameOffset) {
    const MidiMessage msbMessage = simple(MidiMessageType::Cc, channel, ccNumber, ccValue >> 7, frameOffset);
    const MidiMessage lsbMessage = simple(MidiMessageType::Cc, channel, ccNumber + 32, ccValue & 0x7F, frameOffset);
    return std::pair<MidiMessage, MidiMessage>(msbMessage, lsbMessage);
  }
}
