#ifndef SCAV_CORE_CORPUS_CHARTS_H_INCLUDED
#define SCAV_CORE_CORPUS_CHARTS_H_INCLUDED

// The hand-transcribed corpus (P0): real state machines written the way
// someone would write them. Shared by the parser's corpus tests and the model
// tests that lower the same charts, so both halves of the pipeline are
// measured against identical bytes. Header-only and test-only.

#include <string_view>
#include <vector>

namespace scav::test {

// the design's own worked example, transcribed verbatim including its comment-free
// shape, so the document the format is specified by is a document that parses.
constexpr std::string_view VAC{ R"(
chart vac "robot vacuum" {
  include "wifi.scav" as wifi,

  state Off "powered down",
  state Booting,
  state PreConfig choice,

  trans * -> Off,
  trans Off -> Booting "POWER_ON",

  state On {
    @doc = "Enter: publishes EVT_POWERED_ON",
    @libhsm { submachine_handler, legacy = "false" },

    submachine main {
      state Idle { @libhsm:handler = "false" },
      state Ready,
      trans * -> Idle,
      trans internal Ready -> Ready "BUMP_RETRY",
      trans Ready -> wifi/On/Connected "handoff",
    },
    submachine strays "sweeps while main drives" {
      state Idle,
      trans * -> Idle,
    },
  },
}
)" };

// A TCP connection. Real-world shape: many peer states, a couple of
// pseudostates, one deep-history resume, and transitions that skip levels.
constexpr std::string_view TCP{ R"(
// RFC 793 connection states, as a chart rather than as a table.
chart tcp "TCP connection" {
  @doc = "Passive and active opens share the established substate.",

  state Closed,
  state Listen,
  state SynSent,
  state SynReceived,

  trans * -> Closed,
  trans Closed -> Listen "passive open",
  trans Closed -> SynSent "active open",
  trans Listen -> SynReceived "recv SYN",
  trans SynSent -> SynReceived "recv SYN",

  state Established "data may flow both ways" {
    // Both halves close independently, which is why this is concurrent.
    submachine inbound {
      state Open,
      state HalfClosed,
      trans * -> Open,
      trans Open -> HalfClosed "recv FIN",
    },
    submachine outbound {
      state Open,
      state FinSent,
      trans * -> Open,
      trans Open -> FinSent "send FIN", // no more data from us
    },
  },

  trans SynReceived -> Established "recv ACK",
  trans SynSent -> Established "recv SYN+ACK",

  state Closing choice,
  state TimeWait,

  // A transition out of a nested state to a top-level one: the long
  // hierarchical edge the whole project exists for.
  trans Established:inbound/HalfClosed -> Closing,
  trans Established:outbound/FinSent -> Closing,
  trans Closing -> TimeWait "both halves closed",
  trans TimeWait -> Closed "2MSL elapsed",
}
)" };

// Firmware over-the-air update. Written in the terse aliases a person drafting
// reaches for, with a fork/join pair and a raw-string label.
constexpr std::string_view OTA{ R"(
chart ota "firmware OTA" {
  @vendor:component = "bootloader",
  @tags = ["firmware", "safety-critical"],

  s Idle,
  t * -> Idle,

  s Downloading {
    @doc = """
      Chunks arrive out of order and are written straight to the
      inactive slot. The manifest is verified only once every chunk
      has landed.
      """,
    m main {
      s Fetching,
      s Writing,
      t * -> Fetching,
      t Fetching -> Writing "chunk ready",
      t internal Writing -> Writing "flash busy",
      t Writing -> Fetching "chunk written",
    },
  },

  s Verify fork,
  s CheckSignature,
  s CheckVersion,
  s Verified join,
  s Failed,

  t Idle -> Downloading "update offered",
  t Downloading -> Verify "all chunks received",
  t Verify -> CheckSignature,
  t Verify -> CheckVersion,
  t CheckSignature -> Verified,
  t CheckVersion -> Verified,
  t Verified -> Idle "swap slots and reboot",

  // Anything can fail at any time, and failure is terminal for this attempt.
  t CheckSignature -> Failed "bad signature",
  t CheckVersion -> Failed "downgrade refused",
  t Downloading -> Failed "timeout",
  t Failed -> *,
}
)" };

struct CorpusChart {
  char const *name;
  std::string_view text;
};

inline std::vector<CorpusChart> corpus() {
  return { { "vac", VAC }, { "tcp", TCP }, { "ota", OTA } };
}

}  // namespace scav::test

#endif  // SCAV_CORE_CORPUS_CHARTS_H_INCLUDED
