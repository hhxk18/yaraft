// Copyright 2017 The etcd Authors
// Copyright 2017 Wu Tao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <unordered_set>

#include "memory_storage.h"
#include "raft.h"
#include "test_utils.h"

namespace yaraft {

class RaftPaperTest : public BaseTest {
 public:
  // TestUpdateTermFromMessage tests that if one server’s current term is
  // smaller than the other’s, then it updates its current term to the larger
  // value. If a candidate or leader discovers that its term is out of date,
  // it immediately reverts to follower state.
  // Reference: section 5.1
  static void TestUpdateTermFromMessage(Raft::StateRole role) {
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));
    switch (role) {
      case Raft::kLeader:
        r->becomeCandidate();
        r->becomeLeader();
        break;
      case Raft::kCandidate:
        r->becomeCandidate();
        break;
      case Raft::kFollower:
        break;
      default:
        break;
    }

    r->Step(PBMessage().Term(2).Type(pb::MsgApp).v);
    ASSERT_EQ(r->currentTerm_, 2);
    ASSERT_EQ(r->role_, Raft::kFollower);
  }

  // TestRejectStaleTermMessage tests that if a server receives a request with
  // a stale term number, it rejects the request.
  // Our implementation ignores the request instead.
  // Reference: section 5.1
  static void TestRejectStaleTermMessage() {
    // This is already tested by Raft.StepIgnoreOldTermMsg
  }

  // TestStartAsFollower tests that when servers start up, they begin as followers.
  // Reference: section 5.2
  static void TestStartAsFollower() {
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));
    ASSERT_EQ(r->role_, Raft::kFollower);
  }

  // TestLeaderBcastBeat tests that if the leader receives a heartbeat tick,
  // it will send a msgApp with m.Index = 0, m.LogTerm=0 and empty entries as
  // heartbeat to all followers.
  // Reference: section 5.2
  static void TestLeaderBcastBeat() {
    int heartbeatInterval = 1;
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, heartbeatInterval, new MemoryStorage()));
    r->becomeCandidate();
    r->becomeLeader();

    for (int i = 0; i < heartbeatInterval; i++) {
      r->Tick();
    }

    ASSERT_EQ(r->mails_.size(), 2);

    std::unordered_set<std::string> s1;
    std::for_each(r->mails_.begin(), r->mails_.end(),
                  [&](const pb::Message& m) { s1.insert(DumpPB(m)); });

    std::unordered_set<std::string> s2;
    s2.insert(DumpPB(PBMessage().From(1).To(2).Term(1).Commit(0).Type(pb::MsgHeartbeat).v));
    s2.insert(DumpPB(PBMessage().From(1).To(3).Term(1).Commit(0).Type(pb::MsgHeartbeat).v));

    ASSERT_EQ(s1, s2);
  }

  // TestNonleaderStartElection tests that if a follower receives no communication
  // over election timeout, it begins an election to choose a new leader. It
  // increments its current term and transitions to candidate state. It then
  // votes for itself and issues RequestVote RPCs in parallel to each of the
  // other servers in the cluster.
  // Reference: section 5.2
  // Also if a candidate fails to obtain a majority, it will time out and
  // start a new election by incrementing its term and initiating another
  // round of RequestVote RPCs.
  // Reference: section 5.2
  static void TestNonleaderStartElection(Raft::StateRole role) {
    int electionTimeout = 10;
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, electionTimeout, 1, new MemoryStorage()));

    if (role == Raft::kFollower) {
      // term = 1, lead = 2
      r->becomeFollower(1, 2);
    } else if (role == Raft::kCandidate) {
      r->becomeCandidate();
    }

    for (int i = 1; i < 2 * electionTimeout; i++) {
      r->Tick();
    }

    ASSERT_EQ(r->currentTerm_, 2);
    ASSERT_EQ(r->role_, Raft::kCandidate);

    // vote for self
    ASSERT_EQ(r->votedFor_, r->id_);
    ASSERT_TRUE(r->voteGranted_[r->id_]);

    std::unordered_set<std::string> s1;
    std::for_each(r->mails_.begin(), r->mails_.end(),
                  [&](const pb::Message& m) { s1.insert(DumpPB(m)); });

    std::unordered_set<std::string> s2;
    s2.insert(DumpPB(PBMessage().From(1).To(2).Term(2).LogTerm(0).Index(0).Type(pb::MsgVote).v));
    s2.insert(DumpPB(PBMessage().From(1).To(3).Term(2).LogTerm(0).Index(0).Type(pb::MsgVote).v));
    ASSERT_EQ(s1, s2);
  }

  // TestVoter tests the voter denies its vote if its own log is more up-to-date
  // than that of the candidate.
  // Reference: section 5.4.1
  static void TestVoter() {
    struct TestData {
      EntryVec ents;
      uint64_t index;
      uint64_t logterm;

      bool wreject;
    } tests[] = {
        // same logterm
        {{pbEntry(1, 1)}, 1, 1, false},
        {{pbEntry(1, 1)}, 2, 1, false},
        {{pbEntry(1, 1), pbEntry(2, 1)}, 1, 1, true},
        {{pbEntry(1, 1), pbEntry(2, 1)}, 2, 1, false},

        // candidate higher logterm
        {{pbEntry(1, 1)}, 1, 2, false},
        {{pbEntry(1, 1)}, 2, 2, false},
        {{pbEntry(1, 1), pbEntry(2, 1)}, 1, 2, false},

        // voter higher logterm
        {{pbEntry(1, 2)}, 1, 1, true},
        {{pbEntry(1, 2)}, 2, 1, true},
        {{pbEntry(1, 1), pbEntry(2, 2)}, 1, 1, true},
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage(t.ents)));
      r->Step(
          PBMessage().From(2).To(1).Type(pb::MsgVote).Term(3).LogTerm(t.logterm).Index(t.index).v);

      ASSERT_EQ(r->mails_.size(), 1);
      auto& m = r->mails_[0];
      ASSERT_EQ(m.type(), pb::MsgVoteResp);
      ASSERT_EQ(m.reject(), t.wreject);
    }
  }

  // TestLeaderOnlyCommitsLogFromCurrentTerm tests that only log entries from the leader’s
  // current term are committed by counting replicas.
  // Reference: section 5.4.2
  static void TestLeaderOnlyCommitsLogFromCurrentTerm() {
    struct TestData {
      uint64_t index;

      uint64_t wcommit;
    } tests[] = {
        // do not commit log entries in previous terms
        {1, 0},
        {2, 0},
        // commit log in current term
        {3, 3},
    };

    for (auto t : tests) {
      auto memstore = new MemoryStorage({pbEntry(1, 1), pbEntry(2, 2)});
      RaftUPtr r(newTestRaft(1, {1, 2}, 10, 1, memstore));
      r->loadState(PBHardState().Term(2).v);

      // become leader at term 3
      r->becomeCandidate();
      r->becomeLeader();
      ASSERT_EQ(r->prs_[1].MatchIndex(), 2);
      ASSERT_EQ(r->currentTerm_, 3);

      // append a empty entry with index = 3
      r->Step(PBMessage()
                  .From(1)
                  .To(1)
                  .Type(pb::MsgProp)
                  .Term(r->currentTerm_)
                  .Entries({pb::Entry()})
                  .v);
      ASSERT_EQ(r->prs_[1].MatchIndex(), 3);

      r->Step(
          PBMessage().From(2).To(1).Type(pb::MsgAppResp).Term(r->currentTerm_).Index(t.index).v);

      ASSERT_EQ(r->log_->CommitIndex(), t.wcommit);
    }
  }

  // TestVoteRequest tests that the vote request includes information about the candidate’s log
  // and are sent to all of the other nodes.
  // Reference: section 5.4.1
  static void TestVoteRequest() {
    struct TestData {
      EntryVec ents;
      uint64_t wterm;
    } tests[] = {
        // {{pbEntry(1, 1)}, 2},
        {{pbEntry(1, 1), pbEntry(2, 2)}, 3},
    };
    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));

      // receiving MsgApp from higher term,
      // r will convert to follower and set currentTerm to t.wterm - 1
      // and append t.ents to log.
      r->Step(PBMessage()
                  .From(2)
                  .To(1)
                  .Type(pb::MsgApp)
                  .Term(t.wterm - 1)
                  .LogTerm(0)
                  .Index(0)
                  .Entries(t.ents)
                  .v);
      r->mails_.clear();

      for (int i = 0; i < r->c_->electionTick * 2 - 1; i++) {
        r->Tick();
      }

      ASSERT_EQ(r->mails_.size(), 2);

      uint64_t wlogterm = t.ents.rbegin()->term();
      uint64_t windex = t.ents.rbegin()->index();
      for (int i = 0; i < 2; i++) {
        ASSERT_EQ(r->mails_[i].type(), pb::MsgVote);
        ASSERT_EQ(r->mails_[i].term(), t.wterm);

        ASSERT_EQ(r->mails_[i].logterm(), wlogterm);
        ASSERT_EQ(r->mails_[i].index(), windex);
        ASSERT_EQ(r->mails_[i].term(), t.wterm);
      }

      std::set<uint64_t> to{r->mails_[0].to(), r->mails_[1].to()};
      ASSERT_EQ(to, std::set<uint64_t>({2, 3}));
    }
  }

  // TestFollowerAppendEntries tests that when AppendEntries RPC is valid,
  // the follower will delete the existing conflict entry and all that follow it,
  // and append any new entries not already in the log.
  // Also, it writes the new entry into stable storage.
  // Reference: section 5.3
  static void TestFollowerAppendEntries() {
    // This test actually repeats Raft.HandleAppendEntries.

    struct TestData {
      uint64_t index, term;
      EntryVec ents;

      EntryVec wents;
      EntryVec wunstable;
    } tests[] = {
        {2, 2, {pbEntry(3, 3)}, {pbEntry(1, 1), pbEntry(2, 2), pbEntry(3, 3)}, {pbEntry(3, 3)}},
        {
            1,
            1,
            {pbEntry(2, 3), pbEntry(3, 4)},
            {pbEntry(1, 1), pbEntry(2, 3), pbEntry(3, 4)},
            {pbEntry(2, 3), pbEntry(3, 4)},
        },
        {0, 0, {pbEntry(1, 1)}, {pbEntry(1, 1), pbEntry(2, 2)}},
        {0, 0, {pbEntry(1, 3)}, {pbEntry(1, 3)}, {pbEntry(1, 3)}},
    };

    for (auto t : tests) {
      RaftUPtr r(
          newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage({pbEntry(1, 1), pbEntry(2, 2)})));
      r->becomeFollower(2, 2);
      r->Step(PBMessage()
                  .From(2)
                  .To(1)
                  .Type(pb::MsgApp)
                  .Term(2)
                  .LogTerm(t.term)
                  .Index(t.index)
                  .Entries(t.ents)
                  .v);

      EntryVec_ASSERT_EQ(r->log_->AllEntries(), t.wents);
      EntryVec_ASSERT_EQ(r->log_->GetUnstable().entries, t.wunstable);
    }
  }

  // TestLeaderCommitPrecedingEntries tests that when leader commits a log entry,
  // it also commits all preceding entries in the leader’s log, including
  // entries created by previous leaders.
  // Also, it applies the entry to its local state machine (in log order).
  // Reference: section 5.3
  static void TestLeaderCommitPrecedingEntries() {
    struct TestData {
      EntryVec ents;
    } tests[] = {
        {}, {{pbEntry(1, 2)}}, {{pbEntry(1, 1), pbEntry(2, 2)}}, {{pbEntry(1, 1)}},
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage(t.ents)));
      r->loadState(PBHardState().Term(2).v);
      r->becomeCandidate();
      r->becomeLeader();

      auto m = PBMessage()
                   .From(1)
                   .To(1)
                   .Type(pb::MsgProp)
                   .Term(r->currentTerm_)
                   .Entries({PBEntry().Data("some data").v})
                   .v;
      r->Step(m);

      auto apps = r->mails_;
      r->mails_.clear();
      for (auto msg : apps) {
        auto resp = replyMsgApp(msg);
        r->Step(resp);
      }

      uint64_t lastIdx = r->c_->storage->LastIndex().GetValue();
      EntryVec expect = t.ents;
      expect.push_back(PBEntry().Term(3).Index(lastIdx + 1).Data("some data").v);

      EntryVec actual = t.ents;
      EntryVec& unstable = r->log_->GetUnstable().entries;
      std::copy(unstable.begin(), unstable.end(), std::back_inserter(actual));

      EntryVec_ASSERT_EQ(actual, expect);
    }
  }

  // TestLeaderAcknowledgeCommit tests that a log entry is committed once the
  // leader that created the entry has replicated it on a majority of the servers.
  // Reference: section 5.3
  static void TestLeaderAcknowledgeCommit() {
    struct TestData {
      size_t size;
      std::set<uint64_t> acceptors;

      bool wack;
    } tests[] = {
        {1, {}, true},     {3, {}, false},       {3, {2}, true},
        {3, {2, 3}, true}, {5, {}, false},       {5, {2}, false},
        {5, {2, 3}, true}, {5, {2, 3, 4}, true}, {5, {2, 3, 4, 5}, true},
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, idsBySize(t.size), 10, 1, new MemoryStorage()));
      r->becomeCandidate();
      r->becomeLeader();
      r->mails_.clear();

      r->Step(PBMessage()
                  .From(1)
                  .To(1)
                  .Type(pb::MsgProp)
                  .Term(r->currentTerm_)
                  .Entries({PBEntry().Data("some data").v})
                  .v);
      auto apps = r->mails_;
      r->mails_.clear();

      for (auto& m : apps) {
        if (t.acceptors.find(m.to()) != t.acceptors.end()) {
          auto resp = replyMsgApp(m);
          r->Step(resp);
        }
      }

      uint64_t li = r->log_->LastIndex();
      bool ack = (r->log_->CommitIndex() >= 1);
      ASSERT_EQ(ack, t.wack);
    }
  }

  // TestCandidateFallback tests that while waiting for votes,
  // if a candidate receives an AppendEntries RPC from another server claiming
  // to be leader whose term is at least as large as the candidate's current term,
  // it recognizes the leader as legitimate and returns to follower state.
  // Reference: section 5.2
  static void TestCandidateFallback() {
    struct TestData {
      uint64_t term;
    } tests[] = {
        1, 2,
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));
      r->Step(PBMessage().From(1).To(1).Type(pb::MsgHup).v);
      ASSERT_EQ(r->role_, Raft::kCandidate);
      ASSERT_EQ(r->currentTerm_, 1);

      r->Step(PBMessage().From(2).To(1).Term(t.term).Type(pb::MsgApp).v);
      ASSERT_EQ(r->role_, Raft::kFollower);
      ASSERT_EQ(r->currentTerm_, t.term);
    }
  }

  // TestLeaderStartReplication tests that when receiving client proposals,
  // the leader appends the proposal to its log as a new entry, then issues
  // AppendEntries RPCs in parallel to each of the other servers to replicate
  // the entry. Also, when sending an AppendEntries RPC, the leader includes
  // the index and term of the entry in its log that immediately precedes
  // the new entries.
  // Also, it writes the new entry into stable storage.
  // Reference: section 5.3
  static void TestLeaderStartReplication() {
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));
    r->becomeCandidate();
    r->becomeLeader();
    ASSERT_EQ(r->currentTerm_, 1);

    // clean up noop entry generated when leader elected
    r->mails_.clear();
    r->log_->GetUnstable().entries.clear();

    auto ents = {PBEntry().Data("some data").v};
    uint64_t li = r->log_->LastIndex();
    r->Step(PBMessage().From(1).To(1).Term(1).Type(pb::MsgProp).Entries(ents).v);
    ASSERT_EQ(r->log_->LastIndex(), li + 1);
    ASSERT_EQ(r->log_->CommitIndex(), li);

    EntryVec wents({PBEntry().Term(1).Index(li + 1).Data("some data").v});
    EntryVec_ASSERT_EQ(r->log_->GetUnstable().entries, wents);

    std::unordered_set<std::string> s1;
    std::for_each(r->mails_.begin(), r->mails_.end(),
                  [&](const pb::Message& m) { s1.insert(DumpPB(m)); });

    auto msg =
        PBMessage().Index(li).LogTerm(0).From(1).Type(pb::MsgApp).Entries(wents).Term(1).Commit(li);
    std::unordered_set<std::string> s2;
    s2.insert(DumpPB(msg.To(2).v));
    s2.insert(DumpPB(msg.To(3).v));

    ASSERT_EQ(s1, s2);
  }

  // TestLeaderCommitEntry tests that when the entry has been safely replicated,
  // the leader gives out the applied entries, which can be applied to its state
  // machine.
  // Also, the leader keeps track of the highest index it knows to be committed,
  // and it includes that index in future AppendEntries RPCs so that the other
  // servers eventually find out.
  // Reference: section 5.3
  static void TestLeaderCommitEntry() {
    int heartbeatTimeout = 3;
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, heartbeatTimeout, new MemoryStorage()));
    r->becomeCandidate();
    r->becomeLeader();

    // clean up noop entry generated when leader elected
    r->mails_.clear();
    r->log_->GetUnstable().entries.clear();

    uint64_t li = r->log_->LastIndex();
    auto ents = {PBEntry().Data("some data").v};
    r->Step(PBMessage().From(1).To(1).Term(1).Type(pb::MsgProp).Entries(ents).v);

    auto apps = r->mails_;
    r->mails_.clear();
    for (auto& m : apps) {
      auto resp = replyMsgApp(m);
      r->Step(resp);
    }

    ASSERT_EQ(r->log_->CommitIndex(), li + 1);

    for (int i = 0; i < heartbeatTimeout; i++) {
      r->Tick();
    }

    ASSERT_EQ(r->mails_.size(), 2);
    for (auto m : r->mails_) {
      ASSERT_EQ(m.type(), pb::MsgHeartbeat);
      ASSERT_EQ(m.commit(), li + 1);
    }
  }

  // TestFollowerCheckMsgApp tests that if the follower does not find an
  // entry in its log with the same index and term as the one in AppendEntries RPC,
  // then it refuses the new entries. Otherwise it replies that it accepts the
  // append entries.
  // Reference: section 5.3
  static void TestFollowerCheckMsgApp() {
    struct TestData {
      uint64_t prevLogTerm;
      uint64_t prevLogIndex;

      bool wreject;
    } tests[] = {
        {0, 0, false}, {1, 1, false}, {2, 2, false}, {1, 2, true}, {3, 3, true},
    };

    for (auto t : tests) {
      RaftUPtr r(
          newTestRaft(1, {1, 2, 3}, 10, 3, new MemoryStorage({pbEntry(1, 1), pbEntry(2, 2)})));
      r->becomeFollower(2, 2);
      r->Step(PBMessage()
                  .Type(pb::MsgApp)
                  .Index(t.prevLogIndex)
                  .LogTerm(t.prevLogTerm)
                  .Term(2)
                  .From(2)
                  .To(1)
                  .v);

      ASSERT_EQ(r->mails_.size(), 1);

      auto resp = r->mails_[0];
      ASSERT_EQ(resp.type(), pb::MsgAppResp);
      ASSERT_EQ(resp.from(), 1);
      ASSERT_EQ(resp.to(), 2);
      ASSERT_EQ(resp.reject(), t.wreject);
      ASSERT_EQ(resp.term(), 2);
    }
  }

  // TestLeaderElectionInOneRoundRPC tests all cases that may happen in
  // leader election during one round of RequestVote RPC:
  // a) it wins the election
  // b) another server establishes itself as leader
  // c) a period of time goes by with no winner
  // Reference: section 5.2
  static void TestLeaderElectionInOneRoundRPC() {
    struct TestData {
      size_t size;
      std::set<uint64_t> acceptors;
      std::set<uint64_t> rejected;

      Raft::StateRole wstate;
    } tests[] = {
        // win the election when receiving votes from a majority of the servers
        {1, {}, {}, Raft::kLeader},
        {3, {2, 3}, {}, Raft::kLeader},
        {3, {2}, {}, Raft::kLeader},
        {5, {2, 3, 4, 5}, {}, Raft::kLeader},
        {5, {2, 3, 4}, {}, Raft::kLeader},
        {5, {2, 3}, {}, Raft::kLeader},

        // return to follower state if it receives vote denial from a majority
        {3, {}, {2, 3}, Raft::kFollower},
        {5, {}, {2, 3, 4, 5}, Raft::kFollower},
        {5, {2}, {3, 4, 5}, Raft::kFollower},

        // stay in candidate if it does not obtain the majority
        {3, {}, {}, Raft::kCandidate},
        {5, {2}, {}, Raft::kCandidate},
        {5, {}, {2, 3}, Raft::kCandidate},
        {5, {}, {}, Raft::kCandidate},
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, idsBySize(t.size), 10, 1, new MemoryStorage()));

      r->Step(PBMessage().From(1).To(1).Term(r->currentTerm_).Type(pb::MsgHup).v);
      ASSERT_EQ(r->currentTerm_, 1);

      for (uint64_t id = 2; id <= t.size; id++) {
        auto m = PBMessage().From(id).To(1).Type(pb::MsgVoteResp).Term(r->currentTerm_);

        bool accept = (t.acceptors.find(id) != t.acceptors.end());
        if (accept) {
          r->Step(m.v);
        }

        bool reject = (t.rejected.find(id) != t.rejected.end());
        if (reject) {
          r->Step(m.Reject(true).v);
        }
      }

      ASSERT_EQ(r->role_, t.wstate);
    }
  }

  // TestLeaderSyncFollowerLog tests that the leader could bring a follower's log
  // into consistency with its own.
  // Reference: section 5.3, figure 7
  static void TestLeaderSyncFollowerLog() {
    struct TestData {
      EntryVec ents;
    } tests[] = {
        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4), pbEntry(5, 4), pbEntry(6, 5),
          pbEntry(7, 5), pbEntry(8, 6), pbEntry(9, 6)}},

        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4)}},

        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4), pbEntry(5, 4), pbEntry(6, 5),
          pbEntry(7, 5), pbEntry(8, 6), pbEntry(9, 6), pbEntry(10, 6), pbEntry(11, 6)}},

        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4), pbEntry(5, 4), pbEntry(6, 5),
          pbEntry(7, 5), pbEntry(8, 6), pbEntry(9, 6), pbEntry(10, 6), pbEntry(11, 7),
          pbEntry(12, 7)}},

        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4), pbEntry(5, 4), pbEntry(6, 4),
          pbEntry(7, 4)}},

        {{pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 2), pbEntry(5, 2), pbEntry(6, 2),
          pbEntry(7, 3), pbEntry(8, 3), pbEntry(9, 3), pbEntry(10, 3), pbEntry(11, 3)}},
    };

    for (auto t : tests) {
      auto lead = newTestRaft(
          1, {1, 2, 3}, 10, 1,
          new MemoryStorage({
              pbEntry(1, 1), pbEntry(2, 1), pbEntry(3, 1), pbEntry(4, 4), pbEntry(5, 4),
              pbEntry(6, 5), pbEntry(7, 5), pbEntry(8, 6), pbEntry(9, 6), pbEntry(10, 6),
          }));
      lead->loadState(PBHardState().Commit(lead->log_->LastIndex()).Term(8).v);

      auto follower = newTestRaft(2, {1, 2, 3}, 10, 1, new MemoryStorage(t.ents));
      follower->loadState(PBHardState().Term(7).v);

      // It is necessary to have a three-node cluster.
      // The second may have more up-to-date log than the first one, so the
      // first node needs the vote from the third node to become the leader.
      auto follower3 = newTestRaft(3, {1, 2, 3}, 10, 1, new MemoryStorage);
      std::unique_ptr<Network> net(Network::New(3)->Set(lead)->Set(follower)->Set(follower3));

      // Elect 1 as leader.
      net->StartElection(1);
      ASSERT_EQ(net->Peer(1)->role_, Raft::kLeader);

      net->Propose(1);

      ASSERT_EQ(net->Peer(1)->log_->LastIndex(), 12);
      ASSERT_EQ(net->Peer(3)->log_->LastIndex(), 12);
      ASSERT_EQ(net->Peer(1)->log_->CommitIndex(), 12);

      auto followerEnts = follower->log_->AllEntries();
      auto leadEnts = lead->log_->AllEntries();
      EntryVec_ASSERT_EQ(followerEnts, leadEnts);
    }
  }

  // TestVoteFromCandidateWithDifferentTerm tests each server will vote for
  // at most one candidate in a given term, on a first-come-first-served basis.
  static void TestVoteFromCandidateWithDifferentTerm() {
    RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));
    r->Step(PBMessage().From(2).To(1).Type(pb::MsgVote).Term(3).v);
    ASSERT_EQ(r->votedFor_, 2);
    ASSERT_EQ(r->mails_.size(), 1);
    ASSERT_EQ(DumpPB(r->mails_[0]),
              DumpPB(PBMessage().From(1).To(2).Type(pb::MsgVoteResp).Term(3).Reject(false).v));
    r->mails_.clear();

    r->Step(PBMessage().From(3).To(1).Type(pb::MsgVote).Term(3).v);
    ASSERT_EQ(r->votedFor_, 2);
    ASSERT_EQ(r->mails_.size(), 1);
    ASSERT_EQ(DumpPB(r->mails_[0]),
              DumpPB(PBMessage().From(1).To(3).Type(pb::MsgVoteResp).Term(3).Reject(true).v));
    r->mails_.clear();

    r->Step(PBMessage().From(3).To(1).Type(pb::MsgVote).Term(4).v);
    ASSERT_EQ(r->votedFor_, 3);
    ASSERT_EQ(r->mails_.size(), 1);
    ASSERT_EQ(DumpPB(r->mails_[0]),
              DumpPB(PBMessage().From(1).To(3).Type(pb::MsgVoteResp).Term(4).Reject(false).v));
    r->mails_.clear();

    r->Step(PBMessage().From(2).To(1).Type(pb::MsgVote).Term(4).v);
    ASSERT_EQ(r->votedFor_, 3);
    ASSERT_EQ(r->mails_.size(), 1);
    ASSERT_EQ(DumpPB(r->mails_[0]),
              DumpPB(PBMessage().From(1).To(2).Type(pb::MsgVoteResp).Term(4).Reject(true).v));
    r->mails_.clear();

    r->Step(PBMessage().From(2).To(1).Type(pb::MsgVote).Term(5).v);
    ASSERT_EQ(r->votedFor_, 2);
    ASSERT_EQ(r->mails_.size(), 1);
    ASSERT_EQ(DumpPB(r->mails_[0]),
              DumpPB(PBMessage().From(1).To(2).Type(pb::MsgVoteResp).Term(5).Reject(false).v));
    r->mails_.clear();
  }

  static pb::Message replyMsgApp(pb::Message m) {
    return PBMessage()
        .From(m.to())
        .To(m.from())
        .Type(pb::MsgAppResp)
        .Index(m.index() + m.entries_size())
        .Term(m.term())
        .v;
  }

  static std::vector<uint64_t> idsBySize(size_t size) {
    std::vector<uint64_t> ids(size);
    int n = 1;
    std::generate(ids.begin(), ids.end(), [&n] { return n++; });
    return ids;
  }
};

}  // namespace yaraft

using namespace yaraft;

TEST_F(RaftPaperTest, FollowerUpdateTermFromMessage) {
  RaftPaperTest::TestUpdateTermFromMessage(Raft::kFollower);
}

TEST_F(RaftPaperTest, CandidateUpdateTermFromMessage) {
  RaftPaperTest::TestUpdateTermFromMessage(Raft::kCandidate);
}

TEST_F(RaftPaperTest, LeaderUpdateTermFromMessage) {
  RaftPaperTest::TestUpdateTermFromMessage(Raft::kLeader);
}

TEST_F(RaftPaperTest, StartAsFollower) {
  RaftPaperTest::TestStartAsFollower();
}

TEST_F(RaftPaperTest, LeaderBcastBeat) {
  RaftPaperTest::TestLeaderBcastBeat();
}

TEST_F(RaftPaperTest, FollowerStartElection) {
  RaftPaperTest::TestNonleaderStartElection(Raft::kFollower);
}

TEST_F(RaftPaperTest, CandidateStartNewElection) {
  RaftPaperTest::TestNonleaderStartElection(Raft::kCandidate);
}

TEST_F(RaftPaperTest, Voter) {
  RaftPaperTest::TestVoter();
}

TEST_F(RaftPaperTest, LeaderOnlyCommitsLogFromCurrentTerm) {
  RaftPaperTest::TestLeaderOnlyCommitsLogFromCurrentTerm();
}

TEST_F(RaftPaperTest, VoteRequest) {
  RaftPaperTest::TestVoteRequest();
}

TEST_F(RaftPaperTest, FollowerAppendEntries) {
  RaftPaperTest::TestFollowerAppendEntries();
}

TEST_F(RaftPaperTest, LeaderCommitPrecedingEntries) {
  RaftPaperTest::TestLeaderCommitPrecedingEntries();
}

TEST_F(RaftPaperTest, LeaderAcknowledgeCommit) {
  RaftPaperTest::TestLeaderAcknowledgeCommit();
}

TEST_F(RaftPaperTest, CandidateFallback) {
  RaftPaperTest::TestCandidateFallback();
}

TEST_F(RaftPaperTest, LeaderStartReplication) {
  RaftPaperTest::TestLeaderStartReplication();
}

TEST_F(RaftPaperTest, LeaderCommitEntry) {
  RaftPaperTest::TestLeaderCommitEntry();
}

TEST_F(RaftPaperTest, FollowerCheckMsgApp) {
  RaftPaperTest::TestFollowerCheckMsgApp();
}

TEST_F(RaftPaperTest, LeaderElectionInOneRoundRPC) {
  RaftPaperTest::TestLeaderElectionInOneRoundRPC();
}

TEST_F(RaftPaperTest, LeaderSyncFollowerLog) {
  RaftPaperTest::TestLeaderSyncFollowerLog();
}

TEST_F(RaftPaperTest, VoteFromCandidateWithDifferentTerm) {
  RaftPaperTest::TestVoteFromCandidateWithDifferentTerm();
}